# Lrpc 源码精读

这份文档是给你做“第二遍阅读”用的。

第一遍你需要知道项目在做什么。
第二遍你需要知道每个核心类为什么存在、入口在哪里、数据怎么流动、哪些地方是框架的关键抽象。

这份文档按“从外到内、从调用到实现”的顺序展开。建议你打开源码，一边看文档一边跳文件。

## 1. 先用一句话概括整个项目

Lrpc 是一个基于 protobuf、muduo 和 ZooKeeper 的简化 RPC 框架。

它完成了 4 件事：

1. 用 protobuf 生成统一的服务接口和客户端 stub。
2. 用自定义协议把方法调用封装成网络请求。
3. 用 muduo 处理 TCP 收发和并发连接。
4. 用 ZooKeeper 做服务注册与发现。

如果你忘了全局结构，就回到这 4 句话。

## 2. 你应该先抓住的主调用链

一次 `stub.Login(...)` 请求的完整路径是：

1. 客户端业务代码调用 protobuf 生成的 Stub。
2. Stub 最终调用你实现的 `LrpcChannel::CallMethod`。
3. `LrpcChannel` 查询服务地址、建立连接、打包请求、发送网络数据。
4. 服务端 `LrpcProvider::OnMessage` 收包并解协议。
5. `LrpcProvider` 根据 service 名和 method 名找到目标业务对象。
6. 通过 protobuf 反射调用真正的业务函数。
7. 业务函数填充 response 后执行 `done->Run()`。
8. 框架回调发送响应，客户端解析 response。

你后面读任何一个文件，都要不断问自己：

1. 这个文件处于上面哪一步？
2. 它依赖谁？
3. 它把数据交给谁？

## 3. 先看业务示例，理解“框架怎么被使用”

先读这 3 个文件：

1. `example/user.proto`
2. `example/caller/Lclient.cc`
3. `example/callee/Lserver.cc`

### 3.1 `example/user.proto`

这是整个示例的接口定义。你要重点看 3 类内容：

1. 请求消息
2. 响应消息
3. service 定义

关键点：

1. `LoginRequest` 和 `LoginResponse` 描述了方法参数和返回值。
2. `UserServiceRpc` 定义了服务名和方法名。
3. protobuf 会基于这个 service 生成服务端基类和客户端 stub。

你要真正理解的一点是：

RPC 框架不是凭空知道“调用哪个方法”的，而是依赖 `.proto` 生成的元信息。

### 3.2 `example/caller/Lclient.cc`

这是客户端示例，也是压测入口。

这个文件主要做了 4 件事：

1. 初始化框架配置。
2. 创建 protobuf Stub。
3. 构造 request 并发起 RPC 调用。
4. 统计请求成功数、失败数和 QPS。

你读这个文件时要特别留意两个层次。

第一个层次是业务调用层：

1. `Kuser::UserServiceRpc_Stub stub(new LrpcChannel(false));`
2. `stub.Login(&controller, &request, &response, nullptr);`

这两行非常关键。

它说明 protobuf stub 本身不负责网络，它只是把调用委托给 `RpcChannel`。
你的 `LrpcChannel` 正是框架和 protobuf 接口之间的桥。

第二个层次是压测层：

1. 多线程并发发请求。
2. 每个线程复用一个 stub 和一个 channel。
3. 每次请求前调用 `controller.Reset()`。

这里你需要理解：

1. 为什么 controller 要 Reset。
2. 为什么一个线程复用一个 channel 可以减少建连开销。
3. 为什么去掉逐请求打印后 QPS 大幅提升。

### 3.3 `example/callee/Lserver.cc`

这是服务端业务示例。

这个文件最重要的不是业务逻辑，而是你能看到 protobuf service 在 RPC 场景下的使用方式。

你会看到两种 `Login`：

1. 业务逻辑版 `Login(string, string)`
2. RPC 接入版 `Login(controller, request, response, done)`

这两者的职责完全不同：

1. 前者只处理业务。
2. 后者负责把 protobuf request 转换成业务输入，再把业务结果写回 protobuf response。

读这个文件时要理解一个设计原则：

框架层只负责通信和分发，真正的业务逻辑始终放在服务实现类里。

## 4. 初始化模块：框架是怎么启动起来的

这里对应 3 个文件：

1. `src/Lrpcapplication.cc`
2. `src/Lrpcconfig.cc`
3. `bin/test.conf`

### 4.1 `src/Lrpcapplication.cc`

这个类是全局入口管理器。

它主要做两件事：

1. 解析命令行参数，找到配置文件路径。
2. 提供全局可访问的配置对象。

你应该重点理解这几个函数。

#### `Init`

职责：

1. 通过 `getopt` 解析 `-i` 参数。
2. 读取配置文件路径。
3. 调用配置对象加载文件。

设计意图：

让所有程序入口统一使用同一种启动方式，例如：

`./server -i test.conf`

#### `GetInstance`

这是一个带互斥锁保护的单例入口。

注意：

1. 这个单例并不是为了存业务状态。
2. 它更多是为了给全局配置提供统一访问方式。
3. `atexit(deleteInstance)` 让进程退出时自动清理单例对象。

你读这里时要想清楚：

这个项目为什么选择单例而不是把配置对象层层传参？

答案通常是“简单”，但代价是全局依赖变多。

### 4.2 `src/Lrpcconfig.cc`

这个类负责配置文件解析。

整体逻辑很直接：

1. 打开配置文件。
2. 逐行读取。
3. 去除前后空格。
4. 过滤空行和注释。
5. 按 `key=value` 存入 `unordered_map`。

这里你要特别关注两个点。

#### 第一，`Trim` 的职责很基础但很重要

配置模块的稳定性，往往不在复杂逻辑，而在各种边界处理。

比如：

1. 前后空格
2. 空行
3. 行尾换行

如果这些处理不干净，后面配置读取看起来像“偶现问题”。

#### 第二，配置模块是全框架的依赖源

后续这些模块都依赖配置：

1. 服务端读取监听 IP 和端口。
2. ZooKeeper 客户端读取注册中心地址。

所以你可以把配置层理解为“框架启动依赖注入的最简版本”。

## 5. 客户端核心：`LrpcChannel` 为什么是整个框架最重要的类

文件：

1. `src/include/Lrpcchannel.h`
2. `src/Lrpcchannel.cc`

这是整个客户端调用链的核心。

### 5.1 这个类的职责

`LrpcChannel` 继承自 protobuf 的 `google::protobuf::RpcChannel`。

这意味着它的职责不是“表示一个 socket”，而是“把一次抽象的 RPC 方法调用翻译成一次真实的网络请求”。

你可以把它理解成：

`protobuf stub <-> LrpcChannel <-> TCP`

### 5.2 成员变量在表达什么

主要成员有：

1. `m_clientfd`：底层 TCP 连接。
2. `service_name`：当前调用的服务名。
3. `method_name`：当前调用的方法名。
4. `m_ip` 和 `m_port`：目标服务地址。
5. `m_idx`：解析 `ip:port` 字符串时分隔符位置。

这里要注意一点：

这个 channel 当前更接近“单服务单连接”的实现，而不是成熟 RPC 框架中的连接池或连接复用管理器。

### 5.3 `CallMethod` 是怎么工作的

这是整份源码里最值得反复看的函数。

建议你按下面 6 段拆开理解。

#### 第一段：懒连接

如果 `m_clientfd == -1`，说明当前还没有建立连接。

于是函数会：

1. 从 `MethodDescriptor` 拿到服务名和方法名。
2. 创建 `ZkClient`。
3. 去 ZooKeeper 查 `/service_name/method_name` 对应的地址。
4. 拿到 `ip:port` 后建立 TCP 连接。

这段代码反映出一个设计选择：

客户端第一次发请求时才做服务发现和建连，而不是在 stub 创建时就建连。

优点：

1. 初始化轻量。
2. 不浪费无用连接。

缺点：

1. 首次调用延迟更高。
2. 没有连接池。
3. 没有多节点负载均衡。

#### 第二段：请求序列化

框架把 request protobuf 对象序列化成 `args_str`。

这个动作的意义是：

protobuf 负责把结构化数据转成字节流，框架不关心字段内部如何编码。

#### 第三段：协议头构建

框架定义了一个 `RpcHeader`，包含：

1. `service_name`
2. `method_name`
3. `args_size`

注意这一步很关键：

如果没有协议头，服务端虽然能收到 protobuf 字节流，但不知道：

1. 这是哪个服务的请求。
2. 这是哪个方法。
3. 请求体有多长。

#### 第四段：自定义报文格式

当前请求包格式是：

1. 4 字节总长度 `total_len`
2. 4 字节头部长度 `header_len`
3. 变长 header 数据
4. 变长 args 数据

这个设计解决了两个问题：

1. 服务端知道如何拆包。
2. 服务端知道 header 和 body 的边界。

这个格式是你必须完全吃透的，因为服务端 `OnMessage` 会按这个格式反向解析。

#### 第五段：发送和接收

这里现在用了：

1. `send_exact`
2. `recv_exact`

这两个函数的意义是：

TCP 是字节流，不保证一次 `send` 或一次 `recv` 就把完整报文传完。

这也是 RPC 框架和“写个简单 socket demo”之间最大的差别之一。

#### 第六段：响应反序列化

客户端收到响应后，先读 4 字节长度，再按长度读取 response body，最后用 protobuf 反序列化成响应对象。

你要记住：

客户端和服务端并不是直接交换对象，而是交换“符合协议的字节流”。

### 5.4 `QueryServiceHost` 在干什么

这个函数做了两件事：

1. 把服务名和方法名拼成 ZooKeeper 路径。
2. 从节点数据里拿到 `ip:port`。

这里的路径设计是：

1. 服务层节点：`/UserServiceRpc`
2. 方法层节点：`/UserServiceRpc/Login`

也就是说，当前服务发现粒度是“方法级地址发现”。

你可以思考一个问题：

如果一个服务的多个方法都部署在同一个进程里，按方法粒度注册是不是有点冗余？

这是后续可优化点之一。

## 6. 服务端核心：`LrpcProvider` 怎么把网络请求变成函数调用

文件：

1. `src/include/Lrpcprovider.h`
2. `src/Lrpcprovider.cc`

### 6.1 这个类的职责

`LrpcProvider` 是服务端框架入口。

它主要负责：

1. 保存已发布服务。
2. 启动 muduo TCP 服务。
3. 注册服务到 ZooKeeper。
4. 收包、解包、查找目标方法。
5. 调用业务实现并发送响应。

### 6.2 `ServiceInfo` 和 `service_map` 是整个服务端分发表

数据结构：

1. `service_map` 是 `service_name -> ServiceInfo`
2. `ServiceInfo` 里有：
   `service`：业务服务对象
   `method_map`：`method_name -> MethodDescriptor`

这相当于一个两级路由表：

1. 先按服务名找到服务对象。
2. 再按方法名找到 protobuf 方法描述符。

你可以把它理解成最简单的 RPC 路由中心。

### 6.3 `NotifyService` 的本质是“把服务注册到内存”

这个函数不是注册到 ZooKeeper，而是先注册到当前进程的内存结构里。

它做了 3 件事：

1. 调 `service->GetDescriptor()` 拿到 protobuf service 描述信息。
2. 遍历 service 中所有 method。
3. 把 service 指针和 method 描述符存入 `service_map`。

你要特别理解 protobuf 反射在这里的价值：

框架不需要写死 `Login`、`Register` 这些函数名，它只依赖 protobuf 提供的元信息动态调度。

### 6.4 `Run` 的职责是“启动服务端运行环境”

这个函数做了 5 件大事：

1. 从配置读取服务监听地址。
2. 创建 muduo 的 `TcpServer`。
3. 绑定连接回调和消息回调。
4. 把已发布服务注册到 ZooKeeper。
5. 启动事件循环。

这里你要注意两个层面的注册：

1. 内存注册：`NotifyService`
2. 注册中心注册：`Run` 里面调用 `zkclient.Create`

这两层缺一不可。

没有内存注册，服务端收到了请求也不知道怎么调。
没有 ZooKeeper 注册，客户端就找不到服务地址。

### 6.5 `OnMessage` 是服务端最关键的函数

这几乎是整个服务端的“请求入口”。

你应该按下面顺序理解。

#### 第一，为什么用 `while (buffer->readableBytes() >= 4)`

因为这是基于 TCP 的字节流协议，muduo 的 `Buffer` 可能一次读进来：

1. 半个包
2. 一个完整包
3. 多个包

所以这里必须循环处理缓冲区，才能解决粘包拆包问题。

#### 第二，先看 `total_len`

服务端先从缓冲区头部读 4 字节总长度，但还不急着真正消费业务数据。

它会先判断：

当前 buffer 中的可读字节是否已经足够组成一个完整包。

如果不够，就直接 `break`，等待下一次网络数据到达。

这一步是理解 TCP 粘包拆包最核心的地方。

#### 第三，再看 `header_len`

取到完整包后，继续读取 4 字节 `header_len`，然后做合法性校验。

当前代码已经补了一个保护：

1. `total_len < 4` 视为非法包。
2. `header_len > total_len - 4` 视为非法包。

这是协议边界保护，避免坏包把后续解析带崩。

#### 第四，解析 header 和 body

当前 body 大小通过：

`args_size = total_len - 4 - header_len`

注意这里的 `4` 指的是 `header_len` 字段本身所占的 4 字节。

于是服务端就能准确切出：

1. header protobuf 字节流
2. args protobuf 字节流

#### 第五，根据服务名和方法名做路由

header 反序列化后，拿到：

1. `service_name`
2. `method_name`

然后去 `service_map` 里查。

这一步本质上就是 RPC 版的路由分发。

#### 第六，用 protobuf 反射创建 request 和 response

这里很重要。

框架并不知道目标方法的参数类型是什么，但它可以通过：

1. `GetRequestPrototype(method).New()`
2. `GetResponsePrototype(method).New()`

动态创建正确类型的 request 和 response。

这就是 protobuf 反射让“通用 RPC 框架”成立的原因。

#### 第七，为什么要有 `RpcResponseClosure`

这是你当前版本很值得学习的一点。

服务端调用业务函数时传入了一个 `done`，当业务函数执行完后调用 `done->Run()`。

`RpcResponseClosure` 封装了 3 件事：

1. 发送 response
2. 释放 request
3. 释放 response

这解决了之前长压测下的对象泄漏问题。

这个设计体现了一个很典型的异步框架思想：

业务代码只负责“完成处理”，资源回收和响应发送由框架回调托管。

### 6.6 `SendRpcResponse`

这个函数职责很单纯：

1. 把 response 序列化成字符串。
2. 在前面加 4 字节长度。
3. 调 muduo 的 `conn->send` 发回客户端。

你会发现响应协议比请求协议简单，因为服务端回包不需要再带 service 名和 method 名。

原因很简单：

客户端此时已经知道自己在等哪个调用的结果。

## 7. 注册中心模块：为什么客户端和服务端都要依赖 ZooKeeper

文件：

1. `src/include/zookeeperutil.h`
2. `src/zookeeperutil.cc`

### 7.1 `ZkClient` 的职责

这个类封装了 ZooKeeper C API，当前只暴露 3 个核心动作：

1. `Start`：建立连接
2. `Create`：创建节点
3. `GetData`：读取节点数据

### 7.2 服务端怎么用它

服务端在 `Run` 里做的是：

1. 创建服务名永久节点
2. 创建方法名临时节点
3. 节点数据里写入 `ip:port`

这意味着：

1. 服务名是目录概念
2. 方法名节点是真正承载服务地址的节点

临时节点的意义是：

服务端进程挂掉后，ZooKeeper 会自动删除节点，客户端下次查询时不会继续拿到失效地址。

### 7.3 客户端怎么用它

客户端在首次调用时通过 `GetData(method_path)` 查到服务地址。

当前实现是“请求时查询，再本地持有连接”，属于最简单可用版本。

### 7.4 这里有哪些值得你注意的实现特点

1. `global_watcher` 用全局条件变量通知连接成功。
2. `is_connected` 是全局状态，而不是每个 `ZkClient` 私有状态。
3. 当前实现够用，但并不适合更复杂的并发场景。

这正是你以后可以继续优化的方向。

## 8. 控制器模块：`Lrpccontroller` 为什么存在

文件：

1. `src/include/Lrpccontroller.h`
2. `src/Lrpccontroller.cc`

它是 protobuf `RpcController` 的一个轻量实现。

当前主要功能是：

1. 记录调用是否失败。
2. 记录错误文本。

这部分实现不复杂，但概念上很重要。

因为 RPC 和本地函数不同，本地函数出错通常靠返回值或异常，RPC 框架需要一个统一的“调用上下文对象”来传递状态。

你当前压测里每次调用前都 `Reset`，就是在复用这个调用上下文。

## 9. 协议模块：为什么一定要单独看 `Lrpcheader.proto`

文件：

1. `src/Lrpcheader.proto`

这个 proto 不是业务协议，而是框架协议头。

它负责表达 3 个字段：

1. 服务名
2. 方法名
3. 参数体长度

这个设计说明一个重要思想：

框架协议和业务协议是分层的。

1. 业务协议描述“参数是什么”。
2. 框架协议描述“把参数交给谁处理”。

成熟 RPC 框架里通常也都有类似的元信息层。

## 10. 你现在应该怎样做源码精读

不要想着把所有文件一口气看完。建议你按下面顺序逐步精读。

### 第一轮：只追主线

顺序：

1. `example/caller/Lclient.cc`
2. `src/Lrpcchannel.cc`
3. `src/Lrpcprovider.cc`
4. `example/callee/Lserver.cc`

目标：

只回答“请求怎么走”。

### 第二轮：补配套模块

顺序：

1. `src/Lrpcapplication.cc`
2. `src/Lrpcconfig.cc`
3. `src/zookeeperutil.cc`
4. `src/Lrpccontroller.cc`

目标：

回答“主线依赖什么环境和基础能力”。

### 第三轮：带着问题反读

你要带着这些问题再读一次：

1. 为什么客户端要继承 `RpcChannel`？
2. 为什么服务端要继承 protobuf service 基类？
3. 为什么服务发现不直接写死地址？
4. 为什么收包一定要先看长度？
5. 为什么框架要依赖 protobuf 反射？

## 11. 最适合你的断点位置

建议你按下面顺序打断点：

1. `example/caller/Lclient.cc` 的 `stub.Login(...)`
2. `src/Lrpcchannel.cc` 的 `CallMethod`
3. `src/Lrpcchannel.cc` 的 `send_exact`
4. `src/Lrpcprovider.cc` 的 `OnMessage`
5. `example/callee/Lserver.cc` 的 RPC `Login`
6. `src/Lrpcprovider.cc` 的 `SendRpcResponse`

这样你可以完整看到：

1. 请求对象在客户端如何序列化
2. 报文是怎样进网络层的
3. 服务端怎样解包
4. 业务函数什么时候真正执行
5. 响应什么时候回到客户端

## 12. 你做源码精读时要重点记录什么

建议你自己准备一个笔记，至少记录下面 5 类内容：

1. 每个核心类的职责
2. 每个核心函数的输入和输出
3. 请求协议格式
4. 服务端路由表结构
5. 你认为可以继续优化的点

如果你能把这 5 类内容写出来，说明你已经不是“看过代码”，而是真的吃透了主干。

## 13. 当前代码里你要特别理解的 6 个设计点

### 设计点 1：protobuf 不是网络层，而是接口层和序列化层

很多初学者会把 protobuf 误以为是“RPC 本身”。

其实在这个项目里，protobuf 负责：

1. 定义接口
2. 生成 stub 和 service 基类
3. 负责消息序列化和反序列化

真正的网络发送、服务发现、分发路由，都是你自己的框架代码完成的。

### 设计点 2：`RpcChannel` 是客户端桥接层

stub 并不知道 socket、ZooKeeper、muduo，它只知道自己有一个 channel 可以发请求。

这正是抽象的力量。

### 设计点 3：服务端核心不是业务函数，而是 `OnMessage`

业务函数只是被调用者。

真正让 RPC 框架成立的是：

1. 协议解析
2. 路由查找
3. 反射创建对象
4. 调用调度

这些都发生在 `OnMessage`。

### 设计点 4：协议长度字段是 TCP 稳定收发的基础

没有长度字段，你就无法可靠解决：

1. 半包
2. 粘包
3. 多包连在一起

### 设计点 5：注册中心把“地址感知”从代码里抽离了

如果没有 ZooKeeper，客户端只能写死服务端地址。

有了注册中心，服务地址可以动态变化，框架具备了微服务化的基础形态。

### 设计点 6：回调对象承担资源生命周期管理

`RpcResponseClosure` 不只是一个回调，它还是服务端请求生命周期结束时的“清理器”。

这在网络框架里是很常见的模式。

## 14. 当前项目你可以继续深挖的方向

当你读懂现有代码后，可以继续思考这些方向：

1. 客户端连接池怎么设计
2. 服务地址缓存怎么设计
3. 多节点负载均衡怎么做
4. 超时控制和重试机制怎么做
5. 服务端线程模型是否要可配置
6. watcher 机制如何让客户端动态感知节点变化
7. 如何支持异步 RPC

这些问题没有标准答案，但每一个都能把你从“会看源码”推向“会设计框架”。

## 15. 你下一步最合适的学习动作

建议你按这个顺序继续：

1. 先把这份文档配合源码完整走一遍。
2. 然后让我带你做一次“逐函数精讲”。
3. 再让我带你新增一个新的 RPC 方法。
4. 最后把项目初始化 Git 和 GitHub，开始按版本迭代。

如果你继续往下学，下一份最有价值的内容不是再写概念文档，而是我直接带你做“逐文件逐函数讲解”。