#pragma once
#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <ctype.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

class Exception : public std::exception
{
public:
    Exception() : pMessage("") {}
    Exception(const char *pStr) : pMessage(pStr) {}
    const char *what() const throw() { return pMessage; }

private:
    const char *pMessage;
};

class poll_server
{
    using self = poll_server;

    struct WriteRequest
    {
        std::string data;                               // 要写入的数据
        std::function<void(self &, int, int)> callback; // 回调函数,参数2:fd，参数3:发送的字节数，负数则为失败，是0表示对方已关闭了链接
        int out_bytes;                                  // 数据发送计数器，分片发送时，最后一次成功回调需要
    };

    struct connection
    {
        pollfd info;
        std::queue<WriteRequest> out;
        bool write_closed = false; // 标记对端是否关闭写端
    };

private:
    std::unordered_map<int, connection> connections;
    std::function<int(self &, int)> OnLoop;
    std::function<void(self &, int)> OnOpen;
    std::function<void(self &, int, const char *, int)> OnData;
    int startup(int port, int backlog = 128, const char *host = "")
    {
        int httpd = socket(AF_INET, SOCK_STREAM, 0);
        if (httpd < 0)
        {
            throw Exception(strerror(errno));
        }

        if (set_noblocking(httpd) != 0)
        {
            throw Exception(strerror(errno));
        }

        if (set_reuse_port(httpd) != 0)
        {
            throw Exception(strerror(errno));
        }

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        serv_addr.sin_addr.s_addr = strlen(host) < 1 ? INADDR_ANY : inet_addr(host);
        if (bind(httpd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0)
        {
            close(httpd);
            throw Exception(strerror(errno));
        }
        if (listen(httpd, backlog) < 0)
        {
            close(httpd);
            throw Exception(strerror(errno));
        }
        return httpd;
    }

    inline int set_noblocking(int sockfd) const
    {
        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags == -1)
        {
            return -1;
        }
        if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            return -2;
        }
        return 0;
    }

    inline int set_reuse_port(int sockfd) const
    {
        int opt = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        {
            return -1;
        }
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        {
            return -2;
        }
        return 0;
    }

    // 关闭指定的fd, 并执行回调, 如果已经关闭过，则忽略，err>0 时不执行回调
    bool closefd(int fd, int err)
    {
        if (connections.erase(fd) > 0)
        {
            if (err <= 0)
            {
                OnData(*this, fd, nullptr, err);
            }
            return close(fd) == 0;
        }
        return false;
    }

public:
    poll_server(std::function<int(self &, int)> on_loop, std::function<void(self &, int)> on_open, std::function<void(self &, int, const char *, int)> on_data)
        : OnLoop(on_loop),
          OnOpen(on_open),
          OnData(on_data)
    {
    }
    // 返回值，已经入队的数量，当入队数量过多时，调用者需放缓以防止内存耗尽
    // 如果传入的fd不对，将抛出异常
    // 如果要发送的数据0字节，忽略发送请求，并且也没有回调函数
    int write(int fd, const char *data, int len, std::function<void(self &, int, int)> cb = nullptr)
    {
        return write(fd, std::string(data, len), cb);
    }
    // 返回值，已经入队的数量，当入队数量过多时，调用者需放缓以防止内存耗尽
    // 如果传入的fd不对，将抛出异常
    // 如果要发送的数据0字节，忽略发送请求，并且也没有回调函数
    int write(int fd, std::string data, std::function<void(self &, int, int)> cb = nullptr)
    {
        auto &c = connections.at(fd); // 如果传入的fd不对，此处抛出异常
        if (data.size() <= 0)
        {
            return c.out.size();
        }
        c.out.emplace(std::move(data), std::move(cb), 0);
        c.info.events |= POLLOUT;
        return c.out.size();
    }
    // 关闭指定的fd, 供外部主动调用, 如果已经关闭过，则忽略，调用后可能会触发关闭回调
    bool closefd(int fd)
    {
        return closefd(fd, 0);
    }

    bool start(int port, const char *host = "")
    {
        int backlog = 128;
        int server_sock = startup(port, backlog, host);
        if (server_sock < 1)
        {
            return false;
        }

        connections[server_sock] = {.info = {server_sock, POLLIN, 0}};

        struct sockaddr_in client_name;
        socklen_t client_name_len = sizeof(client_name);
        char buf[8192];
        std::vector<pollfd> pollfds; // 重新组织 pollfd 数组时使用的缓存，我们存放在上层服复用
        while (true)
        {
            auto cs = connections.size();
            auto n = OnLoop(*this, cs);
            if (n < 1) // OnLoop返回小于1代表意图停止服务
            {
                break;
            }
            // 重新组织 pollfd 数组, 此处有性能开销因此连接数也不应过大，即backlog变量一般应小于1024
            pollfds.clear();
            for (const auto &c : connections)
            {
                pollfds.emplace_back(c.second.info);
            }
            int num_fds = poll(pollfds.data(), pollfds.size(), n);
            // 返回值，正整数：就绪的文件描述符数量，0: 超时，-1: 错误，第三个参数配置的是超时时间
            if (num_fds < 1)
            {
                if (num_fds == 0) // 表明超时
                {
                    continue;
                }
                // 其他情况 poll 返回 -1 代表错误
                if (errno == EINTR)
                {
                    continue;
                }
                throw Exception(strerror(errno));
            }

            for (const auto &item : pollfds)
            {
                // 检查服务器套接字是否有新连接
                if (item.fd == server_sock)
                {
                    if (item.revents & POLLIN)
                    {
                        // 接受新连接
                        int client_sock = accept(server_sock, (struct sockaddr *)&client_name, &client_name_len);
                        if (client_sock < 1)
                        {
                            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                            {
                                continue;
                            }
                            throw Exception(strerror(errno));
                        }
                        if (set_noblocking(client_sock) != 0)
                        {
                            throw Exception(strerror(errno));
                        }
                        if ((int)pollfds.size() < backlog)
                        {
                            connections[client_sock] = {.info = {client_sock, POLLIN, 0}}; // POLLHUP无需设置，总是会自动报告POLLHUP事件，如果设置了POLLOUT，发送缓冲区一直有空间，会重复报告
                            OnOpen(*this, client_sock);
                        }
                        else
                        {
                            close(client_sock);
                            OnOpen(*this, -1);
                        }
                    }
                    // else 没有事件

                    // 服务器监听的socket，上面是处理逻辑，只需要处理POLLIN事件，处理完毕在此直接跳到下个循环
                    continue;
                }

                if (item.revents & POLLIN)
                {
                    int ret;
                    while ((ret = recv(item.fd, buf, sizeof(buf) - 1, 0)) > 0)
                    {
                        OnData(*this, item.fd, buf, ret);
                    }
                    if (ret == 0)
                    {
                        // 客户端关闭写端（半关闭状态）
                        auto &c = connections.at(item.fd); // 此处需要必然存在
                        c.write_closed = true;
                        c.info.events &= ~POLLIN;
                        if (c.out.empty())
                        {
                            closefd(item.fd, -10); // 队列为空，直接关闭
                        }
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                        {
                            // 没有数据可读，继续等待
                            continue;
                        }
                        closefd(item.fd, -4);
                    }
                }
                else if (item.revents & POLLOUT)
                {
                    auto &c = connections.at(item.fd); // 此处需要必然存在
                    auto &q = c.out;
                    if (!q.empty())
                    {
                        auto &r = q.front();
                        int bytesSent = send(item.fd, r.data.c_str() + r.out_bytes, r.data.size() - r.out_bytes, MSG_DONTWAIT | MSG_NOSIGNAL);
                        if (bytesSent < 0)
                        {
                            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                            {
                                continue; // 发送缓冲区满，稍后重试
                            }
                            // 处理 EPIPE 或其他错误 可以读取 strerror(errno)
                            q.pop();
                            c.info.events &= ~POLLOUT;
                            closefd(item.fd, -3);
                        }
                        else if (bytesSent > 0)
                        {
                            // 部分数据发送成功，可能需要稍后再试
                            r.out_bytes += bytesSent;
                            if (r.out_bytes == (int)r.data.size())
                            {
                                auto callback = std::move(r.callback); // 保存回调函数
                                auto sent_bytes = r.out_bytes;         // 保存发送的字节数
                                q.pop();                               // 发送完成，移除请求
                                if (callback)
                                {
                                    callback(*this, item.fd, sent_bytes);
                                }
                            }
                        }
                        else // bytesSent == 0（对方关闭连接）
                        {
                            q.pop();
                            c.info.events &= ~POLLOUT;
                            closefd(item.fd, -2);
                        }
                    }
                    else
                    {
                        // 发送队列为空，移除 POLLOUT 事件
                        c.info.events &= ~POLLOUT;
                        if (c.write_closed)
                        {
                            closefd(item.fd, -10);
                        }
                    }
                }
                else if (item.revents & POLLHUP)
                {
                    // 对方关闭连接,使用-1标记是POLLHUP事件触发了
                    closefd(item.fd, -1);
                }
                else if (item.revents & (POLLERR | POLLNVAL))
                {
                    closefd(item.fd, -5);
                }
                // else 没有事件
            }
        }
        close(server_sock);
        return true;
    }
};
