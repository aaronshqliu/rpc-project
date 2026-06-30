#ifndef RPC_CHANNEL_H
#define RPC_CHANNEL_H

#include "rpc_endpoint.h"

#include <atomic>
#include <google/protobuf/service.h>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class MyRpcChannel : public google::protobuf::RpcChannel {
public:
    MyRpcChannel() {}
    virtual ~MyRpcChannel() {}

    void CallMethod(const ::google::protobuf::MethodDescriptor *method,
                    ::google::protobuf::RpcController *controller,
                    const ::google::protobuf::Message *request,
                    ::google::protobuf::Message *response,
                    ::google::protobuf::Closure *done) override;
    
    void PreFetchService(const std::string &service_name, const std::string &method_name);

private:
    // 将主机列表和独立的轮询计数器绑定在一起
    struct ServiceNodeList {
        std::vector<RpcEndpoint> hosts;
        std::atomic<uint32_t> next_idx {0}; // 专属这个服务节点的计数器，从 0 开始
    };

    std::unordered_map<std::string, std::shared_ptr<ServiceNodeList>> host_cache; // 缓存的是该方法下的【所有可用节点列表】
    std::shared_mutex cache_mutex;

    // 死节点黑名单
    std::unordered_map<RpcEndpoint, std::chrono::steady_clock::time_point, RpcEndpointHash> quarantine_list;
    std::mutex quarantine_mutex;

    bool IsHostQuarantined(const RpcEndpoint &endpoint);  // 检查指定节点是否还在黑名单隔离期内
    RpcEndpoint QueryZkForHost(const std::string &service_name, const std::string &method_name);
    void RemoveInvalidHost(const std::string &path, const RpcEndpoint &invalid_host);
    void BackgroundRefreshCache(const std::string &path);
    RpcEndpoint GetHostByRoundRobin(std::shared_ptr<ServiceNodeList> list);
    std::vector<RpcEndpoint> ParseHostStrings(const std::vector<std::string> &host_strs);
};

#endif
