#include "rpc_application.h"
#include "rpc_provider.h"
#include "user_service.h"

#include <glog/logging.h>

int main(int argc, char **argv)
{
    // 1. 框架初始化
    RpcApplication::GetInstance().Init(argc, argv);

    // 2. 创建 RpcProvider 实例
    RpcProvider provider;

    // 3. 将本地业务对象注册发布到 rpc 节点上
    provider.NotifyService(new UserService());

    // 4. 启动
    LOG(INFO) << "User Server is starting...";
    provider.Run();

    // 5. 进程安全退出
    LOG(INFO) << "Server completely shutdown. Goodbye!";
    LOG(INFO) << "===========================================";

    return 0;
}
