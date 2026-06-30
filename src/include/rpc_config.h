#ifndef RPC_CONFIG_H
#define RPC_CONFIG_H

#include <string>
#include <unordered_map>
#include <variant>

// RpcConfig类，负责加载配置文件并提供查询接口
class RpcConfig {
public:
    bool LoadConfigFile(const std::string &fileName);    // 加载配置文件
    std::string GetString(const std::string &key, const std::string &default_val = "") const; // 查找key对应的value
    int GetInt(const std::string &key, int default_val = 0) const;

private:
    static void Trim(std::string &str); // 去掉字符串前后的空格

    std::unordered_map<std::string, std::string> config_map;
};

#endif // RPC_CONFIG_H
