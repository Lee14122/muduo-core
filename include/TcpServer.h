/* ============================================================================
 * 【文件】include/TcpServer.h
 * 【一句话】声明对外的“服务器门面”：把 Acceptor、线程池、连接表、用户回调组装在一起，
 *          让用户只写 “构造 + 设回调 + setThreadNum + start” 四步就能跑起来一个服务器。
 * 【Reactor 角色】门面（Facade）+ 组装者：本身不参与 epoll，负责把 MainReactor 与
 *          若干 SubReactor（one loop per thread）连成一张对象图。
 * 【依赖】EventLoop.h、Acceptor.h、InetAddress.h、EventLoopThreadPool.h、Callbacks.h、
 *        TcpConnection.h、Buffer.h、noncopyable.h —— 几乎把全库都 include 了（见下面两段说明）。
 * 【谁用它】用户代码（example/testserver.cc 里的 EchoServer 持有它一个值成员）；
 *          用户之外没有谁调它，它是调用链的最顶端。
 * 【先修知识】① Acceptor / EventLoopThreadPool / TcpConnection 三个类各自干什么；
 *        ② mainLoop 与 subLoop 的分工；③ std::function + std::bind 做成员函数回调；
 *        ④ unordered_map 存对象所有权（value 是 shared_ptr）。
 * 【难度】★★★☆☆   【建议耗时】25 分钟（配合 src/TcpServer.cc 读，那个文件才是重点）
 * 【自检】1) 为什么这个头文件要 include 全库，而 Callbacks.h 一个项目头都不 include？
 *        2) connections_ 为什么以 string 连接名为 key，而不是以 fd 为 key？谁能读写它？
 *        3) 哪个成员是“写了没人读”的死成员？哪个选项传进去其实没生效？
 * ============================================================================ */
#pragma once

// 【设计】原注释“用户使用muduo编写服务器程序”就是本类的定位：它是**用户视角的库入口**。
//   读源码时可以用它当“目录”：想理解 accept → 分发 → IO 的整条链，从这里往下钻。
/**
 * 用户使用muduo编写服务器程序
 **/

// 【语法】这一组标准库 include 是给下面成员用的：<functional>（ThreadInitCallback）、
//   <string>（ipPort_/name_）、<memory>（两个智能指针）、<atomic>（started_）、
//   <unordered_map>（connections_）。
//   【设计】注意风格差异：Callbacks.h 追求“零项目依赖 + 只前向声明”，TcpServer.h 追求
//   “用户只用 include 这一个头”。两种取向都对，因为它们在依赖图里的位置不同：
//   Callbacks.h 在最底层（被很多人 include，越少越好），TcpServer.h 在最顶层
//   （只被用户 include，越多越方便）。这是“头文件分层”的核心判断：**看谁 include 我**。
#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>

// 【设计】七个项目头，覆盖“组装一台服务器需要的全部零件”。读依赖方向要读出三条规矩：
//   ① TcpServer → Acceptor / EventLoopThreadPool / TcpConnection：门面对下依赖，单向；
//   ② TcpConnection.h 里**没有** include TcpServer.h（它只认识自己的回调别名），
//      所以“TcpServer 传给 TcpConnection 的 closeCallback_ 里绑 TcpServer::removeConnection”
//      这条反向连线是靠 std::function 的类型擦除实现的 —— 依赖图依旧无环（这就是
//      Callbacks.h 那套技巧在这里的回报）；
//   ③ Buffer.h / InetAddress.h 是为了让用户在 onMessage 里能直接用 buf->retrieveAllAsString()
//      而不用自己 include，属于“替用户着想”的 include。
//   【易错】代价是全库重编：改 EventLoop.h 会重编所有 include TcpServer.h 的用户文件。
//      原版 muduo 的做法一致（TcpServer.h 也是大杂烩），只在 2.0 之后拆出更细的头。
//   【对比原版】原版 muduo 的 TcpServer.h 用 boost 智能指针 + 前向声明 TcpConnection，
//   并且 TcpServer 构造里就 acceptor_.reset(new Acceptor(...))、
//   threadPool_(new EventLoopThreadPool(...))，本项目结构相同。
#include "EventLoop.h"
#include "Acceptor.h"
#include "InetAddress.h"
#include "noncopyable.h"
#include "EventLoopThreadPool.h"
#include "Callbacks.h"
#include "TcpConnection.h"
#include "Buffer.h"

// 【设计】本类的“线程归属表”（面试常问“muduo 各对象在哪个线程”，答这张表）：
//   ┌────────────────┬───────────────────┬──────────────────────────────────┐
//   │ 成员            │ 归哪个线程         │ 说明                              │
//   ├────────────────┼───────────────────┼──────────────────────────────────┤
//   │ loop_          │ mainLoop（主线程） │ 用户 main 里那个 EventLoop        │
//   │ acceptor_      │ mainLoop          │ 只监听 listenfd 的 EPOLLIN        │
//   │ threadPool_    │ 创建在 mainLoop    │ 内部每线程一个 EventLoop（subLoop）│
//   │ connections_   │ 只在 mainLoop 读写 │ 无锁的资本（见下面成员说明）        │
//   │ 三个用户回调    │ 只读快照            │ newConnection 时拷给每条连接        │
//   │ started_       │ 任意线程（atomic） │ 防重复 start                       │
//   │ nextConnId_    │ 只在 mainLoop      │ 所以不需要 atomic                  │
//   └────────────────┴───────────────────┴──────────────────────────────────┘
//   【设计】本类自己的成员函数没有一个是“会被 epoll 事件直接调起”的 —— 除了 newConnection
//   （由 Acceptor 的回调同步调起，仍在 mainLoop）。这也是它能保持无锁的根本原因。
// 对外的服务器编程使用的类
class TcpServer
{
public:
// 【设计】ThreadInitCallback 定义在类内而不是放进 Callbacks.h：它的参数是 EventLoop*，
//   放进 Callbacks.h 就会逼着那个契约层去前向声明 EventLoop，破坏“契约层零项目依赖”。
//   EventLoopThreadPool::start 里用的是**同名同型**的别名（各自类内定义），
//   两者是同一个 std::function<void(EventLoop*)> 类型，可以直接互相赋值（结构类型等价）。
//   【流程】用途：给每个 subLoop 线程做初始化（设线程名、绑 CPU、初始化线程局部资源）。
//   【线程】cb 在**每个 subLoop 线程**里被调用一次（在 EventLoop 构造好之后、进入 loop 之前），
//   单线程模式（setThreadNum(0)）则在主线程对 baseLoop 调一次。
    using ThreadInitCallback = std::function<void(EventLoop *)>;

// 【设计】Option 是“枚举开关”，故意不用 bool 参数：调用点写 TcpServer(..., TcpServer::kReusePort)
//   比写 TcpServer(..., true) 可读得多（避免“bool 参数黑洞”）。
//   【⚠问题】这个选项在本项目里**目前是空的**：Acceptor 构造函数无条件 setReusePort(true)，
//   完全没读 reuseport 形参（见 include/Acceptor.h 与 src/Acceptor.cc 里那条【⚠问题】），
//   所以传 kNoReusePort 也会打开 SO_REUSEPORT。想看出差别，只能改 Acceptor。
//   SO_REUSEPORT 的真实用途是“多个进程/线程各持一个 listenfd 绑同一端口，内核按连接
//   做负载均衡”（原版 muduo 的 TCP 多线程 accept 优化就是它），本项目只有一个 listenfd，
//   开了它只带来一个副作用：别的进程也能同时绑同一端口，端口冲突从报错变成静默。
    enum Option
    {
        kNoReusePort,//不允许重用本地端口
        kReusePort,//允许重用本地端口
    };

// 【语法】Option option = kNoReusePort 是**默认实参**，规则有两条：
//   ① 只能写在声明里，定义（src/TcpServer.cc 的 TcpServer::TcpServer）里不能再写一遍，
//      否则“重定义默认实参”编译错误 —— 去看那个 .cc 的定义，参数表里确实没写默认值；
//   ② 默认实参之后的参数也必须都有默认值（这里 option 是最后一个，正好）。
//   【设计】四个参数的顺序体现“从最重要到可选”：loop（没有它什么都不干）、
//   listenAddr（必须）、nameArg（只影响日志与连接名）、option（可有可无）。
//   【易错】构造即 bind：端口被占用 / 绑 1024 以下端口没有 root 权限 → 构造函数里
//   就 LOG_FATAL → exit(-1)，**不会**返回错误码。所以“构造服务器失败”在这套库里
//   是进程级事件，不是异常。（原版 muduo 用 abort + 日志，思路相同：绑不上就别跑了。）
    TcpServer(EventLoop *loop,
              const InetAddress &listenAddr,
              const std::string &nameArg,
              Option option = kNoReusePort);
// 【流程】析构是 ⑫/⑪ 之外的另一条回收路径：进程退出时把还活着的连接逐个送进
//   各自 loop 线程做 connectDestroyed（实现见 src/TcpServer.cc 里那段 item.second.reset() 技巧）。
//   【线程】调用者线程（通常是主线程，且就是 mainLoop 线程）；投递的动作在 subLoop。
//   【易错】析构之后 subLoop 线程也才被 join（靠 threadPool_ 这个成员随后析构 →
//   EventLoopThreadPool 析构 → EventLoopThread 析构 → quit + join）。
//   顺序完全由**成员声明顺序**决定：connections_ 必须在 threads_ 之前被清空/投递，
//   而 threadPool_ 声明在 connections_ 之前 → 销毁顺序里 threadPool_ 比 connections_ **晚**死，
//   所以 connections_ 里的连接对象能先被安全销毁。改这个顺序会直接崩在退出路径上。
    ~TcpServer();

// 【流程】唯一的“必须早于 start”的 setter：src/TcpServer.cc 的 start() 里把它传给
// 【线程】mainLoop（用户在构造 EchoServer/start 之前调用）。
//   threadPool_->start(cb)，start 之后再改就只对（不存在的）未来线程池生效。
//   【易错】它没有同步；在别的线程 setThreadInitCallback 同时主线程 start 是数据竞争。
    void setThreadInitCallback(const ThreadInitCallback &cb) { threadInitCallback_ = cb; }
// 【设计】三个 setter 就是“回调三级传递”的第一站（用户 → TcpServer → TcpConnection）。
// 【线程】mainLoop（构造期/main 里），必须在 start() 之前；之后 subLoop 已经在读这些成员的副本，
//   再改本类成员既没有锁保护、也影响不到已建立的连接。
//   【易错】对外的 setter 只在这四个里，**故意没有 setCloseCallback**：
//   CloseCallback 是库内部回收钩子，被 TcpServer::newConnection 独占，用户碰不到 ——
//   这防住了“用户覆盖回收回调导致连接永不释放”的事故，是这套 API 设计里很讲究的一点。
//   【易错】也**没有 setHighWaterMarkCallback**：高水位只能在 onConnection 里对单条连接调
//   conn->setHighWaterMarkCallback(cb, 阈值)，没有“对所有连接统一设”的入口（原版同构）。
//   【流程】这些 setter 必须在 start() 之前调；对**已建立**的连接改动无效 ——
//   因为 newConnection 是“快照式拷贝”给每个连接的。
    void setConnectionCallback(const ConnectionCallback &cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb) { writeCompleteCallback_ = cb; }

// 【流程】③ 的前置：决定 subLoop 个数（0 = 单 Reactor 单线程模式，
//   所有连接的 IO 都压在主线程；N = MainReactor 只 accept，N 个 SubReactor 做 IO）。
//   【线程】mainLoop（用户构造期调用）。
    // 设置底层subloop的个数
    void setThreadNum(int numThreads);
// 【易错】实现里有一个同名局部变量遮蔽成员 numThreads_（src/TcpServer.cc 的 setThreadNum），
//   结果成员永远保持未初始化值。功能上没错（真正的数字交给了 threadPool_），
//   但读代码时很容易以为“TcpServer 自己记了一份线程数”。规范点名的【易错】就是这条。
    /**
     * 如果没有监听, 就启动服务器(监听).
     * 多次调用没有副作用.
     * 线程安全.
     */
    void start();

// 【设计】三个私有函数正好是连接生命周期的三段，线程各不同（面试重点）：
//   newConnection          ⑥  mainLoop（Acceptor 回调同步调起）
//   removeConnection       ⑪  任意线程，实际是 subLoop（TcpConnection::handleClose 调起）
//                            —— 函数体只做一件事：把下个函数投回 mainLoop（【线程跳转 2】）
//   removeConnectionInLoop ⑪  mainLoop（erase + 把 connectDestroyed 投给 subLoop，【线程跳转 3】）
//   为什么这么切：数据归属线程。connections_ 只能在 mainLoop 动，
//   而 Channel/epoll 的摘除只能在 subLoop 动，所以中间必须有两次投递。
// 【语法】这三个都是成员函数，被 std::bind 绑进 std::function 时第一个实参补对象（this 或 shared_ptr）。
private:
    void newConnection(int sockfd, const InetAddress &peerAddr);
    void removeConnection(const TcpConnectionPtr &conn);
    void removeConnectionInLoop(const TcpConnectionPtr &conn);

// 【设计】key 为什么是 std::string 连接名而不是 int fd：
//   ① fd 会被内核复用：一条连接 close 后号码可以给下一条，用它做 key 有踩到旧条目的风险；
//   ② 名字里带 ip:port#序号，日志可读（removeConnectionInLoop 打日志时能直接对上）；
//   ③ 唯一性由 nextConnId_ 自增保证，与 fd 生命周期彻底解耦。
//   【语法】unordered_map<string, TcpConnectionPtr> 的 value 是 shared_ptr → 这张表是
//   所有连接的**主要所有者**（一个强引用）。value 若改成 TcpConnection* 就会当场失效：
//   TcpServer::newConnection 结束后没人持有对象，直接析构 + fd 关闭。
//   【易错】这张表**不是线程安全的**，只在 mainLoop 里被增删（newConnection 插、
//   removeConnectionInLoop 删）；不要在别的线程遍历/查它，也没有暴露任何公开接口去读它。
//   原版 muduo 用同一个设计（MuduoTcpServer 里是 map<string, TcpConnectionPtr> + 注释
//   “只在 IO 线程访问”）。
    using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

// 【设计】mainLoop 的裸指针（不拥有：EventLoop 活在用户 main 的栈上）。
//   【易错】它必须是**用户栈上那个**，绝不能传某个 subLoop 的指针 —— 那会让
//   accept 与连接表被塞进 IO 线程，破坏“MainReactor 只做分发”的分工。
//   src/TcpServer.cc 构造里先用 CheckLoopNotNull 挡掉 nullptr。
    EventLoop *loop_; // baseloop 用户自定义的loop

// 【设计】ipPort_ 是 listenAddr.toIpPort() 的**快照字符串**（形如 0.0.0.0:8080），
//   只用于拼连接名和打日志；const 成员保证“没人能改”，所以任何线程读它都安全。
//   【易错】它是 bind 时的地址快照，不是“当前实际地址”；本项目里没有热改端口的路径。
    const std::string ipPort_;
// 【设计】服务器名（如 EchoServer），参与连接名拼接（newConnection 里 connName = name_ + buf）。
//   另外它还是 EventLoopThreadPool 的名字来源（线程名/EventLoopThread 名依赖它）。
    const std::string name_;

// 【设计】为什么用 unique_ptr<Acceptor> 而不是值成员 Acceptor acceptor_：
//   ① Acceptor 是 noncopyable 且没有默认构造，值成员也能在初始化列表里带参构造，
//      但 unique_ptr 让“所有权 + 何时创建/销毁”在类型上写得更明白（门面拥有且只此一份）；
//   ② Acceptor 的构造参数依赖 option 判断（option == kReusePort），用 new 表达式写更直观；
//   ③ 原版 muduo 同样是 std::unique_ptr<Acceptor> acceptor_，并在 ~TcpServer 里显式
//      acceptor_.reset() 以固定“先停 accept、再拆连接”的销毁顺序 ——
//      本项目没有那句显式 reset，靠成员反序销毁（acceptor_ 声明在 connections_ 之前，
//      所以它**晚于** connections_ 析构，也就是说拆连接时 listenfd 还开着）。
//      【⚠问题】销毁顺序与原版不同：本项目在 ~TcpServer 函数体里先投 connectDestroyed，
//      随后才轮到 acceptor_ 成员析构，实际效果仍可接受（函数体先跑），
//      但如果以后有人在函数体里 return 之前提前销毁 acceptor_，要留意新连接与拆连接的竞态。
//   【线程】mainLoop（listenfd 的 Channel 注册在 mainLoop 的 Poller 上）。
    std::unique_ptr<Acceptor> acceptor_; // 运行在mainloop 任务就是监听新连接事件

// 【设计】one loop per thread 的载体：它内部是 N 个 EventLoopThread，每个跑一个 EventLoop。
//   【对比原版】原版 muduo 这里是 std::unique_ptr<EventLoopThreadPool>；本项目写 shared_ptr，
//   语义上等价（没有第二处持有者，所以共享计数其实只用到 1），
//   代价是构造/析构各多一对原子操作，收益是“将来想把线程池借给别人”不用改类型。
//   读代码时别把它当成“线程池可以被多个 server 共享”的证据 —— 它是每个 TcpServer 一份。
//   【线程】对象创建/销毁在 mainLoop，里面的线程由 start() 拉起。
//   【易错】它没有“优雅停止”接口：停止靠 unique_ptr 析构链（EventLoopThread 析构里
//   quit + join），见 EventLoopThreadPool/EventLoopThread 那两个文件的说明。
    std::shared_ptr<EventLoopThreadPool> threadPool_; // one loop per thread

// 【流程】用户回调在门面层的落点：newConnection 里逐个拷给 TcpConnection（三级传递第二站）。
//   【设计】三个 setter 里 connectionCallback_ 是唯一“库本身也关心”的（用户不注册时
//   原版 muduo 会挂一个默认打印回调，本项目没有默认值 → 空 function 被裸调会抛
//   std::bad_function_call，见 TcpConnection.h 里那处说明）。
    ConnectionCallback connectionCallback_;       //有新连接时的回调
    MessageCallback messageCallback_;             // 有读写事件发生时的回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完成后的回调

// 【线程】这个回调不在本类里调用，而是**交给 threadPool_->start(threadInitCallback_)**，
//   由各个 subLoop 线程在自己的线程里执行 —— 它是“把初始化代码搬到目标线程”的标准手法。
//   【易错】在里面不要做阻塞很久的活（线程名、CPU 亲和性、线程局部缓存才合适）；
//   也不要在这里存下 EventLoop* 之外的对象引用。
    ThreadInitCallback threadInitCallback_; // loop线程初始化的回调
// 【⚠问题】死成员：唯一的写入点是 setThreadNum 里的**局部同名变量**（遮蔽），
//   所以本成员从未被真正赋过值（是未初始化的 int），也从未被任何地方读。
//   真正的线程数真相在 EventLoopThreadPool::numThreads_ 里。
//   【易错】别把它当“当前有几条连接/几个 loop”的依据；要看就去 threadPool_。
//   【语法】“有名字遮蔽但没有编译器报错”的原因是 -Wshadow 默认不开（-Wall 不含它）。
    int numThreads_;//线程池中线程的数量。
// 【语法】用 std::atomic_int 当布尔标志（0/1）。原版 muduo 是 std::atomic<bool> started_，
//   本项目用 fetch_add 判首次，所以写成 atomic_int 更贴合实现。
//   【流程】③ 的守门员；start() 里 started_.fetch_add(1)==0 那条说明它如何防重复。
//   【易错】atomic 只保证“计数不丢”，不保证 start 的**完成**：第二次 start() 返回时，
//   第一次的 listen 可能还在 pendingFunctors_ 里没跑（原版 muduo 也有这个语义，
//   所以 start() 不能理解为“返回即已可接客”）。
    std::atomic_int started_;
// 【设计】连接名里的自增号。它**不是** atomic，因为 ++ 只发生在 mainLoop
//   （newConnection 里，见 src/TcpServer.cc 里那条同样内容的说明）——
//   这是“数据归属线程”换取无锁的典型例子：靠线程约定而不是靠 atomic 或锁。
    int nextConnId_;
// 【设计】这张表是“服务器到底还活着多少连接”的唯一真相（不含正在回收中的连接）。
//   它同时提供了三件东西：连接对象的所有权、名字唯一性检查、以及 ~TcpServer 的遍历入口。
//   【易错】它不排序、不能按地址查；要统计在线连接数只能在本类内部加接口 ——
//   门面故意没暴露 connections_，用户看不到（想在线人数得自己在 onConnection 里计数）。
    ConnectionMap connections_; // 保存所有的连接
};