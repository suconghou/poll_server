#include "poll.cpp"
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class RedisServer
{
    using self = RedisServer;

private:
    using CommandHandler = void (self::*)(poll_server &, int, const std::vector<std::string> &);
    std::unordered_map<std::string, CommandHandler> command_handlers;
    std::unordered_map<std::string, std::string> db; // 存储键值对
    std::unordered_map<int, std::string> clients;

    // 解析 RESP 协议中的单个命令
    std::optional<std::vector<std::string>> parse_command(const std::string &buffer, size_t &parsed_len)
    {
        parsed_len = 0;
        if (buffer.empty() || buffer[0] != '*')
        {
            return std::nullopt;
        }
        size_t pos = 1;
        // 解析数组长度
        size_t array_len = 0;
        while (pos < buffer.size() && buffer[pos] != '\r')
        {
            if (!isdigit(buffer[pos]))
            {
                return std::nullopt;
            }
            array_len = array_len * 10 + (buffer[pos] - '0');
            pos++;
        }
        if (pos + 1 >= buffer.size() || buffer[pos] != '\r' || buffer[++pos] != '\n')
        {
            return std::nullopt;
        }
        pos++;
        std::vector<std::string> args;
        for (size_t i = 0; i < array_len; ++i)
        {
            if (buffer[pos] != '$')
            {
                return std::nullopt;
            }
            pos++;
            // 解析批量字符串长度
            size_t bulk_len = 0;
            while (pos < buffer.size() && buffer[pos] != '\r')
            {
                if (!isdigit(buffer[pos]))
                {
                    return std::nullopt;
                }
                bulk_len = bulk_len * 10 + (buffer[pos] - '0');
                pos++;
            }
            if (pos + 1 >= buffer.size() || buffer[pos] != '\r' || buffer[++pos] != '\n')
            {
                return std::nullopt;
            }
            pos++;
            if (pos + bulk_len + 2 > buffer.size())
            {
                return std::nullopt;
            }
            args.emplace_back(buffer.data() + pos, bulk_len);
            pos += bulk_len + 2; // 跳过 \r\n
        }
        parsed_len = pos;
        return args;
    }

    // 处理客户端命令
    void process_command(poll_server &server, int fd, const std::vector<std::string> &args)
    {
        if (args.empty())
        {
            send_response(server, fd, "-ERR invalid command\r\n");
            return;
        }
        std::string cmd = args[0];
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
        auto handler = command_handlers.find(cmd);
        if (handler != command_handlers.end())
        {
            (this->*(handler->second))(server, fd, args);
        }
        else
        {
            send_error(server, fd, "unknown command '" + cmd + "'");
        }
    }

    // 处理 GET 命令
    void handle_get(poll_server &server, int fd, const std::vector<std::string> &args)
    {
        if (args.size() != 2)
        {
            send_error(server, fd, "wrong number of arguments for 'GET'");
            return;
        }
        auto it = db.find(args[1]);
        if (it != db.end())
        {
            std::string resp = "$" + std::to_string(it->second.size()) + "\r\n" + it->second + "\r\n";
            send_response(server, fd, resp);
        }
        else
        {
            send_response(server, fd, "$-1\r\n");
        }
    }

    // 处理 SET 命令
    void handle_set(poll_server &server, int fd, const std::vector<std::string> &args)
    {
        if (args.size() != 3)
        {
            send_error(server, fd, "wrong number of arguments for 'SET'");
            return;
        }
        db[args[1]] = args[2];
        send_response(server, fd, "+OK\r\n");
    }

    // 处理 SETNX 命令
    void handle_setnx(poll_server &server, int fd, const std::vector<std::string> &args)
    {
        if (args.size() != 3)
        {
            send_error(server, fd, "wrong number of arguments for 'SETNX'");
            return;
        }
        auto it = db.find(args[1]);
        if (it == db.end())
        {
            db[args[1]] = args[2];
            send_response(server, fd, ":1\r\n");
        }
        else
        {
            send_response(server, fd, ":0\r\n");
        }
    }

    // 处理 DEL 命令
    void handle_del(poll_server &server, int fd, const std::vector<std::string> &args)
    {
        if (args.size() < 2)
        {
            send_error(server, fd, "wrong number of arguments for 'DEL'");
            return;
        }
        size_t total_deleted = 0;
        // 从索引1开始处理所有的键（跳过命令名称）
        for (size_t i = 1; i < args.size(); i++)
        {
            total_deleted += db.erase(args[i]);
        }
        send_response(server, fd, ":" + std::to_string(total_deleted) + "\r\n");
    }

    // 处理 INCR/INCRBY 命令
    void handle_incr(poll_server &server, int fd, const std::vector<std::string> &args)
    {
        std::string cmd = args[0];
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
        // 添加参数数量检查
        if ((cmd == "INCR" && args.size() != 2) || (cmd == "INCRBY" && args.size() != 3))
        {
            send_error(server, fd, "wrong number of arguments for '" + cmd + "'");
            return;
        }
        auto increment = (cmd == "INCR") ? std::optional<int64_t>(1) : parse_increment(args);
        if (!increment)
        {
            send_error(server, fd, "invalid increment value");
            return;
        }
        process_incrby(server, fd, args[1], *increment);
    }

    // 处理 INCRBY 核心逻辑
    void process_incrby(poll_server &server, int fd, const std::string &key, int64_t increment)
    {
        auto value_opt = get_and_validate_int(key);
        if (!value_opt && db.find(key) != db.end())
        {
            send_error(server, fd, "value is not an integer or out of range");
            return;
        }
        int64_t value = value_opt.value_or(0);
        if ((increment > 0 && value > INT64_MAX - increment) || (increment < 0 && value < INT64_MIN - increment))
        {
            send_error(server, fd, "increment would overflow");
            return;
        }
        value += increment;
        db[key] = std::to_string(value);
        send_response(server, fd, ":" + std::to_string(value) + "\r\n");
    }

    // 处理 INFO 命令
    void handle_info(poll_server &server, int fd, const std::vector<std::string> &args)
    {
        if (args.size() != 1)
        {
            send_error(server, fd, "wrong number of arguments for 'INFO'");
            return;
        }
        std::string info_str = generate_info_response();
        std::string resp = "$" + std::to_string(info_str.size()) + "\r\n" + info_str + "\r\n";
        send_response(server, fd, resp);
    }

    void handle_ping(poll_server &server, int fd, const std::vector<std::string> &args)
    {
        if (args.size() > 2)
        {
            send_error(server, fd, "wrong number of arguments for 'PING'");
            return;
        }
        if (args.size() == 2)
        {
            // 如果提供了参数，返回该参数
            std::string resp = "$" + std::to_string(args[1].size()) + "\r\n" + args[1] + "\r\n";
            send_response(server, fd, resp);
        }
        else
        {
            // 无参数时返回 PONG
            send_response(server, fd, "+PONG\r\n");
        }
    }

    // 生成 INFO 响应内容
    std::string generate_info_response()
    {
        std::ostringstream oss;
        oss << "keys:" << db.size() << "\r\n";
        oss << "clients:" << clients.size() << "\r\n";
        return oss.str();
    }

    // 解析 INCRBY 增量值
    std::optional<int64_t> parse_increment(const std::vector<std::string> &args)
    {
        if (args.size() != 3)
        {
            return std::nullopt;
        }
        try
        {
            return std::stoll(args[2]);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    // 验证键值是否为整数
    std::optional<int64_t> get_and_validate_int(const std::string &key)
    {
        auto it = db.find(key);
        if (it == db.end())
        {
            return 0;
        }
        try
        {
            size_t pos;
            int64_t value = std::stoll(it->second, &pos);
            if (pos != it->second.size())
            {
                return std::nullopt;
            }
            return value;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    // 发送响应
    void send_response(poll_server &server, int fd, const std::string &resp)
    {
        server.write(fd, resp.c_str(), resp.size());
    }

    // 统一错误响应
    void send_error(poll_server &server, int fd, const std::string &msg)
    {
        send_response(server, fd, "-ERR " + msg + "\r\n");
    }

public:
    RedisServer()
    {
        command_handlers.emplace("GET", &self::handle_get);
        command_handlers.emplace("SET", &self::handle_set);
        command_handlers.emplace("SETNX", &self::handle_setnx);
        command_handlers.emplace("DEL", &self::handle_del);
        command_handlers.emplace("INCR", &self::handle_incr);
        command_handlers.emplace("INCRBY", &self::handle_incr);
        command_handlers.emplace("INFO", &self::handle_info);
        command_handlers.emplace("PING", &self::handle_ping);
    }
    void run(int port)
    {
        auto on_loop = [](poll_server &, int)
        {
            return 1000;
        };
        auto on_open = [this](poll_server &, int fd)
        {
            if (fd > 0)
            {
                clients[fd] = "";
            }
        };
        auto on_data = [this](poll_server &s, int fd, const char *data, int len)
        {
            if (len > 0)
            {
                auto &buffer = clients.at(fd);
                // 添加缓冲区大小检查
                if (buffer.size() + len > 1024 * 1024)
                {
                    clients.erase(fd);
                    s.closefd(fd);
                    return;
                }
                buffer.append(data, len);
                size_t parsed = 0;
                while (parsed < buffer.size())
                {
                    auto cmd = parse_command(buffer, parsed);
                    if (!cmd)
                    {
                        break;
                    }
                    process_command(s, fd, *cmd);
                }
                if (parsed > 0)
                {
                    buffer.erase(0, parsed);
                }
            }
            else
            {
                clients.erase(fd);
                s.closefd(fd);
            }
        };
        poll_server server(on_loop, on_open, on_data);
        server.start(port);
    }
};

int main()
{
    RedisServer redis;
    redis.run(6479);
    return 0;
}