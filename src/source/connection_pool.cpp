#include "connection_pool.h"
#include "net_utils.h"
#include "rpc_application.h"
#include "rpc_header.pb.h"

#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <glog/logging.h>
#include <netinet/tcp.h>

ConnectionPool& ConnectionPool::GetInstance()
{
    static ConnectionPool instance;
    return instance;
}

ConnectionPool::~ConnectionPool()
{
    std::lock_guard<std::mutex> global_lock(global_mutex);
    for (auto &[key, hp] : pool) {
        // 使用局部锁清理各自的队列
        std::lock_guard<std::mutex> local_lock(hp->pool_mutex);
        while (!hp->free_queue.empty()) {
            ConnectionItem item = hp->free_queue.front();
            hp->free_queue.pop();
            if (item.fd != -1) {
                close(item.fd);
            }
        }
    }
}

bool ConnectionPool::Ping(int fd)
{
    // 1. 备份原有的超时设置
    struct timeval old_tv;
    socklen_t len = sizeof(old_tv);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&old_tv, &len) < 0) {
        return false;
    }

    // 定义一个守卫，在作用域结束时还原现场
    struct TimeoutGuard {
        int fd_;
        struct timeval old_tv_;
        ~TimeoutGuard() {
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&old_tv_, sizeof(old_tv_));
        }
    } guard{fd, old_tv};

    /**
     * 2. 设置短超时 (500ms) 执行探测
     * 如果没有设置这 500ms 会怎样？
     * 假设此时服务端的机器电源被拔了，或者中间的防火墙把连接切断了（并且没发 RST 包）。
     * 当执行到 recv(fd, buffer) 时，因为 TCP 连接表面上还在，但永远不会有数据回来了。
     * Linux 底层的 recv 是一个阻塞调用，默认情况下，它会一直死等，可能会等上十几分钟甚至几个小时（直到系统底层的 TCP Keepalive 超时）。
     * 这就意味着，你这个业务线程永远卡死在这一行代码上了。如果有 600 个并发请求，瞬间你的 600 个线程就全卡死在 Ping 上了，你的系统直接瘫痪。
     * 
     * 设置了 500ms 会怎样？（快速失败 Fail-fast）
     * 设置了 SO_RCVTIMEO = 500000 微秒（即 500 毫秒）后，recv 最多只会等 500 毫秒。
     * 如果 500 毫秒内服务端回了 Pong，说明连接极其健康，完美。
     * 如果 500 毫秒没收到，recv 会立刻返回 -1，并把 errno 设置为 EAGAIN 或 EWOULDBLOCK。
     * 此时 Ping 函数就能迅速返回 false，通知连接池这个 fd 已经死了，赶紧 Close 掉去建新连接。
     */
    struct timeval tv_temp;
    tv_temp.tv_sec = 0;
    tv_temp.tv_usec = 500000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_temp, sizeof(tv_temp)) < 0) {
        return false;
    }

    myrpc::RpcRequestHeader ping_header;
    ping_header.set_msg_type(myrpc::PING);

    std::string header_str;
    ping_header.SerializeToString(&header_str);

    uint32_t magic_num = 0x12345678;
    uint32_t header_size = header_str.size();
    // 协议前缀 12 字节 (4+4+4) + 变长 header_size + 0 字节 Args
    uint32_t total_size = 12 + header_size; // Ping 也没有 Args

    uint32_t net_magic_num = htonl(magic_num);
    uint32_t net_total_size = htonl(total_size);
    uint32_t net_header_size = htonl(header_size);

    std::string send_buf;
    send_buf.append((const char *)&net_magic_num, 4);
    send_buf.append((const char *)&net_total_size, 4);
    send_buf.append((const char *)&net_header_size, 4);
    send_buf.append(header_str);

    // 发送 Ping
    if (send(fd, send_buf.c_str(), send_buf.size(), 0) == -1) {
        return false;
    }

    // 接收 Pong
    // 响应协议是：[4字节总长度] + [4字节Header长度] + [RpcResponseHeader]
    char recv_prefix[8];
    // 先严格读满 8 字节前缀 (总长度 + 头长度)
    if (net_utils::recv_exact(fd, recv_prefix, 8) != 8) {
        return false;
    }
    
    uint32_t recv_total_size = ntohl(*(uint32_t*)(&recv_prefix[0]));
    uint32_t recv_header_size = ntohl(*(uint32_t*)(&recv_prefix[4]));

    // 计算还需要读多少字节的 Header 
    std::string recv_header_buf;
    recv_header_buf.resize(recv_header_size);
    if (net_utils::recv_exact(fd, &recv_header_buf[0], recv_header_size) != recv_header_size) {
        return false;
    }
    
    myrpc::RpcResponseHeader pong_header;
    if (pong_header.ParseFromArray(recv_header_buf.data(), recv_header_size)) {
        if (pong_header.msg_type() == myrpc::PONG && pong_header.errcode() == 0) {
            
            // 排空脏数据
            uint32_t remaining_bytes = recv_total_size - 8 - recv_header_size;
            if (remaining_bytes > 0) {
                std::string dump_buf;
                dump_buf.resize(remaining_bytes);
                net_utils::recv_exact(fd, &dump_buf[0], remaining_bytes);
            }
            
            return true; 
        }
    }

    return false;
}

int ConnectionPool::GetConnection(const RpcEndpoint &endpoint)
{
    while (true) {
        HostPool *hp = nullptr;
        // 【第一阶段：全局锁】只负责查字典
        {
            std::lock_guard<std::mutex> global_lock(global_mutex);
            if (pool.find(endpoint) == pool.end()) {
                pool[endpoint] = std::make_unique<HostPool>();
            }
            hp = pool[endpoint].get();
        } // 全局锁释放，此时请求不同 IP 的线程彻底分道扬镳

        int fd = -1;
        std::chrono::steady_clock::time_point last_active;
        bool need_create = false;

        // 【第二阶段：局部锁】在各自的池子里处理排队和拿取
        {
            std::unique_lock<std::mutex> local_lock(hp->pool_mutex);
            
            hp->cv.wait(local_lock, [hp]() {
                return !hp->free_queue.empty() || hp->total_created < hp->max_connections;
            });

            if (!hp->free_queue.empty()) {
                auto item = hp->free_queue.front();
                hp->free_queue.pop();
                fd = item.fd;
                last_active = item.last_active_time;
            } else {
                hp->total_created++; 
                need_create = true;
            }
        }  // 局部锁释放

        // 【第三阶段：无锁操作】去底层建连或执行 Ping
        if (need_create) {
            fd = CreateNewConnection(endpoint);
            if (fd == -1) {
                // 建连失败，重获局部锁退回名额
                std::lock_guard<std::mutex> local_lock(hp->pool_mutex);
                hp->total_created--;
                hp->cv.notify_one();
                return -1;
            }
            return fd; // 新建成功，返回給业务
        }

        // 拿到了，在【无锁】状态下进行空闲判断和 Ping 测试。
        auto now = std::chrono::steady_clock::now();
        auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_active).count();

        if (idle_duration > 10) { // 惰性心跳
            if (Ping(fd)) {
                // 心跳存活，返回可用 fd
                return fd;
            } else {
                // 心跳失败，关闭死连接。
                // 此时还在 while(true) 循环内，会自动进入下一次循环，去池子里拿下一个
                CloseConnection(endpoint, fd);
                continue;
            }
        } else {
            // 10 秒内刚用过，大概率是健康的，直接返回
            return fd;
        }
    }
}

void ConnectionPool::ReleaseConnection(const RpcEndpoint &endpoint, int fd)
{
    if (fd == -1) {
        return;
    }

    HostPool* hp = nullptr;

    {
        std::lock_guard<std::mutex> global_lock(global_mutex);
        if (pool.find(endpoint) != pool.end()) {
            hp = pool[endpoint].get();
        }
    }

    // 找到目标后，脱离全局锁，只用局部锁放回连接
    if (hp) {
        std::lock_guard<std::mutex> local_lock(hp->pool_mutex);
        hp->free_queue.emplace(fd, std::chrono::steady_clock::now());
        hp->cv.notify_one(); 
    }
}

void ConnectionPool::CloseConnection(const RpcEndpoint &endpoint, int fd)
{
    if (fd != -1) {
        close(fd); 
        HostPool* hp = nullptr;

        {
            std::lock_guard<std::mutex> global_lock(global_mutex);
            if (pool.find(endpoint) != pool.end()) {
                hp = pool[endpoint].get();
            }
        }

        // 找到目标后，脱离全局锁，只用局部锁腾出名额
        if (hp) {
            std::lock_guard<std::mutex> local_lock(hp->pool_mutex);
            hp->total_created--;  
            hp->cv.notify_one();  
        }
    }
}

int ConnectionPool::CreateNewConnection(const RpcEndpoint &endpoint)
{
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        return -1;
    }

    // 1. 设置 socket 为非阻塞模式
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags == -1) {
        close(client_fd);
        return -1;
    }
    if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(client_fd);
        return -1;
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(endpoint.port);
    server_addr.sin_addr.s_addr = inet_addr(endpoint.ip.c_str());

    // 2. 发起非阻塞 connect
    int ret = connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret == -1) {
        if (errno == EINPROGRESS) {
            // 三次握手正在进行中，使用 poll 监听可写事件
            struct pollfd pfd;
            pfd.fd = client_fd;
            pfd.events = POLLOUT;

            // 控制建連超時為 5000 毫秒（5秒）
            int poll_ret = poll(&pfd, 1, 5000); 
            if (poll_ret <= 0) { 
                // poll_ret == 0 表示超時，< 0 表示 poll 出錯
                close(client_fd);
                return -1;
            }
            
            // 檢查 socket 錯誤狀態以確認是否真正建連成功
            int err = 0;
            socklen_t errlen = sizeof(err);
            if (getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
                close(client_fd);
                return -1;
            }
        } else {
            // 其他直接失敗的錯誤（如 Network is unreachable）
            close(client_fd);
            return -1;
        }
    }

    // 3. 建連成功後，恢復為原來的阻塞模式，交由 SO_RCVTIMEO/SO_SNDTIMEO 控制常規讀寫超時
    if (fcntl(client_fd, F_SETFL, flags) == -1) {
        close(client_fd);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    // 禁用 TCP 的 Nagle 算法
    // Nagle 算法会把小包凑成大包再发，这会导致 RPC 调用（通常是几十字节的小包）产生 40ms 的延迟！
    // 工业级 RPC 框架（gRPC, Dubbo）都会默认开启 TCP_NODELAY。
    int opt_val = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt_val, sizeof(opt_val));

    return client_fd;
}
