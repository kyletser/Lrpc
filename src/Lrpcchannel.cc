/*
    * Lrpcchannel.cc
    *
    * Created on: 2026年3月5日
    * Author:LXP
*/
#include "Lrpcchannel.h"
#include "Lrpcheader.pb.h"
#include "zookeeperutil.h"
#include "Lrpcapplication.h"
#include "Lrpccontroller.h"
#include "memory"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <errno.h>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include "LrpcLogger.h"

namespace {

using Clock = std::chrono::steady_clock;

struct DiscoveryCacheEntry {
    std::vector<std::string> hosts;
    Clock::time_point expire_at;
};

std::mutex g_discovery_cache_mu;
std::unordered_map<std::string, DiscoveryCacheEntry> g_discovery_cache;
const std::chrono::milliseconds kDiscoveryCacheTtl(2000);

ZkClient *GetSharedZkClient() {
    static ZkClient zk_client;
    static std::once_flag init_flag;

    std::call_once(init_flag, [] {
        zk_client.Start();
    });

    return &zk_client;
}

bool CreateConnectionByEndpoint(const std::string &endpoint, int &fd) {
    size_t idx = endpoint.find(':');
    if (idx == std::string::npos) {
        return false;
    }

    std::string ip = endpoint.substr(0, idx);
    uint16_t port = static_cast<uint16_t>(atoi(endpoint.substr(idx + 1).c_str()));

    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (clientfd == -1) {
        return false;
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (connect(clientfd, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) == -1) {
        close(clientfd);
        return false;
    }

    fd = clientfd;
    return true;
}

class RpcConnectionPool {
public:
    static RpcConnectionPool &Instance() {
        static RpcConnectionPool pool;
        return pool;
    }

    int Acquire(const std::string &endpoint, int wait_ms) {
        auto bucket = GetBucket(endpoint);
        std::unique_lock<std::mutex> lock(bucket->mu);

        while (true) {
            if (!bucket->idle_fds.empty()) {
                int fd = bucket->idle_fds.front();
                bucket->idle_fds.pop_front();
                bucket->busy_fds.insert(fd);
                return fd;
            }

            if (bucket->total < max_per_endpoint_) {
                ++bucket->total;
                lock.unlock();

                int fd = -1;
                bool ok = CreateConnectionByEndpoint(endpoint, fd);

                lock.lock();
                if (ok) {
                    bucket->busy_fds.insert(fd);
                    return fd;
                }

                --bucket->total;
                bucket->cv.notify_one();
                return -1;
            }

            if (!bucket->cv.wait_for(lock,
                                     std::chrono::milliseconds(wait_ms),
                                     [bucket, this] {
                                         return !bucket->idle_fds.empty() || bucket->total < max_per_endpoint_;
                                     })) {
                return -1;
            }
        }
    }

    void Release(const std::string &endpoint, int fd, bool healthy) {
        auto bucket = GetBucket(endpoint);
        std::lock_guard<std::mutex> lock(bucket->mu);

        if (bucket->busy_fds.erase(fd) == 0) {
            return;
        }

        if (healthy) {
            bucket->idle_fds.push_back(fd);
        } else {
            close(fd);
            --bucket->total;
        }

        bucket->cv.notify_one();
    }

private:
    struct Bucket {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<int> idle_fds;
        std::unordered_set<int> busy_fds;
        int total{0};
    };

    std::shared_ptr<Bucket> GetBucket(const std::string &endpoint) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = buckets_.find(endpoint);
        if (it != buckets_.end()) {
            return it->second;
        }

        auto bucket = std::make_shared<Bucket>();
        buckets_.emplace(endpoint, bucket);
        return bucket;
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Bucket>> buckets_;
    int max_per_endpoint_{64};
};

std::string PickEndpointByRoundRobin(const std::string &service_name,
                                     const std::string &method_name,
                                     const std::vector<std::string> &hosts) {
    static std::mutex rr_mu;
    static std::unordered_map<std::string, size_t> rr_index;

    std::string key = service_name + "/" + method_name;
    std::lock_guard<std::mutex> lock(rr_mu);
    size_t &idx = rr_index[key];
    std::string endpoint = hosts[idx % hosts.size()];
    ++idx;
    return endpoint;
}

std::string BuildServiceMethodKey(const std::string &service_name, const std::string &method_name) {
    return service_name + "/" + method_name;
}

} // namespace

ssize_t LrpcChannel::send_exact(int fd, const char* buf, size_t size) {
    size_t total_sent = 0;
    while (total_sent < size) {
        ssize_t ret = send(fd, buf + total_sent, size - total_sent, 0);
        if (ret == 0) return total_sent;
        if (ret == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        total_sent += ret;
    }
    return total_sent;
}

ssize_t LrpcChannel::recv_exact(int fd, char* buf, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t ret = recv(fd, buf + total_read, size - total_read, 0);
        if (ret == 0) return 0;
        if (ret == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        total_read += ret;
    }
    return total_read;
}

void LrpcChannel::CallMethod(const ::google::protobuf::MethodDescriptor *method,
                             ::google::protobuf::RpcController *controller,
                             const ::google::protobuf::Message *request,
                             ::google::protobuf::Message *response,
                             ::google::protobuf::Closure *done)
{
    const google::protobuf::ServiceDescriptor *sd = method->service();
    std::string service_name = sd->name();
    std::string method_name = method->name();

    ZkClient *zkCli = GetSharedZkClient();

    std::vector<std::string> hosts;
    std::string cache_key = BuildServiceMethodKey(service_name, method_name);
    Clock::time_point now = Clock::now();

    {
        std::lock_guard<std::mutex> lock(g_discovery_cache_mu);
        auto it = g_discovery_cache.find(cache_key);
        if (it != g_discovery_cache.end() && it->second.expire_at > now) {
            hosts = it->second.hosts;
        }
    }

    if (hosts.empty()) {
        hosts = QueryServiceHosts(zkCli, service_name, method_name);
        std::lock_guard<std::mutex> lock(g_discovery_cache_mu);
        g_discovery_cache[cache_key] = DiscoveryCacheEntry{hosts, now + kDiscoveryCacheTtl};
    }

    if (hosts.empty()) {
        controller->SetFailed("no available provider for " + service_name + "." + method_name);
        return;
    }

    std::string endpoint = PickEndpointByRoundRobin(service_name, method_name, hosts);
    int fd = RpcConnectionPool::Instance().Acquire(endpoint, 1000);
    if (fd < 0) {
        controller->SetFailed("acquire pooled connection failed");
        return;
    }

    bool healthy = true;

    std::string args_str;
    if (!request->SerializeToString(&args_str)) {
        controller->SetFailed("serialize request fail");
        RpcConnectionPool::Instance().Release(endpoint, fd, healthy);
        return;
    }

    Lrpc::RpcHeader Lrpcheader;
    Lrpcheader.set_service_name(service_name);
    Lrpcheader.set_method_name(method_name);
    Lrpcheader.set_args_size(args_str.size());

    std::string rpc_header_str;
    if (!Lrpcheader.SerializeToString(&rpc_header_str)) {
        controller->SetFailed("serialize rpc header error!");
        RpcConnectionPool::Instance().Release(endpoint, fd, healthy);
        return;
    }

    uint32_t header_size = rpc_header_str.size();
    uint32_t total_len = 4 + header_size + args_str.size();

    uint32_t net_total_len = htonl(total_len);
    uint32_t net_header_len = htonl(header_size);

    std::string send_rpc_str;
    send_rpc_str.reserve(4 + 4 + header_size + args_str.size());

    send_rpc_str.append((char*)&net_total_len, 4);
    send_rpc_str.append((char*)&net_header_len, 4);
    send_rpc_str.append(rpc_header_str);
    send_rpc_str.append(args_str);

    if (send_exact(fd, send_rpc_str.data(), send_rpc_str.size()) != static_cast<ssize_t>(send_rpc_str.size())) {
        healthy = false;
        controller->SetFailed("send error");
        RpcConnectionPool::Instance().Release(endpoint, fd, healthy);
        return;
    }

    uint32_t response_len = 0;
    if (recv_exact(fd, (char*)&response_len, 4) != 4) {
        healthy = false;
        controller->SetFailed("recv response length error");
        RpcConnectionPool::Instance().Release(endpoint, fd, healthy);
        return;
    }
    response_len = ntohl(response_len);

    std::vector<char> recv_buf(response_len);
    if (recv_exact(fd, recv_buf.data(), response_len) != static_cast<ssize_t>(response_len)) {
        healthy = false;
        controller->SetFailed("recv response body error");
        RpcConnectionPool::Instance().Release(endpoint, fd, healthy);
        return;
    }

    if (!response->ParseFromArray(recv_buf.data(), response_len)) {
        healthy = false;
        controller->SetFailed("parse response error");
        RpcConnectionPool::Instance().Release(endpoint, fd, healthy);
        return;
    }

    RpcConnectionPool::Instance().Release(endpoint, fd, healthy);
    if (done != nullptr) {
        done->Run();
    }
}

std::vector<std::string> LrpcChannel::QueryServiceHosts(ZkClient *zkclient,
                                                         const std::string &service_name,
                                                         const std::string &method_name) {
    std::string method_path = "/" + service_name + "/" + method_name;
    std::vector<std::string> hosts;

    std::vector<std::string> children = zkclient->GetChildren(method_path.c_str());
    for (const auto &child : children) {
        std::string child_path = method_path + "/" + child;
        std::string host = zkclient->GetData(child_path.c_str());
        if (!host.empty() && host.find(':') != std::string::npos) {
            hosts.emplace_back(host);
        }
    }

    if (hosts.empty()) {
        std::string host = zkclient->GetData(method_path.c_str());
        if (!host.empty() && host.find(':') != std::string::npos) {
            hosts.emplace_back(host);
        }
    }

    return hosts;
}

LrpcChannel::LrpcChannel(bool connectNow) {
    (void)connectNow;
}
