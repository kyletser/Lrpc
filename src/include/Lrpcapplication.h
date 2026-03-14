#ifndef _Lrpcapplication_H
#define _Lrpcapplication_H
#include "Lrpcconfig.h"
#include "Lrpcchannel.h" 
#include  "Lrpccontroller.h"
#include<mutex>
//Lrpc基础类，负责框架的一些初始化操作
class LrpcApplication
{
    public:
    static void Init(int argc,char **argv);
    static LrpcApplication & GetInstance();
    static void deleteInstance();
    static Lrpcconfig& GetConfig();
    private:
    static Lrpcconfig m_config;
    static LrpcApplication * m_application;//全局唯一单例访问对象
    static std::mutex m_mutex;
    LrpcApplication(){}
    ~LrpcApplication(){}
    LrpcApplication(const LrpcApplication&)=delete;
    LrpcApplication(LrpcApplication&&)=delete;
};
#endif 
