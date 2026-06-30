#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <csignal>
#include <memory>

#include <glog/logging.h>

#include "rpc_application.h"
#include "rpc_channel.h"
#include "rpc_controller.h"
#include "user.pb.h"
#include "order.pb.h"

// 全局运行标志位，用于优雅停机
std::atomic<bool> g_running {true};

void SignalHandler(int signum) 
{
    LOG(INFO) << "===============================================";
    LOG(INFO) << "Caught signal " << signum << ". Initiating graceful shutdown...";
    g_running = false; 
}

// 压测指标统计结构体（使用原子操作保证无锁线程安全）
struct BenchmarkMetrics {
    std::atomic<int64_t> total_requests{0};
    std::atomic<int64_t> success_requests{0};
    std::atomic<int64_t> network_failures{0};  // 网络/RPC框架层面的失败
    std::atomic<int64_t> business_failures{0}; // 业务逻辑失败（如密码错误）
};

// 核心工作线程函数
void BenchmarkWorker(MyRpcChannel* global_channel, int thread_id, int duration_seconds, BenchmarkMetrics& metrics)
{
    user::UserServiceRpc_Stub user_stub(global_channel);
    order::OrderServiceRpc_Stub order_stub(global_channel);

    MyRpcController controller;

    // 提前实例化 Protobuf 对象，避免循环内频繁在堆上分配内存
    user::RegisterRequest reg_request;
    user::RegisterResponse reg_response;
    user::LoginRequest login_request;
    user::LoginResponse login_response;
    order::MakeOrderRequest order_request;
    order::MakeOrderResponse order_response;

    auto start_time = std::chrono::steady_clock::now();
    int iteration = 0;

    // 循环条件：全局标志位为 true，且没有超时
    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= duration_seconds) {
            break; // 达到设定时间，自然退出
        }

        int current_user_id = thread_id * 10000 + iteration;
        std::string unique_username = "user_" + std::to_string(thread_id) + "_" + std::to_string(iteration);

        bool network_error_occurred = false;

        // ================= 1. 测试 Register =================
        controller.Reset();
        reg_request.Clear();
        reg_response.Clear();
        reg_request.set_id(current_user_id);
        reg_request.set_username(unique_username);
        reg_request.set_password("12345");

        user_stub.Register(&controller, &reg_request, &reg_response, nullptr);
        metrics.total_requests++;

        if (controller.Failed()) {
            metrics.network_failures++;
            network_error_occurred = true;
        } else if (reg_response.result().code() == 0) {
            metrics.success_requests++;
        } else {
            metrics.business_failures++;
        }

        // ================= 2. 测试 Login =================
        controller.Reset();
        login_request.Clear();
        login_response.Clear();
        login_request.set_username(unique_username);
        login_request.set_password("12345");

        user_stub.Login(&controller, &login_request, &login_response, nullptr);
        metrics.total_requests++;

        if (controller.Failed()) {
            metrics.network_failures++;
            network_error_occurred = true;
        } else if (login_response.result().code() == 0) {
            metrics.success_requests++;
        } else {
            metrics.business_failures++;
        }

        // ================= 3. 测试 MakeOrder =================
        controller.Reset();
        order_request.Clear(); 
        order_response.Clear();
        order_request.set_user_id(current_user_id);
        order_request.set_product_id(1);

        order_stub.MakeOrder(&controller, &order_request, &order_response, nullptr);
        metrics.total_requests++;

        if (controller.Failed()) {
            metrics.network_failures++;
            network_error_occurred = true;
        } else if (order_response.success()) { 
            metrics.success_requests++; 
        } else { 
            metrics.business_failures++; 
        }

        // ================= 空转保护 =================
        // 如果连续发生网络错误，休眠 50ms。
        // 防止在断网测试时，Fail-Fast 机制导致 while 循环瞬间跑满，造成 CPU 100% 假死。
        if (network_error_occurred) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        iteration++;
    }
}

int main(int argc, char **argv)
{
    signal(SIGINT, SignalHandler);

    RpcApplication::GetInstance().Init(argc, argv);

    // 压测参数配置
    const int thread_counts = 64;            // 并发线程数
    const int duration_seconds = 60;         // 测试总时长（秒）

    LOG(INFO) << "================ RPC Benchmark Client ================";
    LOG(INFO) << "Starting " << thread_counts << " threads for " << duration_seconds << " seconds...";
    LOG(INFO) << "Press Ctrl+C to stop early and generate report.";
    LOG(INFO) << "======================================================";

    std::unique_ptr<MyRpcChannel> global_channel = std::make_unique<MyRpcChannel>();
    BenchmarkMetrics metrics;
    std::vector<std::thread> threads;

    // 记录测试开始时间
    auto start_time = std::chrono::high_resolution_clock::now();

    // 启动多线程压测
    for (int i = 0; i < thread_counts; ++i) {
        threads.emplace_back(
            BenchmarkWorker, global_channel.get(), i, duration_seconds, std::ref(metrics));
    }

    // 阻塞等待所有线程工作完毕
    for (auto &thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // 记录测试结束时间
    auto end_time = std::chrono::high_resolution_clock::now();
    double actual_duration = std::chrono::duration<double>(end_time - start_time).count();

    // 汇总与计算 QPS
    int64_t total = metrics.total_requests.load();
    int64_t success = metrics.success_requests.load();
    int64_t net_fail = metrics.network_failures.load();
    int64_t biz_fail = metrics.business_failures.load();

    LOG(INFO) << "================ RPC 压测报告 ================";
    LOG(INFO) << "实际运行耗时 (Time taken)   : " << actual_duration << " seconds";
    LOG(INFO) << "发起的总请求 (Total Reqs)   : " << total;
    LOG(INFO) << "成功请求 (Success)       : " << success;
    LOG(INFO) << "网络失败 (Network Fail)  : " << net_fail;
    LOG(INFO) << "业务失败 (Business Fail) : " << biz_fail;
    LOG(INFO) << "-----------------------------------------------";

    if (actual_duration > 0.000001) {
        double total_qps = total / actual_duration;
        double success_qps = success / actual_duration;

        LOG(INFO) << "总体吞吐量 (Total QPS)   : " << static_cast<int64_t>(total_qps) << " req/s";
        LOG(INFO) << "成功吞吐量 (Success QPS) : " << static_cast<int64_t>(success_qps) << " req/s";
    }

    LOG(INFO) << "=================================================";
    return 0;
}
