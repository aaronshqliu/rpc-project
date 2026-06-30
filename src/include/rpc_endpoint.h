#ifndef RPC_ENDPOINT_H_
#define RPC_ENDPOINT_H_

#include <cstdint>
#include <functional>
#include <string>

struct RpcEndpoint {
    std::string ip;
    uint16_t port;

    RpcEndpoint() : ip(""), port(0) {}
    RpcEndpoint(const std::string& i, uint16_t p) : ip(i), port(p) {}

    std::string ToString() const {
        return ip + ":" + std::to_string(port);
    }

    bool operator==(const RpcEndpoint &other) const {
        return ip == other.ip && port == other.port;
    }

    bool operator!=(const RpcEndpoint &other) const {
        return !(*this == other);
    }
};

struct RpcEndpointHash {
    size_t operator()(const RpcEndpoint &ep) const {
        size_t h1 = std::hash<std::string>{}(ep.ip);
        size_t h2 = std::hash<uint16_t>{}(ep.port);
        return h1 ^ (h2 << 1);
    }
};

#endif  /* RPC_ENDPOINT_H_ */
