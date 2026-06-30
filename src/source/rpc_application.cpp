#include "rpc_application.h"

#include <muduo/base/Logging.h>
#include <glog/logging.h>
#include <iostream>
#include <signal.h>
#include <unistd.h>

RpcApplication &RpcApplication::GetInstance()
{
    static RpcApplication app;
    return app;
}

void RpcApplication::Init(int argc, char **argv)
{
    // 信号隔离：屏蔽当前线程（也就是主线程）的 SIGINT 和 SIGTERM
    // 这样后续所有由主线程派生出来的业务线程、Muduo 网络线程，都会继承这个掩码，免疫这些信号。
    sigset_t set;  // 声明一个信号集变量 set
    sigemptyset(&set);  // 将里面的所有位都清零
    sigaddset(&set, SIGINT);  // SIGINT 是中断信号，Ctrl + C
    sigaddset(&set, SIGTERM); // SIGTERM是终止信号，kill <pid>（不带 -9 参数）
    pthread_sigmask(SIG_BLOCK, &set, nullptr);  // 对于当前调用这行代码的线程（main 主线程），把 set 里的信号（即 SIGINT 和 SIGTERM）屏蔽掉

    if (argc < 2) {
        LOG(ERROR) << "Usage: " << argv[0] << " -i <config_file>";
        exit(EXIT_FAILURE);
    }

    // 解析命令行参数，获取配置文件路径
    int opt;
    std::string config_file;
    while ((opt = getopt(argc, argv, "i:")) != -1) {
        switch (opt) {
            case 'i':
                config_file = optarg;
                break;
            case '?': // 未知参数
            case ':': // 缺少参数
            default:
                LOG(ERROR) << "Invalid option: " << optopt;
                exit(EXIT_FAILURE);
        }
    }

    if (config_file.empty()) {
        LOG(ERROR) << "Config file is not specified. Use -i <config_file>";
        exit(EXIT_FAILURE);
    }

    if (!config.LoadConfigFile(config_file)) {
        LOG(ERROR) << "Failed to load config file: " << config_file;
        exit(EXIT_FAILURE);
    }

    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    FLAGS_alsologtostderr = true;
    muduo::Logger::setLogLevel(muduo::Logger::WARN);

    // 启动 ZooKeeper 客户端，阻塞等待连接成功
    zk_client.Start();
}

RpcConfig &RpcApplication::GetConfig()
{
    return config;
}

ZkClient &RpcApplication::GetZkClient()
{
    return zk_client;
}
