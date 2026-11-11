# muduo-core 源码学习总指南（请先读这个文件）

> 这套注释的目标：**不买书、不看视频、不翻博客，只在这个仓库里就能读懂 muduo 的核心设计，并扛得住面试追问。**
> 如果你 C++ 和网络编程基础都还薄，就从下面的「阶段 0」开始，一步一步走，不要跳。

---

## 0. 三步上手（今天就能做完）

1. **先让它跑起来**（10 分钟，建立体感）：
   - WSL/Ubuntu 里执行 `cmake -S . -B build && cmake --build build -j`
   - 起服务：`./example/testserver`
   - 另开终端：`nc 127.0.0.1 8080`，随便敲一行字回车，看它回显，同时看服务端刷的日志
   - 完整命令、日志逐行解读、gdb 断点清单都在 `docs/experiments.md`
2. **通读本指南第 2 节的阶段表**，知道自己在哪一站、下一站是什么。
3. **从阶段 1 开始按顺序读代码**。每读完一个文件，回答文件头 `【自检】` 里的 2~3 个问题；答不出就再看一遍该文件的 `【设计】`/`【流程】` 注释。

---

## 1. 先做前置知识自评（决定你要不要先补课）

| # | 能力项 | 自查方法（30 秒） | 不会怎么办 |
|---|---|---|---|
| 1 | 会写普通 socket 服务器（socket/bind/listen/accept/read/write） | 能说出 `accept` 返回的新 fd 与监听 fd 的区别 | 看 `docs/prerequisites.md` 第 1 节 |
| 2 | 懂 epoll（epoll_create/ctl/wait、LT 与 ET） | 能说出 `epoll_wait` 的第二个、第三个参数分别是什么 | 看第 2 节 |
| 3 | 懂 TCP 常见状态与半关闭 | 能说出 `TIME_WAIT` 是谁产生的、为什么 `SO_REUSEADDR` 有用 | 看第 3 节 |
| 4 | 会 pthread/std::thread、互斥锁、条件变量 | 能说出 `cond.wait(lk, pred)` 为什么要带谓词 | 看第 4 节 |
| 5 | C++ 智能指针与函数对象 | 能说出 `enable_shared_from_this` 的使用前提和 `weak_ptr::lock()` 的作用 | 看第 5 节 |
| 6 | 懂回调 / 事件驱动编程范式 | 能画出 Reactor 的三个组成部分 | 看第 6 节 |

说明：第 5、6 项几乎没人一开始就会 —— 这正是 muduo 值得读的原因。**第 1~4 项全空的话，先花 1 天补第 1、2、4 节，再进阶段 2 之后，否则会读得很痛苦。**

---

## 2. 阅读顺序总表（8 个阶段，总计约 24 小时，可拆成两周晚上下班后）

| 阶段 | 主题 | 按这个顺序读文件 | 目标（读完你能回答） | 自检题 | 时长 |
|---|---|---|---|---|---|
| **0** | 先看终点：用户怎么用 | `example/testserver.cc` → `include/TcpServer.h`（只看 public 接口，不看 .cc） | 用户到底配置了什么？框架替我干了什么？ | EchoServer 设置了几类回调？`main` 里最后一行 `loop.loop()` 不写会发生什么？ | 30min |
| **1** | 地基层：时间、日志、线程号、禁止拷贝 | `include/noncopyable.h` → `include/Timestamp.h` → `src/Timestamp.cc` → `include/Logger.h` → `src/Logger.cc` → `include/CurrentThread.h` → `src/CurrentThread.cc` | 三个最外围小工具如何被全库到处使用 | `LOG_INFO` 外面为什么要包 `do{...}while(0)`？`__thread` 与 `static` 变量区别？ | 1.5h |
| **2** | OS 封装层：地址、fd 生命周期、缓冲区 | `include/InetAddress.h` → `src/InetAddress.cc` → `include/Socket.h` → `src/Socket.cc` → `include/Buffer.h` → `src/Buffer.cc` | 系统调用如何被封装成对象；字节流缓冲区怎么设计 | `Socket` 析构做了什么，为什么说「Channel 不关 fd」？`readv` 为什么要两块缓冲区？`kCheapPrepend` 给谁用？ | 3h |
| **3** | **Reactor 三件套（全库心脏）** | `include/Channel.h` → `src/Channel.cc` → `include/Poller.h` → `include/EPollPoller.h` → `src/EPollPoller.cc` → `src/DefaultPoller.cc` → `src/Poller.cc` → `include/EventLoop.h` → `src/EventLoop.cc` | 一个 fd 的事件如何被「注册 → 收集 → 分发 → 执行回调」 | `Channel::index_` 三个值分别代表什么状态？`events_` 与 `revents_` 谁写谁读？`EventLoop::loop()` 的三步循环是哪三步？`queueInLoop` 为什么要 `wakeup`？ | 6h |
| **4** | 线程模型：one loop per thread 落地 | `include/Thread.h` → `src/Thread.cc` → `include/EventLoopThread.h` → `src/EventLoopThread.cc` → `include/EventLoopThreadPool.h` → `src/EventLoopThreadPool.cc` | 线程、EventLoop、epoll 为什么必须一一对应；新连接如何分给某个线程 | `Thread::start()` 里的 `sem_wait` 去掉会怎样？`EventLoopThread::threadFunc` 里的 `EventLoop` 为什么是栈对象且不能 delete？`getNextLoop()` 用什么分配算法？ | 3h |
| **5** | 业务组装层：连接对象与服务端门面 | `include/Callbacks.h` → `include/TcpConnection.h` → `src/TcpConnection.cc` → `include/Acceptor.h` → `src/Acceptor.cc` → `include/TcpServer.h` → `src/TcpServer.cc` | 一次连接从 fd 到对象、从对象到销毁的完整一生 | 为什么 `TcpConnection` 必须继承 `enable_shared_from_this`？`channel_->tie()` 防的是什么事故？`shutdown` 之后对象在哪个线程销毁？`~TcpServer` 里 `item.second.reset()` 是什么操作？ | 6h |
| **6** | 串起来 + 眼见为实 | `docs/architecture.md`（对照 ①→⑫ 逐跳回源码）+ `docs/experiments.md` 的 gdb 章节 | 能在终端里亲眼看到 3 次线程跳转 | 在 `TcpServer::newConnection` 与 `TcpConnection::connectEstablished` 上打断点，两次是不是同一个线程？为什么？ | 3h |
| **7** | 拓展：能改才算会 | `docs/experiments.md` 的 6 个练习 + 陈硕原版 muduo 对照 | 能自己加定时器 / 异步日志 / 协议解析，能讲清与原版差异 | 挑一个练习做完跑通；能答出 `docs/experiments.md` 第 6 节 20 道面试题 | 持续 |

### 为什么是这个顺序

- **自顶向下看一次（阶段 0），自底向上读到底（阶段 1→5）**：先知道要去哪儿，再逐层往上搭；每一层只依赖前几层，不会读着读着冒出一个新概念。
- **`Channel`/`Poller`/`EventLoop` 必须在同一阶段读**：三者互相引用（`Channel` 要调 `EventLoop::updateChannel`，`Poller` 要认得 `Channel`，`EventLoop` 持有 `Poller`），拆开必然卡住。本项目用**前向声明**而不是互相 include 来解决这个环 —— 见各文件的 `【设计】` 注释。
- **`TcpConnection` 排在线程模型之后**：它的 `send`/`shutdown` 每一步都要判断「当前在不在 loop 所属线程」，不懂线程模型就看不懂它为什么这样写。
- **`Acceptor` 故意排在 `TcpConnection` 之后**：`Acceptor::handleRead` 的产出正是 `TcpConnection`（经 `TcpServer::newConnection` 包装），先懂连接才懂分发。

---

## 3. 注释标签说明 + 把 grep 当复习提纲

所有注释都以方括号标签开头，因此可以按标签检索复习：

| 标签 | 含义 | 复习时怎么用 |
|---|---|---|
| `【语法】` | C++ 语言点：这一行为什么这么写、去掉会怎样 | 遇到看不懂的语句就按标签捞 |
| `【系统调用】` | Linux API 的入参 / 返回值 / errno / 阻塞语义 | 背八股前扫一遍，防止 API 说错 |
| `【设计】` | 为什么这么分层、谁持有谁、所有权与线程归属 | 回答「讲讲你的项目设计」的素材库 |
| `【流程】` | 这一步在全链路 ①→⑫ 中的位置、上下游是谁 | 串链路专用 |
| `【线程】` | 明确标注在 mainLoop 还是 subLoop#N | 多线程部分只看这些标签就能通 |
| `【易错】` | 初学者误解、隐蔽前提、竞态 | 面试前必看 |
| `【⚠问题】` | 本项目相对原版的退化点 / 隐患（代码未改动） | **面试别把它当优点说**，见第 8 节 |
| `【对比原版】` | 陈硕原版 muduo 的做法与差异 | 准备「你和原版对比过吗」 |
| `【面试】` | 高频面试题 + 一句话答案要点 | 天然题库 |

每个文件顶部还有一段 `【文件】/【一句话】/【Reactor 角色】/【依赖】/【谁用它】/【先修知识】/【难度】/【建议耗时】/【自检】` 定位块，读新文件先花 30 秒读它。

检索示例：

```bash
cd /home/lee/projects/muduo-core
grep -rn 【面试】 include src | wc -l      # 全库埋了多少个面试点
grep -rn 【线程】 src | head -40           # 快速把所有线程边界过一遍
grep -rn 【⚠问题】 include src             # 所有隐患清单（面试前必读）
grep -rn 【流程】 src                      # 按 ①→⑫ 顺序重放整条链路
```

---
## 4. 必背主线：一个连接的完整一生（①→⑫）

下面这张表是**整个仓库的地图**。第 6 阶段要能做到不看代码、把这 12 步连同「在哪线程」讲出来。

| 步 | 发生了什么 | 在哪个线程 | 关键代码（文件 :: 函数） |
|---|---|---|---|
| ① | 用户在 `main` 里造一个 `EventLoop`（这就是 mainLoop） | 主线程 | `EventLoop::EventLoop`（内部建 `wakeupFd_`、`Poller`） |
| ② | 造 `TcpServer` → 造 `Acceptor`：`socket` + `setReuseAddr/Port` + `bind`，并把 `acceptChannel_` 的读回调绑成 `Acceptor::handleRead` | 主线程 | `TcpServer::TcpServer` → `Acceptor::Acceptor` |
| ③ | `server.start()`：线程池起 N 个 subLoop，然后 `runInLoop(Acceptor::listen)` → `listen` + `enableReading` | 主线程（+ 派生 N 个 IO 线程） | `TcpServer::start` → `EventLoopThreadPool::start` → `Acceptor::listen` |
| ④ | 主线程 `loop.loop()` 进死循环，`epoll_wait` 阻塞等待 | 主线程 | `EventLoop::loop` → `EPollPoller::poll` |
| ⑤ | 客户端 connect → listenfd 可读 → `accept4` 得到非阻塞 connfd | 主线程 | `Channel::handleEvent` → `Acceptor::handleRead` → `Socket::accept` |
| ⑥ | `TcpServer::newConnection`：轮询选一个 subLoop，`new TcpConnection`，注册 4 个事件回调 + 关闭回调，然后 `ioLoop->runInLoop(connectEstablished)` | 主线程 → **线程跳转 1** → subLoop#k | `TcpServer::newConnection`、`EventLoopThreadPool::getNextLoop`、`EventLoop::queueInLoop` + `wakeup` |
| ⑦ | subLoop 被 eventfd 唤醒，执行 pending 回调：`connectEstablished` 里 `tie` + `enableReading`，并回调用户 `onConnection`（`connected()==true`） | subLoop#k | `EventLoop::doPendingFunctors` → `TcpConnection::connectEstablished` |
| ⑧ | 客户端发数据 → `readv` 进 `inputBuffer_` → 回调用户 `onMessage` | subLoop#k | `TcpConnection::handleRead` → `Buffer::readFd` |
| ⑨ | 用户 `conn->send()`：同线程直接 `sendInLoop`；异线程打包成 functor 回 subLoop。先尝试一次 `write`，没写完的进 `outputBuffer_` 并 `enableWriting` | subLoop#k（通常） | `TcpConnection::send` → `sendInLoop` |
| ⑩ | 内核发送缓冲区腾出空间 → EPOLLOUT → `handleWrite` 继续把 `outputBuffer_` 写完 → `disableWriting` → `writeCompleteCallback_`；若处于 `kDisconnecting` 补 `shutdownInLoop` | subLoop#k | `TcpConnection::handleWrite`、`Buffer::writeFd` |
| ⑪ | 客户端关闭 → 读到 0 或 EPOLLHUP → `handleClose`：`disableAll` + 用户 `onConnection`（`connected()==false`）+ `closeCallback_` → `TcpServer::removeConnection`（**线程跳转 2** 回 mainLoop）→ `connections_.erase` → `queueInLoop(connectDestroyed)`（**线程跳转 3** 到 subLoop）→ `TcpConnection` 引用计数归零析构 → `Socket::~Socket` 才 `close(connfd)` | subLoop#k → mainLoop → subLoop#k | `TcpConnection::handleClose`、`TcpServer::removeConnection`、`removeConnectionInLoop`、`TcpConnection::connectDestroyed` |
| ⑫ | 退出：`quit()` → `wakeup()` → `loop()` 的 while 结束 → 子线程函数返回 → `~EventLoopThread` 里 `join` | 各线程 | `EventLoop::quit`、`EventLoopThread::~EventLoopThread` |

**三次线程跳转是本项目最难也最出彩的地方**，面试时能讲清 ⑥ 和 ⑪ 就赢了：
- 跳转 1：mainLoop 把新连接的初始化工作丢给 subLoop（`runInLoop` + `wakeup`）
- 跳转 2：subLoop 发现连接断了，把「从 map 里删掉」交回给持有 map 的 mainLoop
- 跳转 3：mainLoop 删掉引用后，再把「销毁连接」交回给连接所属的 subLoop 去做

为什么删连接一定要回 mainLoop、销毁一定要回 subLoop？因为 `connections_` 只归 mainLoop 管，而 `Channel` 的注册表只归该 Channel 所属的 loop 管。**所有非原子状态都遵循「谁创建谁独占」**，这就是 muduo 几乎不需要加锁的原因（全库只有 `pendingFunctors_` 一把锁 + `EventLoopThread` 里握手用的一把）。

---

## 5. 36 个文件清单（按阶段排，含难度与核心函数）

| 文件 | 阶段 | 难度 | 核心函数 / 成员 | 一句话职责 |
|---|---|---|---|---|
| `include/noncopyable.h` | 1 | ★ | `= delete` 构造 / 赋值 | 继承它就禁止拷贝 |
| `include/Timestamp.h` + `src/Timestamp.cc` | 1 | ★ | `now` `toString` | 时间点值类型，日志与 receiveTime 用 |
| `include/Logger.h` + `src/Logger.cc` | 1 | ★★ | `instance` `log` + 四个宏 | 单例同步日志，宏是对外接口 |
| `include/CurrentThread.h` + `src/CurrentThread.cc` | 1 | ★★ | `tid` `cacheTid` `t_cachedTid` | 用 TLS 缓存线程 id，供 `isInLoopThread` 判断 |
| `include/InetAddress.h` + `src/InetAddress.cc` | 2 | ★★ | `toIp` `toIpPort` `getSockAddr` | `sockaddr_in` 的包装与地址转换 |
| `include/Socket.h` + `src/Socket.cc` | 2 | ★★★ | `accept` `bindAddress` `listen` `set*` `~Socket` | fd 的 RAII 外壳（**fd 的唯一拥有者**） |
| `include/Buffer.h` + `src/Buffer.cc` | 2 | ★★★★ | `readFd` `append` `retrieve` `makeSpace` | 双下标字节缓冲区，`readv` + 栈上 64KB 经典设计 |
| `include/Channel.h` + `src/Channel.cc` | 3 | ★★★★ | `handleEvent` `enable*` `tie` `update` `remove` | fd + 事件位 + 回调的绑定体，分发单位 |
| `include/Poller.h` + `src/Poller.cc` | 3 | ★★★ | `poll` `updateChannel` `removeChannel` `newDefaultPoller` | IO 复用的抽象接口 + `fd→Channel` 注册表 |
| `include/EPollPoller.h` + `src/EPollPoller.cc` | 3 | ★★★★ | `poll` `updateChannel` `update` `fillActiveChannels` | epoll 实现，`kNew/kAdded/kDeleted` 状态机在此 |
| `src/DefaultPoller.cc` | 3 | ★ | `newDefaultPoller` | 按环境变量选 IO 复用实现的工厂 |
| `include/EventLoop.h` + `src/EventLoop.cc` | 3 | ★★★★★ | `loop` `quit` `runInLoop` `queueInLoop` `wakeup` `doPendingFunctors` | 一个线程的心脏；本库最难的 210 行 |
| `include/Thread.h` + `src/Thread.cc` | 4 | ★★★ | `start` `join` `sem_*` `numCreated_` | `std::thread` 的加固壳（拿 tid、防 terminate） |
| `include/EventLoopThread.h` + `src/EventLoopThread.cc` | 4 | ★★★★ | `startLoop` `threadFunc` | 一个线程 + 一个栈上 EventLoop 的绑定 |
| `include/EventLoopThreadPool.h` + `src/EventLoopThreadPool.cc` | 4 | ★★★ | `start` `getNextLoop` `getAllLoops` | subLoop 集合与轮询分派算法 |
| `include/Callbacks.h` | 5 | ★★ | 6 个 `using` 别名 | 用函数对象类型打断类之间的循环依赖 |
| `include/TcpConnection.h` + `src/TcpConnection.cc` | 5 | ★★★★★ | `send` `sendInLoop` `handleRead` `handleWrite` `handleClose` `shutdown` `connectEstablished` `connectDestroyed` | 一条连接的一生：状态机 + 两个 Buffer + 高水位 |
| `include/Acceptor.h` + `src/Acceptor.cc` | 5 | ★★★ | `listen` `handleRead` `setNewConnectionCallback` | 把 listenfd 的可读事件变成 connfd + 回调 |
| `include/TcpServer.h` + `src/TcpServer.cc` | 5 | ★★★★ | `start` `newConnection` `removeConnection` `removeConnectionInLoop` | 对外门面 + 连接拥有者 + 跨线程关闭调度者 |
| `example/testserver.cc` | 0 和 6 | ★★ | `EchoServer::onMessage` `main` | 用户视角：用这个库写服务器只需 4 步 |
| `CMakeLists.txt` 共 3 份 | 0 | ★★ | `add_library` `add_subdirectory` | 怎么把这些编译成 `.so` 与可执行文件 |

---

## 6. 进度打卡表

读完一个勾一个。只勾「读了」不算完，要能回答文件头 `【自检】` 的问题才算。

- [ ] 阶段 0：`example/testserver.cc`、`include/TcpServer.h` 接口
- [ ] 阶段 1：`noncopyable.h` / `Timestamp` / `Logger` / `CurrentThread`
- [ ] 阶段 2：`InetAddress` / `Socket` / `Buffer`（**Buffer 要能默画内存图**）
- [ ] 阶段 3：`Channel` / `Poller` / `EPollPoller` / `DefaultPoller` / `EventLoop`（**EventLoop 要能默画 loop 三步**）
- [ ] 阶段 4：`Thread` / `EventLoopThread` / `EventLoopThreadPool`
- [ ] 阶段 5：`Callbacks` / `TcpConnection` / `Acceptor` / `TcpServer`（**TcpConnection 要能默画四态状态机**）
- [ ] 阶段 6：`docs/architecture.md` 对表 + gdb 亲眼看到 3 次线程跳转
- [ ] 阶段 7：完成 `docs/experiments.md` 至少 1 个练习 + 20 道面试题自测

---

## 7. 按你的可用时间选最短路线

| 可用时间 | 怎么读 | 结果 |
|---|---|---|
| **只剩 2 天（面试突击）** | 阶段 0（30min）→ 只读 `include/*.h` 的类注释与成员注释（2h）→ `EventLoop.cc` 的 `loop` `queueInLoop` `doPendingFunctors` + `Channel.cc` 的 `tie` `handleEventWithGuard`（3h）→ `TcpConnection.cc` 的 `sendInLoop` `handleWrite` `handleClose`（2h）→ `docs/experiments.md` 第 6 节 20 题 + 本指南第 8 节差异表（3h） | 能讲清 Reactor、one loop per thread、关闭链，撑住大部分项目追问 |
| **1 周（读懂 + 能讲）** | 走完阶段 0~6，每天 3 小时，阶段 3 与阶段 5 各占一整天 | 能独立画架构图并说清每一步在哪个线程 |
| **2 周（能改造，可写简历）** | 阶段 0~7 全走 + 完成练习 3（TimerQueue）、练习 4（修 sendfile 忙轮询）、练习 5（异步日志）中的两个 | 有「我基于开源库做了 X、改了 Y」的素材，而不是「我读了别人的代码」 |

---

## 8. 本项目 vs 陈硕原版 muduo（面试必问，别把退化当设计）

| 维度 | 本项目（教学精简版） | 原版 muduo | 面试怎么说 |
|---|---|---|---|
| 时间戳精度 | `Timestamp::now()` 用 `time(NULL)`，精度只到**秒**，字段名却叫 microSeconds | `gettimeofday` 微秒 + `toString` 带小数 | 主动说你知道单位不一致，并说得出修法（练习 1） |
| IO 复用实现 | 只有 `EPollPoller`；`DefaultPoller` 的 poll 分支 `return nullptr` | `EPollPoller` 与 `PollPoller` 都有，按平台/环境变量选 | 讲清 LT + 非阻塞的选型理由，比背 API 更值钱 |
| 定时器 | **没有 TimerQueue**，没有 `runAfter` / `every` | `timerfd_create` + `TimerQueue`，`EventLoop` 提供定时接口 | 高频问题「muduo 定时器怎么实现」必须自己补（练习 3） |
| 日志 | 同步 `std::cout`，单例，级别只影响前缀，`LOG_FATAL` 直接 `exit(-1)` | 异步双缓冲：前端缓冲 + 后端线程 + 定时落盘 + 掉信号回栈 | 先说同步日志的三个问题（阻塞 IO 线程、可能丢日志、无缓冲策略），再讲原版方案（练习 5） |
| Buffer | `vector<char>` + 双下标，**没有 prepend 接口** | 有 `prependInt64` / `prependAsString` + 哨兵字节防误用 | 被问 `kCheapPrepend` 为什么留 8 字节 → 答「协议头/长度字段要往前塞」，并承认本项目没实现 |
| Socket 封装 | 一个 `Socket` 类，`setsockopt` 返回值全忽略，`accept` 不处理 `EINTR` / `EAGAIN` | `Socket` 类 + `sockets.h` 自由函数集，错误处理完整 | 强调**非阻塞 accept 与 LT 的配合**，顺带说还缺哪些错误处理 |
| 连接关闭 | `TcpServer` 无 `stop()`；`Acceptor` 析构不 `shutdown` | 有 `stop()`、`SO_LINGER`、完整 `shutdown` 优雅序列 | 讲 ⑪ 的三跳关闭链已足够体现深度，同时承认缺 shutdown 序列 |
| 防御式检查 | 没有 `assertLoopInThisThread`；`hasChannel` 有声明但定义被注释掉 | 到处 `assert(isInLoopThread())`，误用当场崩 | 「本项目靠约定，原版靠断言」是很强的工程视角回答 |
| 连接分派 | 轮询（`next_` 取模）；曾有一致性哈希后被移除 | 也是轮询 | 被问「为什么不用一致性哈希」→ 连接与 subLoop 一一对应即可，一致性哈希适合有状态后端 |
| 协议层 | 无 HTTP、无粘包处理 | 库本身也不管（原版 examples 里有 HTTP 示例） | **这是正确的设计**：传输层不做协议，别当缺陷说 |

---

## 9. 读不懂时怎么办

### 三级降级策略
1. **只看签名**：折叠函数体，只读 `include/*.h` 的类注释 + 声明 + 成员注释，先建立谁有什么的地图。
2. **只看标签**：`grep -rn 【流程】 src` 与 `grep -rn 【线程】 src`，把 ①→⑫ 骨架串起来，细节全部忽略。
3. **让程序告诉你答案**：照 `docs/experiments.md` 第 3 节上 gdb 断点，用 `info threads` + `bt` 拿运行时事实倒推代码。卡住超过 20 分钟就直接上断点，别硬看。

### 初学者最常卡的 12 个点（注释里都已展开，按文件名去查）
1. `Channel` 为什么只前向声明 `EventLoop` → `include/Channel.h` 的 `【设计】`
2. `events_` / `revents_` / `index_` 三个 int 分别由谁读写 → `include/Channel.h` 成员注释
3. `epoll_wait` 返回的 `data.ptr` 从哪来 → `src/EPollPoller.cc` 的 `update`
4. `enableReading()` 一行为什么就能改 epoll 注册 → `src/Channel.cc` 的 `update`（三层转发链）
5. `wakeupFd_`（eventfd）为什么必须存在 → `src/EventLoop.cc` 构造函数与 `wakeup`
6. `doPendingFunctors` 为什么要 `swap` → `src/EventLoop.cc` 同名函数
7. 新连接为什么不在 mainLoop 直接处理 → `src/TcpServer.cc` 的 `newConnection`
8. `runInLoop` 与 `queueInLoop` 的区别 → `src/EventLoop.cc` 相邻两个函数
9. `TcpConnection` 为什么必须 `enable_shared_from_this` → `include/TcpConnection.h` 类注释
10. `Channel::tie` 防的是什么事故 → `src/Channel.cc` 的 `tie` 与 `handleEvent`
11. 谁真正 `close(connfd)` → `src/Socket.cc` 的 `~Socket` + `TcpConnection` 析构链
12. `main` 里为什么最后必须 `loop.loop()` → `example/testserver.cc` 的 `main`

---

## 10. 术语速查

| 术语 | 在本项目里指 | 代码对应 |
|---|---|---|
| Reactor | 由事件就绪驱动、用户自己收发数据的并发模型 | `EventLoop` + `Poller` + `Channel` |
| Demultiplexer | 从大量 fd 中挑出就绪的那些 | `EPollPoller::poll` |
| one loop per thread | 一个线程最多一个 EventLoop | `__thread t_loopInThisThread` |
| mainLoop / baseLoop | 用户 `main` 里的 loop，负责 accept 与连接表 | `TcpServer::loop_` |
| subLoop / ioLoop | 线程池各线程的 loop，负责 IO 事件 | `EventLoopThreadPool::loops_` |
| Channel | 一个 fd + 感兴趣的事件 + 事件回调 | `class Channel` |
| tie | Channel 弱引用住 TcpConnection，防对象先亡 | `Channel::tie` `tie_` `tied_` |
| 高水位 | 待发数据越过阈值时通知用户减速 | `highWaterMark_`（64MB） |
| readable / writable / prependable | 已读未清理之后的可读区 / 尾部可写区 / 头部预留区 | `Buffer` 的三个 `*Bytes()` |
| kCheapPrepend | 头部预留 8 字节，省一次搬移 | `Buffer::kCheapPrepend` |
| LT / ET | 水平触发（没读完还会再报）/ 边缘触发（状态变化才报一次） | 本项目全用 LT + 非阻塞 |
| 半关闭 | 只关写端，仍可继续读 | `Socket::shutdownWrite` |
| EPOLLHUP | 对端已断，只剩读不出 | `Channel::handleEventWithGuard` 第一个分支 |
| RAII | 资源生命周期绑定对象生命周期 | `Socket::~Socket`、`unique_ptr` 成员 |
| TLS | 线程局部存储，每线程一份 | `__thread` |

---

## 附：这套注释的产生方式与回退方法

注释由 6 组并行工作写入，全部满足「**只加注释、代码零改动**」的机器校验（剥离注释后的 token 流与 `git HEAD` 逐行一致，校验脚本为临时工具，不入仓库）。因此 `git diff` 里全是纯新增行；若你不喜欢某个文件的注释风格，可以单独回退它而不影响其他文件：`git checkout -- src/XXX.cc`。
