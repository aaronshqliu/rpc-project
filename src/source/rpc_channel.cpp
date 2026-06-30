#include "rpc_channel.h"
#include "connection_pool.h"
#include "net_utils.h"
#include "rpc_application.h"
#include "rpc_header.pb.h"
#include "zk_client.h"

#include <arpa/inet.h>
#include <glog/logging.h>
#include <thread>

void MyRpcChannel::CallMethod(const ::google::protobuf::MethodDescriptor *method,
                              ::google::protobuf::RpcController *controller,
                              const ::google::protobuf::Message *request,
                              ::google::protobuf::Message *response,
                              ::google::protobuf::Closure *done)
{
    // 定义一个局部的守护类（RAII 自动守护），C++ 局部变量出作用域会自动调用析构函数，保证 done->Run() 100% 被调用一次。
    struct ClosureGuard {
        ::google::protobuf::Closure *cb;
        ~ClosureGuard() {
            if (cb) {
                cb->Run();
            }
        }
    } guard {done};

    std::string service_name = method->service()->name();
    std::string method_name = method->name();

    // 1. 序列化请求参数
    std::string args_str;
    if (!request->SerializeToString(&args_str)) {
        controller->SetFailed("Serialize request error!");
        return;
    }

    // 2. 构造并序列化 RPC Header
    myrpc::RpcRequestHeader request_header;
    request_header.set_service_name(service_name);
    request_header.set_method_name(method_name);
    request_header.set_args_size(args_str.size());
    request_header.set_msg_type(myrpc::NORMAL_RPC);

    std::string header_str;
    if (!request_header.SerializeToString(&header_str)) {
        controller->SetFailed("Serialize RPC header error!");
        return;
    }

    // 3. 计算各部分长度并组装待发送报文
    // 协议：| 4字节(魔数) | 4字节(总长度) | 4字节(Header长度) | 变长(Header) | 变长(Args) |
    uint32_t magic_num = 0x12345678;
    uint32_t header_size = header_str.size();
    uint32_t args_size = args_str.size();

    // 总长度 = Header长度字段(4字节) + Header内容长度 + Args内容长度
    uint32_t total_size = 4 + header_size + args_size;

    // 转换为网络字节序 (大端)
    uint32_t net_magic_num = htonl(magic_num);
    uint32_t net_total_size = htonl(total_size);
    uint32_t net_header_size = htonl(header_size);

    std::string send_buf;
    send_buf.append((const char *)&net_magic_num, 4);   // 1. 写入魔数
    send_buf.append((const char *)&net_total_size, 4);  // 2. 写入总长度
    send_buf.append((const char *)&net_header_size, 4); // 3. 写入Header长度
    send_buf.append(header_str);                        // 4. 接着写入Header内容
    send_buf.append(args_str);                          // 5. 最后写入Args内容

    // 4. 带自动剔除和负载均衡的重试机制
    int client_fd = -1;
    int max_retries = 3;
    bool rpc_success = false;

    for (int i = 1; i <= max_retries; ++i) {
        // 4.1 每次循环都重新获取地址 (触发轮询负载均衡算法)
        RpcEndpoint host = QueryZkForHost(service_name, method_name);
        if (host.ip.empty()) {
            // 快速失败
            // 既然命中了空占位符，说明知道服务全挂了。
            LOG_EVERY_N(ERROR, 100000) << "Fast-Fail: No providers available for " << service_name 
                                       << " (Waiting for ZK Watcher to recover)";
                                       
            controller->SetFailed("RPC Failed: Service is OFFLINE (No providers).");
            return;
        }

        // 4.2 从连接池获取连接
        client_fd = ConnectionPool::GetInstance().GetConnection(host);
        if (client_fd == -1) {
            LOG(ERROR) << "GetConnection failed for " << host.ip << ":" << host.port 
                       << " | errno: " << errno << " (" << strerror(errno) << ")";

            if (errno == EMFILE || errno == ENFILE) {
                // 客户端自身资源耗尽，不能错杀服务端，直接终止本次 RPC 重试。
                LOG(FATAL) << "Client FD exhausted! Aborting current RPC request.";
                return; 
            }

            // 这是明确的服务端宕机或网络不通的信号，执行剔除。
            LOG(WARNING) << "Server dead or unreachable. Removing invalid host: " << host.ip << ":" << host.port;
            std::string zk_path = "/" + service_name + "/" + method_name;
            RemoveInvalidHost(zk_path, host);

            continue; // 重试下一台
        }

        // 5. 连接成功，跳出重试循环，进入数据收发阶段
        // 注意：数据发送失败不自动重试，防止非幂等操作重复执行
        if (send(client_fd, send_buf.c_str(), send_buf.size(), 0) == -1) {  // 发送失败：说明连接已损坏，必须销毁，不能放回连接池。
            ConnectionPool::GetInstance().CloseConnection(host, client_fd);
            controller->SetFailed("Send RPC request failed!");
            return;
        }

        // 6. 接收服务端发回的响应：[4字节 总长度] + [4字节 Header长度] + [RpcResponseHeader] + [Response Data]
        // 注意：服务端响应为了保持高效，通常不需要加魔数，保持极简即可。
        uint32_t recv_total_size = 0;
        // 先精准读取前 4 个字节，获取后面的总长度
        if (net_utils::recv_exact(client_fd, (char *)&recv_total_size, 4) != 4) {
            ConnectionPool::GetInstance().CloseConnection(host, client_fd);
            controller->SetFailed("Recv response total_size timeout or failed!");
            return;
        }

        recv_total_size = ntohl(recv_total_size);       // 转回主机字节序
        if (recv_total_size > 64 * 1024 * 1024) {       // 限制 RPC 响应最大为 64MB
            ConnectionPool::GetInstance().CloseConnection(host, client_fd);
            controller->SetFailed("Response size too large!");
            return;
        }

        // 把剩下的整包数据全读出来
        std::string recv_buf;
        recv_buf.resize(recv_total_size);
        if (net_utils::recv_exact(client_fd, &recv_buf[0], recv_total_size) == recv_total_size) {

            // 步骤 A：提取 4 字节的 Header 长度
            uint32_t header_size = ntohl(*(uint32_t*)(&recv_buf[0]));

            // 步骤 B：反序列化 RpcResponseHeader (偏移量为 4，长度为 header_size)
            myrpc::RpcResponseHeader resp_header;
            if (!resp_header.ParseFromArray(&recv_buf[4], header_size)) {
                controller->SetFailed("Parse RpcResponseHeader error!");
                ConnectionPool::GetInstance().CloseConnection(host, client_fd);
                return;
            }

            // 步骤 C：检查框架级错误码
            if (resp_header.errcode() != 0) {
                // 如果 errcode != 0，说明业务逻辑根本没执行（比如服务没找到）
                controller->SetFailed("RPC Framework Error: " + resp_header.errmsg());
                // 注意：这里连接是健康的，只是逻辑报错，千万别销毁连接，放回连接池！
                ConnectionPool::GetInstance().ReleaseConnection(host, client_fd);
                return; // 直接退出，不需要再解析 Response Data 了
            }

            // 步骤 D：如果框架层成功，再去反序列化真正的业务 Response
            // 偏移量为 4 + header_size，剩余长度为 total_size - 4 - header_size
            uint32_t data_size = recv_total_size - 4 - header_size;
            if (!response->ParseFromArray(&recv_buf[4 + header_size], data_size)) {   
                controller->SetFailed("Parse business response data error!");
                // 业务数据错乱，安全起见销毁连接
                ConnectionPool::GetInstance().CloseConnection(host, client_fd);
            } else {  
                // RPC 调用彻底成功，将健康的连接放回池中复用！
                rpc_success = true;
                ConnectionPool::GetInstance().ReleaseConnection(host, client_fd);
            }
        } else {
            ConnectionPool::GetInstance().CloseConnection(host, client_fd);
            controller->SetFailed("Recv response data timeout or incomplete!");
            return;
        }

        break; // 建连成功并执行完收发，跳出重试循环
    }

    // --- 退出重试循环后的兜底逻辑 ---
    if (!rpc_success) {
        // 拦截隐性耗尽
        if (!controller->Failed()) {  
            controller->SetFailed("RPC Call failed: Exhausted all " +
                                  std::to_string(max_retries) + " retries for " + service_name);
        }
        LOG(ERROR) << "RPC Call aborted. Final error: " << controller->ErrorText();

        // 拦截执行，绝对不要往下走了
        return; 
    }

    // 如果代码能走到这里，说明 rpc_success == true，意味着 response 已经被成功解析赋值了！
    // 函数执行到最后一行自然结束，局部变量 guard 出栈，自动触发 done->Run()
}

RpcEndpoint MyRpcChannel::QueryZkForHost(const std::string &service_name, const std::string &method_name)
{
    std::string path = "/" + service_name + "/" + method_name;

    // 1. 尝试从缓存获取 (加读锁，允许多线程并发读)
    {
        std::shared_lock<std::shared_mutex> read_lock(cache_mutex);
        auto it = host_cache.find(path);
        if (it != host_cache.end() && it->second) {
            if (it->second->hosts.empty()) {
                // 命中了“宕机占位符”，说明知道没有节点，直接返回空，绝不去查 ZK
                return {"", 0};
            }
            return GetHostByRoundRobin(it->second);
        }
    }

    std::shared_ptr<ServiceNodeList> node_list = nullptr;

    // 2. 缓存未命中，去 ZK 查询
    {
        std::unique_lock<std::shared_mutex> write_lock(cache_mutex);

        // 防止多个线程同时发现缓存为空，阻塞在锁外，拿到锁后重复查询ZK
        auto it = host_cache.find(path);
        if (it != host_cache.end() && it->second) {
            if (it->second->hosts.empty()) {
                return {"", 0}; 
            }
            node_list = it->second;
        } else {  // 真正去请求 ZooKeeper，并开启 Watcher
            ZkClient &zk = RpcApplication::GetInstance().GetZkClient();

            std::vector<std::string> available_hosts;
            if (!zk.GetChildren(path.c_str(), available_hosts, true)) {
                // 情况 A：ZK 挂了。因为此时缓存也为空，我们实在找不到节点了
                LOG(ERROR) << "ZK is DOWN and no local cache for: " << path;
                return {"", 0};
            }
            if (available_hosts.empty()) {
                // 情况 B：ZK 正常，但该服务确实一个 Provider 都没有
                auto empty_list = std::make_shared<ServiceNodeList>();
                host_cache[path] = empty_list;

                LOG_EVERY_N(ERROR, 10000) << "Service exist in ZK but has NO instances: " << path;

                // 即使冷启动时没有实例，也必须注册 Watcher，否则这个服务以后上线了，客户端将永远无法感知
                zk.SubscribeWatcher(path, [this](int type, const std::string &changed_path) {
                    this->BackgroundRefreshCache(changed_path);
                });

                return {"", 0};
            }

            // 情况 C：查询成功且有节点，解析并更新缓存
            std::vector<RpcEndpoint> parsed_hosts = ParseHostStrings(available_hosts);
            if (!parsed_hosts.empty()) {
                node_list = std::make_shared<ServiceNodeList>();
                node_list->hosts = std::move(parsed_hosts);
                host_cache[path] = node_list;

                LOG(INFO) << "--------------------------------------------------";
                LOG(INFO) << "[Service Discovery] Path: " << path;
                LOG(INFO) << "[Service Discovery] Status: ONLINE";
                LOG(INFO) << "[Service Discovery] Instances Found: " << node_list->hosts.size();
                LOG(INFO) << "--------------------------------------------------";

                // 注册监听
                zk.SubscribeWatcher(path, [this](int type, const std::string &changed_path) {
                    this->BackgroundRefreshCache(changed_path);
                });
            }
        }
    }

    // 3. 客户端侧负载均衡 (轮询)
    if (node_list && !node_list->hosts.empty()) {
        return GetHostByRoundRobin(node_list);
    }

    return {"", 0};
}

void MyRpcChannel::PreFetchService(const std::string &service_name, const std::string &method_name)
{
    QueryZkForHost(service_name, method_name);
}

RpcEndpoint MyRpcChannel::GetHostByRoundRobin(std::shared_ptr<ServiceNodeList> list)
{
    // std::memory_order_relaxed: 只关心原子自增，不关心内存屏障同步，这样性能最高
    uint32_t idx = list->next_idx.fetch_add(1, std::memory_order_relaxed);
    return list->hosts[idx % list->hosts.size()];
}

void MyRpcChannel::RemoveInvalidHost(const std::string &path, const RpcEndpoint &invalid_host)
{
    int quarantine_time = RpcApplication::GetInstance().GetConfig().GetInt("quarantine_timeout_sec", 30);

    // 1. 先进入黑名单 30 秒隔离期
    {
        std::lock_guard<std::mutex> q_lock(quarantine_mutex);

        auto now = std::chrono::steady_clock::now();
        auto it = quarantine_list.find(invalid_host);

        // 如果节点已经在黑名单中，并且还没到期，说明已经被前面某个线程拉黑过了，当前线程直接返回
        if (it != quarantine_list.end() && now < it->second) {
            return;
        }

        // 第一次拉黑
        quarantine_list[invalid_host] = std::chrono::steady_clock::now() + std::chrono::seconds(quarantine_time);
        LOG(WARNING) << "Host " << invalid_host.ToString() << " is quarantined for 30s.";
    }

    // 2. COW 写时复制更新路由
    // 获取独占写锁，保护 host_cache 这个 Map 结构
    std::unique_lock<std::shared_mutex> write_lock(cache_mutex);
    auto it = host_cache.find(path);
    // 确保节点存在且指针不为空
    if (it != host_cache.end() && it->second) {
        auto old_list = it->second;

        // 1. 创建一个全新的结构体
        auto new_list = std::make_shared<ServiceNodeList>();

        // 2. 将健康的节点全部拷贝过去，跳过那个失效的节点
        for (const auto &h : old_list->hosts) {
            if (h != invalid_host) {
                new_list->hosts.emplace_back(h);
            }
        }

        // 3. 检查踢掉这个节点后，列表是不是空了
        if (new_list->hosts.empty()) {
            host_cache.erase(it);
            LOG(INFO) << "All instances are down. Removed service path: " << path << " from cache.";
        } else {
            // 4. 继承旧的轮询计数器（保证其他机器的轮询节奏不断）
            new_list->next_idx.store(old_list->next_idx.load(std::memory_order_relaxed), std::memory_order_relaxed);

            // 5. 原子替换智能指针！旧指针会在没线程用它时自动销毁
            it->second = new_list;
        }
    }
}

void MyRpcChannel::BackgroundRefreshCache(const std::string &path)
{
    ZkClient &zk = RpcApplication::GetInstance().GetZkClient();
    std::vector<std::string> new_host_strs;

    // 1. 重新向 ZK 拉取最新的节点列表（watch 参数依然为 true，为了监听下一次变化）
    if (!zk.GetChildren(path.c_str(), new_host_strs, true)) {
        // 如果 ZK 查询报错（网络问题等），不清空缓存，而是保留旧缓存继续使用，这叫“容错降级”。
        LOG(WARNING) << "ZK update failed, keeping STALE cache for path: " << path;
        return;
    }

    // 2. 解析新的节点数据，并执行黑名单惰性过滤
    std::vector<RpcEndpoint> parsed_hosts = ParseHostStrings(new_host_strs);
    std::vector<RpcEndpoint> healthy_hosts;

    for (const auto &host : parsed_hosts) {
        if (!IsHostQuarantined(host)) {
            healthy_hosts.emplace_back(host);
        } else {
            LOG(INFO) << "[Service Discovery] Host " << host.ToString() 
                      << " is currently quarantined. Ignoring ZK update.";
        }
    }

    int new_node_count = healthy_hosts.size();
    LOG(INFO) << "--------------------------------------------------";
    LOG(INFO) << "[Service Discovery] Path: " << path << " (NODE CHANGED)";
    LOG(INFO) << "[Service Discovery] Status: " << (new_node_count > 0 ? "ONLINE" : "OFFLINE");
    LOG(INFO) << "[Service Discovery] Instances Found: " << new_node_count;
    LOG(INFO) << "--------------------------------------------------";

    // 3. 如果全宕机了，或者 ZK 返回的节点全部都在黑名单里，保留缓存键，塞入空列表挡住洪峰，防止缓存击穿
    if (healthy_hosts.empty()) {
        auto empty_list = std::make_shared<ServiceNodeList>();

        std::unique_lock<std::shared_mutex> write_lock(cache_mutex);
        host_cache[path] = empty_list; // 覆盖旧缓存，而不是 erase
        LOG(WARNING) << "All nodes down (or quarantined) for path: " << path << ". Empty cache updated.";
        return;
    }

    // 4. 写时复制，创建新列表，平滑替换旧列表
    auto new_list = std::make_shared<ServiceNodeList>();
    new_list->hosts = std::move(healthy_hosts);  // 将过滤后的健康节点集合移入新列表
    {
        std::unique_lock<std::shared_mutex> write_lock(cache_mutex);
        // 继承旧的轮询计数器，保证负载均衡不断档
        if (auto it = host_cache.find(path); it != host_cache.end() && it->second) {
            new_list->next_idx.store(it->second->next_idx.load(std::memory_order_relaxed), std::memory_order_relaxed);
            it->second = new_list;
        } else {
            host_cache[path] = new_list;
        }
    }
}

std::vector<RpcEndpoint> MyRpcChannel::ParseHostStrings(const std::vector<std::string> &host_strs)
{
    std::vector<RpcEndpoint> parsed_hosts;
    for (const auto &host_str : host_strs) {
        size_t idx = host_str.find(":");
        if (idx != std::string::npos) {
            RpcEndpoint host;
            std::string ip = host_str.substr(0, idx);
            uint16_t port = static_cast<uint16_t>(std::stoi(host_str.substr(idx + 1)));
            parsed_hosts.emplace_back(ip, port);
        } else {
            LOG(ERROR) << "Invalid host data format from ZK (missing ':'): " << host_str;
        }
    }
    return parsed_hosts;
}

bool MyRpcChannel::IsHostQuarantined(const RpcEndpoint &endpoint) 
{
    // 加上独立的局部锁，保护黑名单 Map 的并发读写
    std::lock_guard<std::mutex> lock(quarantine_mutex);

    auto it = quarantine_list.find(endpoint);

    // 如果节点在黑名单里找到了
    if (it != quarantine_list.end()) {
        auto now = std::chrono::steady_clock::now();

        // 比较当前时间与设定的解禁时间
        if (now < it->second) {
            // 时间还没到，拒绝放行
            return true; 
        } else {
            // 惰性清理
            // 时间已经过了，就在它被查询的这一刻，把它从黑名单里删掉。
            quarantine_list.erase(it);
            return false;
        }
    }

    // 压根不在黑名单里，直接放行
    return false;
}
