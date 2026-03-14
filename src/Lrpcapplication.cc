/*
 * LrpcApplication.cc
    *
    * Created on: 2026年3月5日
    * Author:LXP
    * Description: 全局入口管理器,负责解析命令行参数，加载配置文件，提供全局访问接口
 */
#include "Lrpcapplication.h"
#include<cstdlib>
#include<unistd.h>

Lrpcconfig LrpcApplication::m_config;  // 全局配置对象
std::mutex LrpcApplication::m_mutex;  // 用于线程安全的互斥锁
LrpcApplication* LrpcApplication::m_application = nullptr;  // 单例对象指针，初始为空

// 初始化函数，用于解析命令行参数并加载配置文件,让所有程序入口统一使用同一种启动方式
void LrpcApplication::Init(int argc, char **argv) {
    if (argc < 2) {  // 如果命令行参数少于2个，说明没有指定配置文件
        std::cout << "格式: command -i <配置文件路径>" << std::endl;
        exit(EXIT_FAILURE);  // 退出程序
    }

    int o;
    std::string config_file;
    // 使用getopt解析命令行参数，-i表示指定配置文件
    while (-1 != (o = getopt(argc, argv, "i:"))) {
        switch (o) {
            case 'i':  // 如果参数是-i，后面的值就是配置文件的路径
                config_file = optarg;  // 将配置文件路径保存到config_file
                break;
            case '?':  // 如果出现未知参数（不是-i），提示正确格式并退出
                std::cout << "格式: command -i <配置文件路径>" << std::endl;
                exit(EXIT_FAILURE);
                break;
            case ':':  // 如果-i后面没有跟参数，提示正确格式并退出
                std::cout << "格式: command -i <配置文件路径>" << std::endl;
                exit(EXIT_FAILURE);
                break;
            default:
                break;
        }
    }

    // 加载配置文件
    m_config.LoadConfigFile(config_file.c_str());
}

// 获取单例对象的引用，保证全局只有一个实例
LrpcApplication &LrpcApplication::GetInstance() {
    std::lock_guard<std::mutex> lock(m_mutex);  // 加锁，保证线程安全
    if (m_application == nullptr) {  // 如果单例对象还未创建
        m_application = new LrpcApplication();  // 创建单例对象
        atexit(deleteInstance);  // 注册atexit函数，程序退出时自动销毁单例对象
    }
    return *m_application;  // 返回单例对象的引用
}

// 程序退出时自动调用的函数，用于销毁单例对象
void LrpcApplication::deleteInstance() {
    if (m_application) {  // 如果单例对象存在
        delete m_application;  // 销毁单例对象
    }
}

// 获取全局配置对象的引用
Lrpcconfig& LrpcApplication::GetConfig() {
    return m_config;
}