## poll_server

使用`poll`API实现的单线程异步IO SERVER框架


main.cpp 为一个 redis server 示例


```
g++ -Wall -std=c++20 -O1 main.cpp
```

```
g++ -Wall -std=c++20 -flto=auto -static-libstdc++ -static-libgcc --static -Wl,-Bstatic,--gc-sections -O3 -ffunction-sections -fdata-sections main.cpp -o redisserver
```

API 设计

### 构造函数参数

**on_loop**

事件循环持续调用时一直触发，调用此函数携带两个参数： self引用，当前fd活跃个数（包含server的fd）

函数需返回一个数字，代表事件循环检测的超时时间，单位毫秒，这使得单线程程序仍然有机会执行一些外部任务

如果需要在此执行一些简单的外部任务，一般可以配置为1000-5000；如果不需要可以配置的更长

当返回值小于1时，代表意图中断事件循环

**on_open**

当成功`accept`后，回调此函数，并携带参数此链接的`fd`

当系统连接数超过限制（128）后，回调此函数携带的`fd`为-1，代表连接数已满，新链接被关闭

**on_data**

当有数据到达时，回调此函数，并携带参数此链接的`fd`和数据指针，数据长度

调用方必须判断数据长度大于0，才能读取数据buffer；系统保证每次回调携带的数据不超过64KB

数据长度为0，代表业务主动调用了关闭函数

数据长度为-1，代表事件循环收到链接中断（POLLHUP事件）

数据长度为-2，代表send返回=0，对方关闭连接

数据长度为-3，代表send返回<0，可能是 EPIPE 等错误

数据长度为-4，代表recv返回<0，可能是 ECONNRESET，EBADF 等错误

数据长度为-5，代表POLLERR或POLLNVAL 事件

数据长度为-10，代表先收到了recv返回=0，客户端可能处于半连接状态，我方发送完数据后关闭连接

### 数据发送

使用`write`函数提交一个数据发送请求，参数`fd`,发送的数据，回调函数
若传入的数据0字节，则忽略此发送请求

回调函数回调有三个参数：self引用，操作的`fd`, 发送的字节数

发送成功时执行回调函数,此字节数等于提交任务时传入数据的字节数

发送失败时，可能链接被关闭将触发on_data回调，本回调不在执行

### 链接关闭

在任意回调函数里，可直接使用成员方法`closefd`直接关闭`fd`


## 性能测试


使用`redis-benchmark -p 6479 set a b`测试

性能与原版redis性能差不多

```
====== set a b ======
  100000 requests completed in 3.78 seconds
  50 parallel clients
  3 bytes payload
  keep alive: 1

36.56% <= 1 milliseconds
99.83% <= 2 milliseconds
99.99% <= 3 milliseconds
100.00% <= 3 milliseconds
26448.03 requests per second
```

## 其他

基于本项目封装的http server库 https://github.com/suconghou/httplib

