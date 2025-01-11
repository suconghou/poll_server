#include "poll.cpp"

int main()
{
    auto on_loop = [](poll_server &a)
    {
        return 15000;
    };

    auto on_open = [&](poll_server &a, int fd)
    {
        printf("client %d connected \n", fd);

        a.write(fd, "hi", [](poll_server &b, int x, int y)
                { printf("write callback %d %d \n", x, y); });
    };
    // n值为0代表对方关闭了,(recv返回0)
    // -1 代表 收到中断（POLLHUP事件）
    auto on_data = [](poll_server &a, int fd, const char *buf, int n)
    {
        if (n < 1)
        {
            // 测试是否重复
            printf("client %d closed %d \n", fd, n);
            return;
        }
        printf("Received %d data from client %d: %s\n", n, fd, buf);
        a.write(fd, buf, n, nullptr);
    };
    poll_server a(on_loop, on_open, on_data);

    a.start(8080);
}