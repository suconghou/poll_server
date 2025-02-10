#include <ctype.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>
#include <set>
#include <array>
#include <map>
#include <vector>
#include <regex>
#include <queue>
#include <functional>
#include <poll.h>
#include <fcntl.h>

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
    };

private:
    std::map<int, connection> connections;
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

    inline int set_noblocking(int sockfd)
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

    // 关闭指定的fd, 并执行回调, 如果已经关闭过，则忽略，err>0 时不执行回调
    bool closefd(int fd, int err)
    {
        if (connections.erase(fd) > 0)
        {
            if (err < 1)
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
    // 如果要发送的数据0字节，忽略发送请求
    int write(int fd, const char *data, int len, std::function<void(self &, int, int)> cb = nullptr)
    {
        return write(fd, std::string(data, len), cb);
    }
    // 返回值，已经入队的数量，当入队数量过多时，调用者需放缓以防止内存耗尽
    // 如果传入的fd不对，将抛出异常
    // 如果要发送的数据0字节，忽略发送请求
    int write(int fd, std::string data, std::function<void(self &, int, int)> cb = nullptr)
    {
        auto &c = connections.at(fd); // 如果传入的fd不对，此处抛出异常
        if (data.size() <= 0)
        {
            return c.out.size();
        }
        c.out.push({data, cb, 0});
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
        char buf[512];               // recv buffer,全局复用; TODO enlarge max = 8192
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
                        if (pollfds.size() < backlog)
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
                    else if (item.revents & (POLLERR | POLLNVAL))
                    {
                        printf("error: server socket error on fd %d event %d : %s\n", item.fd, item.revents, strerror(errno));
                    }
                    else if (item.revents != 0)
                    {
                        printf("poll server error,waht event %d ? \n", item.revents);
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
                        // 成功读取到数据，注意 recv 不会自动添加0结尾，此处我们附加
                        buf[ret] = '\0';
                        OnData(*this, item.fd, buf, ret);
                    }
                    if (ret == 0)
                    {
                        // recv返回值0 代表 连接已被对端关闭
                        closefd(item.fd, ret);
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                        {
                            // 没有数据可读，继续等待
                            printf("EAGAIN %d %d \n", item.fd, ret);
                            continue;
                        }
                        switch (errno)
                        {
                        case ECONNRESET:
                            closefd(item.fd, -2);
                            break;
                        case EBADF:
                            closefd(item.fd, -3);
                            break;
                        default:
                            closefd(item.fd, -4);
                        }
                    }
                }
                else if (item.revents & POLLOUT)
                {
                    printf("%d can send\n", item.fd);
                    auto &c = connections.at(item.fd); // 此处需要必然存在
                    auto &q = c.out;
                    if (!q.empty())
                    {
                        auto r = q.front();
                        int bytesSent = send(item.fd, r.data.c_str() + r.out_bytes, r.data.size() - r.out_bytes, MSG_DONTWAIT);
                        printf("send ret %d\n", bytesSent);
                        if (bytesSent < 0)
                        {
                            perror("send ret error");
                            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                            {
                                // 发送缓冲区满，稍后重试
                                continue;
                            }
                            // 发送失败,回调函数，负数表示失败，可以读取 strerror(errno)
                            q.pop(); // 清理发送任务
                            if (r.callback)
                            {
                                r.callback(*this, item.fd, bytesSent);
                            }
                            // TODO 是否后续会触发close事件？清理资源
                        }
                        else if (bytesSent > 0)
                        {
                            // 部分数据发送成功，可能需要稍后再试
                            r.out_bytes += bytesSent;
                            if (r.out_bytes == r.data.size())
                            {
                                q.pop(); // 发送完成，移除请求
                                if (r.callback)
                                {
                                    r.callback(*this, item.fd, r.out_bytes);
                                }
                            }
                            else
                            {
                                printf("数据分片 \n");
                                // 数据分片了，虽然send不一定将传给他的数据一次全部发送，但是发送过大的buffer，导致对方接受缓冲区溢出，很可能导致对方发送ECONNRESET中断
                                // 因此不能传递给send大的buffer size
                                usleep(1000);
                            }
                        }
                        else // bytesSent == 0 ，对方已关闭了链接，此时不在执行数据已发送的callback
                        {
                            // TODO test， 是否还会有收到POLLHUP事件？
                            c.info.events &= ~POLLOUT;
                            closefd(item.fd, 0);
                        }
                    }
                    else
                    {
                        // 发送队列为空，移除 POLLOUT 事件
                        c.info.events &= ~POLLOUT;
                    }
                }
                else if (item.revents & POLLHUP)
                {
                    // 对方关闭连接,使用-1标记是POLLHUP事件触发了
                    closefd(item.fd, -1);
                }
                else if (item.revents & (POLLERR | POLLNVAL))
                {
                    printf("socket error on fd %d event %d : %s\n", item.fd, item.revents, strerror(errno));
                    closefd(item.fd, -5);
                }
                else if (item.revents != 0)
                {
                    printf("poll error,waht event %d ? \n", item.revents);
                }
                // else 没有事件
            }
        }
        close(server_sock);
        return true;
    }
};
