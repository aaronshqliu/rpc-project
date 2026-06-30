cmake_minimum_required(VERSION 3.10)
project(MyRpcProject)

# 1. 编译标准设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 如果命令行没有指定 CMAKE_BUILD_TYPE，则默认设置为 Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

message(STATUS "=== 当前 RPC 框架构建模式: ${CMAKE_BUILD_TYPE} ===")

# Debug 模式参数: 开启 gdb 调试信息，关闭所有优化 (-O0)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Release 模式参数: 开启最高级别优化 (-O3)，并定义 NDEBUG 关闭 assert 断言
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# 2. 寻找系统依赖包
find_package(Protobuf REQUIRED)

# 3. 规范化输出目录结构
# 静态库输出到根目录的 lib 文件夹
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
# 可执行文件输出到根目录的 bin 文件夹
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# =========================================================
# 第一部分：编译核心 RPC 框架
# =========================================================
# 自动扫描 src/source 下的所有 .cpp 源文件
file(GLOB RPC_SOURCES "src/source/*.cpp")

# 自动编译框架层的 protobuf 文件
protobuf_generate_cpp(RPC_PROTO_SRCS RPC_PROTO_HDRS src/rpc_header.proto)

# 创建静态库目标 myrpc
add_library(myrpc STATIC ${RPC_SOURCES} ${RPC_PROTO_SRCS})

# 配置 myrpc 的头文件搜索路径
target_include_directories(myrpc PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_BINARY_DIR} # Protobuf 生成的 pb.h 文件存放位置
)

# 添加 THREADED 宏定义
target_compile_definitions(myrpc PRIVATE THREADED)

# 配置 myrpc 需要链接的第三方库
target_link_libraries(myrpc PUBLIC
    ${Protobuf_LIBRARIES}
    muduo_http
    muduo_net
    muduo_base
    zookeeper_mt
    glog
)

# =========================================================
# 第二部分：编译业务层示例程序
# =========================================================
# 自动编译 example 目录下的 protobuf 文件
protobuf_generate_cpp(EXAMPLE_PROTO_SRCS EXAMPLE_PROTO_HDRS
    example/user.proto
    example/order.proto
)

# 告诉 CMake 到哪里找 example 层的头文件
include_directories(
    ${CMAKE_SOURCE_DIR}/example
    ${CMAKE_CURRENT_BINARY_DIR}/example
)

# ---- 编译 Server 程序 ----
add_executable(user_server
    example/user_server.cpp
    example/user_service.cpp
    ${EXAMPLE_PROTO_SRCS}
)
target_link_libraries(user_server PRIVATE myrpc)

add_executable(order_server
    example/order_server.cpp
    example/order_service.cpp
    ${EXAMPLE_PROTO_SRCS}
)
target_link_libraries(order_server PRIVATE myrpc)

# ---- 编译 Client 程序 ----
add_executable(client
    example/client.cpp
    ${EXAMPLE_PROTO_SRCS}
)
target_link_libraries(client PRIVATE myrpc)
