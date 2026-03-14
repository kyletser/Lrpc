# Lrpc 项目学习指南

这份文档的目标不是解释所有细节，而是给你一条能真正走通的学习路径。按这份顺序学，你会先建立整体认知，再逐步吃透实现，最后能自己改、自己测、自己扩展。

## 1. 先明确这个项目在做什么

这个项目实现的是一个简化版 C++ RPC 框架，核心流程是：

1. 客户端像调用本地函数一样调用 protobuf 生成的 Stub 方法。
2. Stub 把调用转交给自定义的 `LrpcChannel`。
3. `LrpcChannel` 负责服务发现、请求序列化、协议封装、网络发送、响应接收。
4. 服务端 `LrpcProvider` 接收请求，解包后找到对应的 Service 和 Method。
5. 服务端调用真正的业务函数。
6. 业务函数把结果写入 response，再由框架序列化返回给客户端。

你只要真正看懂这 6 步，整个项目的主线就清楚了。

## 2. 学习顺序

不要从头文件一行一行死读。建议按下面顺序阅读。

### 第一层：先看怎么用

先看示例代码，理解“用户是怎么使用这个框架的”。

- `example/callee/Lserver.cc`
- `example/caller/Lclient.cc`
- `example/user.proto`

你要回答这几个问题：

1. 服务是怎么定义的？
2. 客户端为什么能直接 `stub.Login(...)`？
3. 服务端为什么只要 `NotifyService(new UserService())` 就能发布服务？

### 第二层：看框架初始化

再看配置和启动入口。

- `src/Lrpcapplication.cc`
- `src/Lrpcconfig.cc`
- `bin/test.conf`

你要搞清楚：

1. 程序怎么拿到配置文件路径？
2. 配置项怎么进入框架？
3. 哪些模块依赖这些配置？

### 第三层：看客户端一次 RPC 怎么发出去

这是最关键的一层。

- `src/Lrpcchannel.cc`
- `src/include/Lrpcchannel.h`
- `src/Lrpcheader.proto`

阅读时只盯住 `CallMethod`，把它拆成以下几个阶段：

1. 从 protobuf 的 `MethodDescriptor` 里拿到服务名和方法名。
2. 去 ZooKeeper 查服务地址。
3. 建立 TCP 连接。
4. 序列化 request。
5. 组装自定义协议头。
6. 发送数据。
7. 接收响应。
8. 反序列化 response。

建议你边看边自己画时序图：

`client stub -> LrpcChannel -> ZooKeeper -> TCP -> LrpcProvider -> UserService -> response`

### 第四层：看服务端怎么收包和分发

- `src/Lrpcprovider.cc`
- `src/include/Lrpcprovider.h`

这里重点盯两个函数：

1. `NotifyService`
2. `OnMessage`

你需要弄懂：

1. `NotifyService` 是怎么把服务注册到内存中的？
2. `OnMessage` 是怎么解决粘包拆包的？
3. 服务端怎么根据 service_name 和 method_name 找到目标函数？
4. protobuf 的反射机制是怎么用在这里的？

### 第五层：看注册中心

- `src/zookeeperutil.cc`
- `src/include/zookeeperutil.h`

这一层只需要看清楚两件事：

1. 服务端如何把服务注册到 ZooKeeper。
2. 客户端如何从 ZooKeeper 查询服务地址。

不要一开始陷进 ZooKeeper 细节。它在这个项目里是“服务发现工具”，不是主角。

### 第六层：看控制器和错误处理

- `src/Lrpccontroller.cc`
- `src/include/Lrpccontroller.h`

这里实现很轻，主要是理解 `RpcController` 在调用失败时扮演什么角色。

## 3. 你应该边学边做的事情

如果你只是读，很容易“看过了但没真正理解”。建议你按下面方式学习。

### 第一步：自己手写一次请求链路

不要看源码，先自己写出下面这条链路：

1. 客户端调用 Stub。
2. Stub 调用 Channel。
3. Channel 发 TCP 请求。
4. Provider 解包。
5. Provider 调业务函数。
6. 业务函数写 response。
7. Provider 回包。
8. 客户端解析响应。

如果你能不看代码写出来，说明你真正建立了主线认知。

### 第二步：用调试器单步跟一次请求

建议打断点的位置：

1. `example/caller/Lclient.cc` 中 `stub.Login(...)`
2. `src/Lrpcchannel.cc` 中 `CallMethod`
3. `src/Lrpcprovider.cc` 中 `OnMessage`
4. `example/callee/Lserver.cc` 中业务 `Login`

单步调一次，你会比读 1000 行注释更快明白这个框架怎么工作。

### 第三步：抓包或打印协议字段

建议你自己验证这几个字段：

1. 总长度 `total_len`
2. 头部长度 `header_len`
3. `service_name`
4. `method_name`
5. 请求体字节长度

你要确认“自定义协议到底长什么样”。理解协议后，很多网络代码就不神秘了。

## 4. 每个核心文件应该怎么看

### `example/caller/Lclient.cc`

你要看的是两层：

1. 业务层如何构造 request 和读取 response。
2. 测试层如何并发发请求和统计 QPS。

你已经测到 4 万到 5 万 QPS，这个文件也是压测入口。以后你改框架，优先用它做回归测试。

### `example/callee/Lserver.cc`

这里是业务服务实现。重点不是业务逻辑本身，而是 protobuf 生成的 RPC service 类如何被你继承。

你要搞懂为什么会同时有：

1. 一个本地 `Login(string, string)`
2. 一个 RPC 重写版 `Login(controller, request, response, done)`

前者是业务逻辑，后者是 RPC 框架接入点。

### `src/Lrpcchannel.cc`

这是客户端核心文件，也是你最应该反复读的文件。

重点关注：

1. 服务发现
2. 建连逻辑
3. 请求打包
4. 响应收包
5. 失败处理

你后续如果想继续做连接池、超时控制、负载均衡、异步 RPC，基本都要从这里演进。

### `src/Lrpcprovider.cc`

这是服务端核心。

重点关注：

1. Service 注册表结构
2. `OnMessage` 的解包过程
3. 反射调用 `CallMethod`
4. response 的发送与资源释放

后续想做线程池优化、请求队列、限流、鉴权，也主要从这里扩展。

### `src/zookeeperutil.cc`

这里只要先理解连接、创建节点、读取节点三件事。等你后续想做 watcher 刷新和服务动态上下线，再回来深挖。

## 5. 这个项目最值得你思考的问题

学习时不要只问“代码怎么写”，要问“为什么这么设计”。建议你认真思考这些问题。

1. 为什么客户端使用 protobuf 的 Stub，而不是自己直接写 socket 调用？
2. 为什么框架要自己定义协议头，而不是直接裸发 protobuf 数据？
3. 为什么服务端要把 service 和 method 存在 map 里？
4. 为什么要借助 ZooKeeper，而不是把 IP 和端口写死？
5. 为什么服务端可以通过反射拿到 request/response 原型？
6. 如果一台服务机器挂掉，客户端如何感知？
7. 如果一个方法调用超时，现在这套代码怎么处理？
8. 如果要支持连接池或异步调用，现在哪些类需要改？

这类问题能帮你从“会看代码”进阶到“会设计框架”。

## 6. 你接下来 7 天可以怎么学

### 第 1 天：只看示例和主流程

目标：知道一次 RPC 从哪来、到哪去。

任务：

1. 通读 `example` 目录。
2. 跟着 `stub.Login(...)` 找到 `CallMethod`。
3. 写出完整时序图。

### 第 2 天：吃透客户端发送流程

目标：搞清楚请求怎么被序列化和发送。

任务：

1. 读 `src/Lrpcchannel.cc`。
2. 画出请求报文格式。
3. 自己解释 `total_len`、`header_len`、`args_size` 的意义。

### 第 3 天：吃透服务端分发流程

目标：搞清楚服务端怎么找到并调用目标函数。

任务：

1. 读 `src/Lrpcprovider.cc`。
2. 理解 `NotifyService` 和 `OnMessage`。
3. 弄懂 protobuf descriptor 和 reflection 在这里的作用。

### 第 4 天：吃透注册中心

目标：理解服务发现。

任务：

1. 读 `src/zookeeperutil.cc`。
2. 自己手动画出 ZooKeeper 上的节点结构。
3. 搞清楚永久节点和临时节点分别用在哪里。

### 第 5 天：调试和验证

目标：把理论和运行结果对应起来。

任务：

1. 打断点单步调一次请求。
2. 观察 request 和 response 的真实内容。
3. 观察客户端和服务端日志时序。

### 第 6 天：开始做一个小扩展

推荐做以下任意一个：

1. 给 `UserService` 新增一个 RPC 方法。
2. 增加一个新的业务服务。
3. 给压测程序加参数，支持配置线程数和请求数。

### 第 7 天：做一次复盘

写一篇你自己的总结，回答：

1. 这个框架最核心的 3 个类是什么？
2. 哪段代码你觉得最关键？
3. 哪段代码你觉得设计还可以继续优化？
4. 如果让你重写一版，你会先保留什么、重构什么？

## 7. 你现在最适合做的练习

建议你按难度从低到高做。

### 练习 1：新增一个业务方法

比如新增 `GetUserInfo`。

你需要改：

1. `example/user.proto`
2. `example/callee/Lserver.cc`
3. `example/caller/Lclient.cc`

做完这个练习，你会真正熟悉 protobuf service 和整个 RPC 路径。

### 练习 2：新增一个服务

比如新增一个 `FriendService`。

这个练习能帮你看懂 `NotifyService` 为什么是按 service 维度注册。

### 练习 3：给客户端压测程序加命令行参数

把线程数和请求数做成可配置。

你会更熟悉：

1. 压测程序结构
2. 多线程测试方式
3. 性能回归方法

### 练习 4：给框架加超时控制

这是从“读懂”进入“真正改框架”的第一步。

## 8. 学这个项目时要避免的误区

1. 不要一开始就纠结所有细枝末节。
2. 不要先钻 ZooKeeper 文档，先把主调用链走通。
3. 不要只看注释，要实际运行、打断点、改代码。
4. 不要试图一次性把所有源码背下来，先建立模块边界。
5. 不要只盯业务示例，要回到框架核心类里理解抽象。

## 9. 学会这个项目之后你应该具备什么能力

如果你真的吃透了这个项目，你应该能做到：

1. 自己解释一次 RPC 请求从客户端到服务端再返回的全过程。
2. 自己新增一个 service 和 method。
3. 自己修改协议头并同步改收发逻辑。
4. 自己定位请求失败是在序列化、网络、发现还是分发阶段。
5. 自己继续做性能优化，例如连接池、缓存、超时、异步化。

## 10. 建议你下一步怎么做

最建议的做法是按下面节奏推进：

1. 先照这份文档把主流程彻底看懂。
2. 然后让我带你逐文件讲一遍源码。
3. 再让我带你做一次“新增 RPC 方法”的实战。
4. 最后把项目初始化 Git 并推到 GitHub，开始按版本演进。

如果你愿意，下一步我可以继续直接给你写第 2 份文档：`源码精读版`，按文件逐个解释每个核心函数是干什么的。