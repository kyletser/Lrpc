#ifndef _Lrpcchannel_h_
#define _Lrpcchannel_h_
// 此类是继承自google::protobuf::RpcChannel
// 目的是为了给客户端进行方法调用的时候，统一接收的
#include <google/protobuf/service.h>
#include "zookeeperutil.h"
#include <vector>
class LrpcChannel : public google::protobuf::RpcChannel
{
public:
    LrpcChannel(bool connectNow);
    virtual ~LrpcChannel() {}
    void CallMethod(const ::google::protobuf::MethodDescriptor *method,
                    ::google::protobuf::RpcController *controller,
                    const ::google::protobuf::Message *request,
                    ::google::protobuf::Message *response,
                    ::google::protobuf::Closure *done) override; // override可以验证是否是虚函数
private:
    std::vector<std::string> QueryServiceHosts(ZkClient *zkclient,
                                               const std::string &service_name,
                                               const std::string &method_name);
    ssize_t send_exact(int fd, const char* buf, size_t size);
    ssize_t recv_exact(int fd, char* buf, size_t size);
};
#endif
