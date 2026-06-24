#!/bin/bash

# 遇到错误立即退出，未定义的变量报错
set -eu

# 默认使用 Release 模式
BUILD_TYPE="Release"

if [ "$#" -ge 1 ]; then
    if [ "$1" = "--debug" ]; then
        BUILD_TYPE="Debug"
    else
        echo "警告: 未知参数 '$1'。"
        echo "用法: ./build.sh [--debug]"
        echo "将默认使用 Release 模式继续..."
        sleep 2
    fi
fi

echo "================================================="
echo "开始环境配置与构建 RPC 项目(当前模式: $BUILD_TYPE)..."
echo "================================================="

# 1. 安装系统级依赖
echo "[1/3] 正在更新软件源并安装基础环境依赖..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    gdb \
    cmake \
    libboost-all-dev \
    libgoogle-glog-dev \
    protobuf-compiler \
    libprotobuf-dev \
    zookeeperd \
    libzookeeper-mt-dev

# 2. 编译并安装 Muduo 网络库
echo "[2/3] 检查并安装 Muduo 网络库..."
if [ ! -d "/usr/local/include/muduo" ]; then
    echo "未检测到 Muduo 安装，正在通过源码编译..."
    
    # 记录当前工作目录（RPC项目根目录）
    PROJECT_ROOT=$(pwd)
    
    # 创建临时目录存放 Muduo 源码
    TEMP_MUDUO_DIR=$(mktemp -d)
    cd "$TEMP_MUDUO_DIR"
    
    git clone https://github.com/chenshuo/muduo.git
    cd muduo
    
    # 执行 muduo 的构建和本地安装脚本
    # 默认情况下，build.sh install 会将产物放到 ../build/release-install-cpp11 目录下
    ./build.sh
    ./build.sh install
    
    echo "将 Muduo 头文件和库文件安装到系统目录 (/usr/local/)..."
    sudo cp -r ../build/release-install-cpp11/include/muduo /usr/local/include/
    sudo cp ../build/release-install-cpp11/lib/* /usr/local/lib/
    
    # 刷新动态链接库缓存，确保 RPC 项目在链接时能找到 libmuduo_*.a 或 .so
    sudo ldconfig
    
    # 返回 RPC 项目目录并清理临时文件
    cd "$PROJECT_ROOT"
    rm -rf "$TEMP_MUDUO_DIR"
    echo "Muduo 安装完成！"
else
    echo "检测到 Muduo 已安装 (/usr/local/include/muduo)，跳过编译步骤。"
fi

# 3. 构建当前 RPC 项目
echo "[3/3] 正在配置并编译 RPC 项目..."
if [ -d "build" ]; then
    echo "清理旧的构建目录和产物..."
    rm -rf bin build lib
fi

mkdir build
cd build
# 将解析出的 BUILD_TYPE 传递给 CMake
cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE ..
make -j$(nproc)

echo "================================================="
echo "✅ 环境配置与编译全部完成! (模式: $BUILD_TYPE)"
echo "================================================="
