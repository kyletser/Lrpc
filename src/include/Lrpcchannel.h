#ifndef _Lrpcchannel_h_
#define _Lrpcchannel_h_
// 此类是继承自google::protobuf::RpcChannel
// 目的是为了给客户端进行方法调用的时候，统一接收的
#include <google/protobuf/service.h>
#include "zookeeperutil.h"
class LrpcChannel : public google::protobuf::RpcChannel
{
public:
    LrpcChannel(bool connectNow);
    virtual ~LrpcChannel()
    {
          if (m_clientfd >= 0) {
        close(m_clientfd);
    }  
    }
    void CallMethod(const ::google::protobuf::MethodDescriptor *method,
                    ::google::protobuf::RpcController *controller,
                    const ::google::protobuf::Message *request,
                    ::google::protobuf::Message *response,
                    ::google::protobuf::Closure *done) override; // override可以验证是否是虚函数
private:
    int m_clientfd; // 存放客户端套接字
    std::string service_name;
    std::string m_ip;
    uint16_t m_port;
    std::string method_name;
    int m_idx; // 用来划分服务器ip和port的下标
    bool newConnect(const char *ip, uint16_t port);
    std::string QueryServiceHost(ZkClient *zkclient, std::string service_name, std::string method_name, int &idx);
    ssize_t send_exact(int fd, const char* buf, size_t size);
    ssize_t recv_exact(int fd, char* buf, size_t size);
};
#endif
