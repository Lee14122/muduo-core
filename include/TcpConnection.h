/* ============================================================================
 * 【文件】include/TcpConnection.h
 * 【一句话】声明“一条已建立的 TCP 连接”这个对象：它持有 connfd、对应的 Channel、
 *          收发两个 Buffer、四个事件回调和五六个用户回调，并规定“只能被 shared_ptr 持有”。
 * 【Reactor 角色】Handler（Reactor 模型里“处理一个连接的业务读写”那一层，one loop per connection）
 * 【依赖】Callbacks.h（六个回调别名）；Buffer.h（两个缓冲区按值内嵌，必须完整类型）；
 *        InetAddress.h（本端/对端地址）；Timestamp.h（收包时间）；noncopyable.h（禁止拷贝）；
 *        <atomic>（state_）；<memory>（enable_shared_from_this、两个 unique_ptr 成员）。
 *        对 Channel / EventLoop / Socket 只前向声明 —— 它们只以“指针/智能指针成员”出现。
 * 【谁用它】TcpServer::newConnection 里 new 出来并存进 connections_；用户在
 *        ConnectionCallback / MessageCallback 里通过 TcpConnectionPtr 调 send / shutdown；
 *        Channel 通过构造时绑好的四个成员函数回调把 epoll 事件转进来。
 * 【先修知识】① std::shared_ptr 的控制块与 enable_shared_from_this；② 状态机；
 *        ③ unique_ptr 删除器为什么需要完整类型；④ 类内成员声明顺序 == 初始化顺序。
 * 【难度】★★★★☆   【建议耗时】35 分钟（配合 src/TcpConnection.cc 一起读）
 * 【自检】1) 为什么必须 public 继承 enable_shared_from_this，直接
 *           shared_ptr<TcpConnection>(this) 会发生什么？构造函数里能不能用 shared_from_this()？
 *        2) 四个状态 kDisconnected/kConnecting/kConnected/kDisconnecting 分别由哪个函数迁入？
 *        3) 为什么 Socket 和 Channel 只前向声明就能当 unique_ptr 成员，而 Buffer 不行？
 * ============================================================================ */
#pragma once

// 【语法】<memory>：enable_shared_from_this、unique_ptr、shared_ptr 都要它；
//   <string>：name_ 是 std::string（连接名，TcpServer 里生成，同时当 connections_ 的 key）；
//   <atomic>：state_ 是 std::atomic_int —— 见下面成员处【线程】那条说明。
#include <memory>
#include <string>
#include <atomic>

// 【设计】include 的选择暴露了本类的内存布局策略：
//   Buffer / InetAddress / Timestamp 是**按值内嵌**的成员，编译器要算 TcpConnection 的大小，
//   所以必须 include 完整定义；Channel / EventLoop / Socket 只以指针形态出现，
//   于是只前向声明（见下面 class Channel; 那一段）。
//   少 include 一个头，编译依赖就少一条边，改 Socket.h 不会重编 TcpConnection.h 的所有使用者。
#include "noncopyable.h"
#include "InetAddress.h"
#include "Callbacks.h"
#include "Buffer.h"
#include "Timestamp.h"

// 【语法】三个前向声明 + 上面那句：为什么 Channel / EventLoop / Socket 可以不 include？
//   EventLoop* loop_; —— 裸指针，只需要名字。
//   std::unique_ptr<Socket> socket_; std::unique_ptr<Channel> channel_; ——
//   声明一个 unique_ptr<T> 成员时只实例化了“存 T* 指针”这部分，T 不完整也可以；
//   但 ~unique_ptr 会实例化删除器（要 sizeof/要调 ~T()），那一刻必须看到 T 的完整定义。
// 【设计】本项目正好躲开这个坑：TcpConnection 的析构函数**只在头里声明、定义写在
//   src/TcpConnection.cc**，而那个 .cc include 了 Socket.h 和 Channel.h。
//   【易错】如果把 ~TcpConnection 写成头里的空实现（inline），Socket 和 Channel 就必须在
//   TcpConnection.h 里 include 完整定义，否则 GCC 报
//   “invalid application of ‘sizeof’ to incomplete type ‘Socket’”。
//   这条规则叫 “unique_ptr + incomplete type ⇒ 析构函数外置到能看到完整定义的 .cc”。
class Channel;
class EventLoop;
class Socket;

// 【设计】这段原注释画的是本类的调用链，把它读成“事件怎么流进业务代码”：
//   Acceptor（mainLoop，listenfd）accept 出 connfd → TcpServer::newConnection 建本对象
//   → 本对象构造时把 handleRead/handleWrite/handleClose/handleError 绑到 Channel 的四个
//   std::function 上 → Channel 注册进 Poller(epoll) → subLoop 的 epoll_wait 返回后
//   Channel::handleEvent 调起对应函数。也就是“Poller 只认识 Channel，Channel 只认识
//   std::function，业务语义在 TcpConnection”。分层的好处：Channel 里没有任何 TCP 词汇。
/**
 * TcpServer => Acceptor => 有一个新用户连接，通过accept函数拿到connfd
 * => TcpConnection设置回调 => 设置到Channel => Poller => Channel回调
 **/

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ enable_shared_from_this：本类为什么“必须”public 继承它                       │
// └────────────────────────────────────────────────────────────────────────────┘
// 【语法】本类要在回调里把自己（一个 TcpConnectionPtr）交给用户：
//   messageCallback_(shared_from_this(), &inputBuffer_, receiveTime)、
//   channel_->tie(shared_from_this())、loop_->queueInLoop(bind(writeCompleteCallback_, shared_from_this()))。
//   没有 shared_from_this 的“正规”替代写法是 shared_ptr<TcpConnection>(this)。
// 【易错】shared_ptr<TcpConnection>(this) 会**新建一个独立的控制块**：同一个对象上有两份
//   引用计数，各自都能归零 → 同一段内存被 delete 两次（double free，堆损坏、随机崩溃），
//   而且其中一份归零后另一份还在用，就是 use-after-free。shared_ptr 的线程安全只保证
//   控制块内的计数原子，绝不阻止你为同一个裸指针建两个控制块。
// 【语法】enable_shared_from_this 的机制（libc++/libstdc++ 实现）：它在对象里多藏一个
//   weak_ptr（_M_weak_this）。shared_ptr 的构造函数在接管裸指针时（__enable_shared_from_this）
//   会用 if constexpr 探测基类，把这个 weak_ptr 指向新控制块。于是 shared_from_this()
//   = 那枚 weak_ptr 的 lock()，拿到的是**共享原控制块**的第二个 shared_ptr，计数 2，安全。
// 【设计】所以两条规矩必须同时成立：
//   ① 必须 public 继承（private/protected 继承时 shared_ptr 无法访问那个 weak_ptr，
//      要么编译失败要么 shared_from_this() 抛 bad_weak_ptr）；
//   ② 对象必须**先被一个 shared_ptr 接管**，之后才能调 shared_from_this()。
//      在构造函数里调 shared_from_this() 一定抛 std::bad_weak_ptr —— 因为构造还没结束，
//      TcpServer::newConnection 里那句“connections_[connName] = conn”还没执行，
//      控制块还不存在。这就是本项目把 connectEstablished 单独做成一个函数、
//      并且必须由 ioLoop->runInLoop 在对象进表之后才调用的根本原因。
// 【易错】也不能把 TcpConnection 放到栈上/当别的类的值成员：那样永远没有控制块，
//   shared_from_this() 直接抛。它只能 new + shared_ptr。
// 【对比原版】原版 muduo 用 boost 时代的 enable_shared_from_this，并有 WeakConnection 之类的包装器来避免
//   回调期间对象失去所有权（具体以原版源码为准）；本仓库可核实：语义要求一样 —— 只能被 shared_ptr 持有。
// 【面试】“shared_from_this 的原理和前提？”→ 对象内藏 weak_ptr，shared_ptr 构造时回填控制块；
//   前提是对象已被 shared_ptr 管理，且继承必须是 public；构造函数里调用抛 bad_weak_ptr。
// 【语法】noncopyable 放在最前且不写 public：默认继承是 private，正好符合“禁止拷贝”
//   只需要对成员函数之外的世界不可见；两个基类合起来把本类钉成
//   “只能在堆上、只能被 shared_ptr 持有、不能复制”的对象。
class TcpConnection : noncopyable, public std::enable_shared_from_this<TcpConnection>
{
public:
// 【设计】构造参数五个：所属 loop（决定这条连接归哪个线程管）、连接名、connfd、
//   本端地址、对端地址。注意构造函数**不做**任何 epoll 注册、也不发上线通知 ——
//   那些留给 connectEstablished，因为此刻 shared_ptr 还没建好（见上面 bad_weak_ptr 那段）。
//   【线程】在 mainLoop 里执行（TcpServer::newConnection 的栈上），但对象属于 ioLoop。
//   【易错】sockfd 是“已经 accept 出来的 connfd”，不是 listenfd；本类通过成员 unique_ptr<Socket>
//   接管它，fd 的最终 close 由 ~Socket 负责（谁持有 Socket 谁负责关）。
    TcpConnection(EventLoop *loop,
                  const std::string &nameArg,
                  int sockfd,
                  const InetAddress &localAddr,
                  const InetAddress &peerAddr);
// 【语法】只在头里声明、定义在 .cc：这是 unique_ptr<不完整类型> 成员的必需品（见上面
//   class Channel; 那段）。【线程】析构发生在“最后一个 shared_ptr 放手”的那个线程，
//   本项目设计上保证是它自己的 loop 线程（TcpServer.cc 的 ~TcpServer 技巧 + connectDestroyed）。
    ~TcpConnection();

// 【设计】四个只读 getter，全部 inline、无锁、返回引用或裸指针（零拷贝零原子操作）。
// 【线程】任意线程都能调（只是读构造期就定下来的成员）；但注意返回的指针/引用所指对象
//   归 mainLoop 或 subLoop#N 所有，真正的使用地点是它自己的 loop 线程。
//   getLoop() 是 TcpServer::~TcpServer 能找到“该把 connectDestroyed 投给哪个线程”的关键；
//   name() 是 connections_ 的 key，removeConnectionInLoop 靠它 erase；
//   localAddress()/peerAddress() 给用户打日志用（testserver.cc 的 onConnection 就用 peerAddress）。
// 【易错】返回 const 引用要求对象活着；把这些引用存到回调之外（比如存进别处）会悬垂。
    EventLoop *getLoop() const { return loop_; }
    const std::string &name() const { return name_; }
    const InetAddress &localAddress() const { return localAddr_; }
    const InetAddress &peerAddress() const { return peerAddr_; }

// 【设计】connected() 只等于 state_ == kConnected，**不代表内核那条 TCP 连接还在**：
//   kDisconnecting（我们已经 shutdownWrite，但缓冲还在发）时它返回 false，
//   而 fd 依然可读写。判断“能不能 send”要看它，判断“链路真的活着”要看 read 返回值。
// 【语法】state_ 是 atomic_int，这里读它是一次 acquire load，不阻塞、不加锁。
// 【线程】任何线程都能读；但读到 true 之后并不保证下一秒还 true（无副作用的检查都有 TOCTOU）。
    bool connected() const { return state_ == kConnected; }

// 【流程】唯一的发送入口（⑨）：用户在自己的回调/业务代码里调，可能在 subLoop，也可能
//   在别的业务线程 —— 函数内部用 loop_->isInLoopThread() 分流：本线程直接 sendInLoop，
//   跨线程用 loop_->runInLoop 把发送动作交给 loop 线程（细节和【⚠问题】见 .cc 的 send / sendInLoop）。
// 【设计】只收 std::string 说明 muduo 是“消息级”API：拆包粘包由用户协议负责，库只管字节。
    // 发送数据
    void send(const std::string &buf);
// 【流程】本项目新增的零拷贝发送（对照 ⑨）：把文件描述符的内容直接送进 socket，
//   绕开 Buffer，走 sendfile 系统调用。【线程】和 send 一样内部做 isInLoopThread 分流。
//   【易错】它是“追加式”的：不知道也不管 outputBuffer_ 里是否还有待发数据，
//   与 send 混用可能让字节顺序乱（详见 .cc 的 sendFileInLoop 注释）。
    void sendFile(int fileDescriptor, off_t offset, size_t count); 
    
// 【设计】“关闭半连接”= 只关写端（shutdown(fd, SHUT_WR)，给对端发 FIN），读端还开着，
//   所以还能收到对端剩余的数据和最终的 FIN/RST。这才是“优雅关闭”的正确做法：
//   直接 close(fd) 会把读端也丢掉，未读数据可能被内核 RST 掉。
// 【流程】⑨/⑩/⑪ 的交界：用户主动 shutdown → state_ 迁 kDisconnecting → 等 outputBuffer_
//   排空 → shutdownInLoop 真正 shutdownWrite → 对端 read==0 后也关 → 本端 read==0 或
//   EPOLLHUP → handleClose → closeCallback_ → 对象回收（⑪）。
// 【对比原版】原版 muduo 的 shutdown 是否要求调用者必须在 loop 线程（assertInLoopThread 一类断言），
//   以原版源码为准。本仓库可核实：这里靠 runInLoop 保证 shutdownInLoop 在 loop 线程，但 state_ 改在调用者线程。
    // 关闭半连接
    void shutdown();

// 【设计】“回调三级传递”的第一级落点（用户 → TcpServer → TcpConnection → Channel）：
// 【线程】setter 只在 mainLoop 调用一次（TcpServer::newConnection 的栈上，连接建立之前）；
//   连接建立之后本类里这些成员只被读，不再被写，所以回调执行时（subLoop）读它们是安全的。
//   用户调 TcpServer::setXxxCallback 存进 TcpServer 成员；TcpServer::newConnection 里
//   再拷贝到每个 TcpConnection 的这些 setter；TcpConnection 在 handleXxx 里调用它们。
// 【易错】五个 setter 都没加锁、也没检查连接状态：必须在新连接建立之前由 TcpServer
//   调用（本项目就是这么用的）。已经跑起来之后再 set 只影响**新**连接，
//   老连接手里的那份副本不会变 —— 因为传的是拷贝，不是指针。
// 【易错】这些成员是 std::function，默认为空。connectEstablished / handleClose /
//   handleRead 调用它们时**没有判空**（对比 writeCompleteCallback_ 是判空的），
//   用户没注册就上线会抛 std::bad_function_call，在 subLoop 里没人 catch → std::terminate。
    void setConnectionCallback(const ConnectionCallback &cb)
    { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb)
    { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb)
    { writeCompleteCallback_ = cb; }
// 【设计】CloseCallback 不是给用户的：唯一设置者是 TcpServer::newConnection
//   （std::bind(&TcpServer::removeConnection, this, _1)）。用户如果手动覆盖它，连接对象
//   就永远不会从 TcpServer::connections_ 里摘掉 → fd 与内存泄漏。
// 【流程】⑪ 的起点，见 Callbacks.h 里 CloseCallback 那段“必须是 handleClose 最后一行”。
    void setCloseCallback(const CloseCallback &cb)
    { closeCallback_ = cb; }
// 【设计】唯一“回调 + 参数”成对设置的 setter：highWaterMark_ 和回调必须一起配，
//   否则阈值和通知函数可能对不上。默认 64MB 在构造函数初始化列表里给（见 .cc）。
// 【易错】两个赋值不在一个原子事务里；本项目只在连接建立前调用所以无害，
//   如果你在别的线程热改，sendInLoop 里读到的可能是新阈值配旧回调。
    void setHighWaterMarkCallback(const HighWaterMarkCallback &cb, size_t highWaterMark)
    { highWaterMarkCallback_ = cb; highWaterMark_ = highWaterMark; }

// 【流程】⑦ 的执行体：state_ 迁 kConnected → channel_->tie(shared_from_this()) →
//   enableReading()（第一次把 connfd 的 EPOLLIN 加进 epoll）→ 调 connectionCallback_ 通知用户上线。
// 【线程】必须在它自己的 subLoop 线程执行 —— 由 TcpServer::newConnection 通过
//   ioLoop->runInLoop 投递（【线程跳转 1】）。原因：enableReading → Channel::update →
//   Poller::updateChannel 要改 epoll 和 Poller 里的 ChannelMap，那些容器都没有锁。
// 【设计】enableReading 放在最后一步之前：先 tie 再注册事件，保证事件到来时对象一定活着。
    // 连接建立
    void connectEstablished();
// 【流程】⑪ 的收口：state_ 迁 kDisconnected + disableAll() + 通知用户下线 + channel_->remove()
//   （epoll_ctl DEL），之后 shared_ptr 放手时对象才真正析构、~Socket 才 close(connfd)。
// 【线程】同样必须在 subLoop：由 TcpServer::removeConnectionInLoop 用
//   ioLoop->queueInLoop 投递（【线程跳转 3】）。
// 【易错】函数里先判 if (state_ == kConnected)：已经 handleClose 过的连接再进来就只补一次
//   channel_->remove()，不会重复通知用户下线 —— 这就是“下线日志只打一次”的实现依据。
    // 连接销毁
    void connectDestroyed();

// 【设计】private 段分成三块：状态机 + 四个事件处理函数 + 三个 InLoop 函数 + 数据成员。
//   成员声明顺序就是构造/析构顺序（见下面成员处那条），所以 loop_/name_/state_ 先建，
//   socket_/channel_ 中间建，回调和 Buffer 最后建；析构严格反序。
private:
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ TcpConnection 四态状态机（全库对象生命周期最难的一段，配合 .cc 逐个函数看）  │
// └────────────────────────────────────────────────────────────────────────────┘
//
//                        ┌──────────────────┐
//                        │   （对象还不存在）│
//                        └────────┬─────────┘
//                TcpConnection 构造函数的初始化列表 state_(kConnecting)
//                    【线程 mainLoop：TcpServer::newConnection 的栈上】
//                                 ▼
//   ┌─────────────────┐  connectEstablished()   ┌────────────┐
//   │  kConnecting    │ ───────────────────────►│ kConnected │  ← connected() 为 true 的唯一状态
//   └─────────────────┘  ⑦ 在 subLoop 里执行     └─────┬──────┘
//         ▲                                            │
//         │                          ┌─────────────────┼──────────────────┐
//         │                  shutdown()         handleClose()      connectDestroyed()
//         │            （用户主动，⑨→⑩）  （对端先断，read==0       （⑪ 末，TcpServer
//         │                            ▼          或 EPOLLHUP）        投递回来的回收）
//         │                   ┌────────────────┐        ┌───────────────┐   ┌───────────────┐
//         │                   │ kDisconnecting │        │ kDisconnected │   │ kDisconnected │
//         │                   └───────┬────────┘        └───────────────┘   └───────────────┘
//         │                           │ shutdownInLoop()：socket_->shutdownWrite()
//         │                           │ 只关写端，发 FIN，读端仍可用
//         │                           ▼
//         │                    对端读完并关掉自己那端 → 本端 read 返回 0
//         │                           └──────────────► handleClose() → kDisconnected
//         │
//         └── 注意：没有任何“回到 kConnecting”的边 —— TcpConnection 是一次性对象，
//             断了就只能销毁，要重连必须让用户再连一次、由 Acceptor 生成新对象。
//
// 【设计】迁移表（谁把状态改成谁）：
//   ┌────────────────┬────────────────┬──────────────────────────┬───────────────────┐
//   │ 从             │ 到             │ 由哪个函数完成            │ 在哪个线程         │
//   ├────────────────┼────────────────┼──────────────────────────┼───────────────────┤
//   │ （构造）        │ kConnecting    │ TcpConnection::TcpConnection │ mainLoop       │
//   │ kConnecting    │ kConnected     │ connectEstablished        │ subLoop#N         │
//   │ kConnected     │ kDisconnecting │ shutdown                  │ 调用者线程（可能不是 subLoop）│
//   │ kConnected     │ kDisconnected  │ handleClose               │ subLoop#N         │
//   │ kConnected     │ kDisconnected  │ connectDestroyed          │ subLoop#N         │
//   │ kDisconnecting │ kDisconnected  │ handleClose               │ subLoop#N         │
//   │ kDisconnected  │ （对象析构）    │ 最后一个 shared_ptr 放手   │ 设计上=它自己的 subLoop │
//   └────────────────┴────────────────┴──────────────────────────┴───────────────────┘
// 【易错】state_ 是 std::atomic_int，但“原子”不等于“状态机正确”：
//   shutdown() 在调用者线程把 kConnected 改成 kDisconnecting，紧接着才把 shutdownInLoop
//   投给 subLoop，这两步不是一个原子事务；send() 在另一线程读 state_ 也可能读到
//   “kConnected 但已经没人会来发数据了”的中间态。真正的串行化靠的是“所有写操作都回到
//   loop 线程执行”这条规矩，不是靠 atomic。
// 【面试】“为什么需要 kDisconnecting 这个中间态？”→ 为了“数据发完再关”：写端还没排空就
//   shutdownWrite 会把未发送的数据丢掉，所以先发 FIN 之前要等 handleWrite 把
//   outputBuffer_ 清空，再在它的末尾补一次 shutdownInLoop。
    enum StateE
    {
        kDisconnected, // 已经断开连接
        kConnecting,   // 正在连接
        kConnected,    // 已连接
        kDisconnecting // 正在断开连接
    };
// 【设计】setState 是 inline 私有写入口；state_ 只有本类自己写，用户只能通过
//   connected() / shutdown() 间接触碰。【易错】它写成非虚、无锁，跨线程调用安全性
//   全靠上面的规矩（见 state_ 成员的【线程】说明）。
    void setState(StateE state) { state_ = state; }

// 【流程】⑧ 的执行体：readv 进 inputBuffer_ → messageCallback_ 交给用户；n==0 转 handleClose。
//   【线程】subLoop#N（在 Channel::handleEventWithGuard 的 readCallback_ 里被调）。
//   【设计】参数是 Timestamp（收包时刻）—— 由 Channel::handleEvent(receiveTime) 一路传下来。
// 【语法】private 函数却能被 std::bind(&TcpConnection::handleRead, this, _1) 取地址：
//   bind 表达式写在 TcpConnection 的成员函数（构造函数）里，类内访问自己的私有成员合法。
    void handleRead(Timestamp receiveTime);
// 【流程】⑩ 的执行体：EPOLLOUT 到达后把 outputBuffer_ 继续写进 fd，排空则 disableWriting。
//   【线程】subLoop#N。【设计】无参 —— Channel::writeCallback_ 是 std::function<void()>。
//   【易错】函数开头必须先判 channel_->isWriting()（原因见 .cc 的 handleWrite 注释）。
    void handleWrite();//处理写事件
// 【流程】⑪ 的第一现场：read 返回 0 或 EPOLLHUP 都会到这里（EPOLLHUP 那条路由
//   Channel::handleEventWithGuard 里的 closeCallback_ 触发）。
//   做三件事：state_→kDisconnected、disableAll、先 connectionCallback_ 再 closeCallback_。
//   【线程】subLoop#N。
    void handleClose();
// 【流程】EPOLLERR 的执行体：本项目只 getsockopt(SO_ERROR) 打日志，不关连接、不通知用户，
//   随后的断开要靠同批的 EPOLLHUP/EPOLLIN(读回 0) 来完成。【线程】subLoop#N。
    void handleError();

// 【设计】xxxInLoop 是本库的命名约定：“函数体里的东西只允许在 loop 自己的线程执行”。
//   对外函数（send/shutdown/sendFile）负责判断线程并投递，InLoop 版本负责真正干活。
//   【易错】任何地方直接调 sendInLoop 而不经过 send 分流，就是在错误的线程改 epoll 状态。
//   本仓库可核实：本项目这些函数里没有任何线程断言；原版是否有 assertInLoopThread 以原版源码为准。
//   【设计】sendInLoop 参数用 (const void*, size_t) 而不是 std::string，
//   是为了给 Buffer / 裸内存留余地，也避免在 functor 里多存一个字符串对象。
    void sendInLoop(const void *data, size_t len);
// 【流程】shutdown 的后半段（真正 socket_->shutdownWrite()）。【线程】subLoop#N。
    void shutdownInLoop();
// 【流程】sendFile 的后半段（真正的 sendfile 循环）。【线程】subLoop#N。
//   【设计】offset 是 off_t（不是指针）：内核 sendfile 会通过它传入的 &offset 就地推进，
//   函数再用推进后的值把自己重新排队，形成续传链（细节见 .cc）。
    void sendFileInLoop(int fileDescriptor, off_t offset, size_t count);
// 【设计】one loop per connection：一条连接绑死一个 EventLoop，之后所有读写、事件注册、
//   析构都在那个线程。这样 connfd 不需要任何锁。【线程】可能是 mainLoop（setThreadNum(0)
//   单 Reactor）或 subLoop#N（多 Reactor）—— TcpServer::newConnection 用轮询决定。
//   注意它是“指向别人拥有的对象”的裸指针：EventLoop 由用户栈（mainLoop）或
//   EventLoopThread（subLoop）拥有，本类不负责释放。
    EventLoop *loop_; // 这里是baseloop还是subloop由TcpServer中创建的线程数决定 若为多Reactor 该loop_指向subloop 若为单Reactor 该loop_指向baseloop
// 【设计】连接名 = TcpServer 的 name_ + “-ip:port#自增号”，在 TcpServer::newConnection 里
//   生成，唯一性靠 nextConnId_（不能用 fd：fd 会被内核复用）。它同时是
//   TcpServer::connections_ 这个 unordered_map 的 key，以及所有日志里的“这条连接的代号”。
// 【语法】const std::string 成员：一旦构造好就永不变，这样 name() 返回的 const 引用
//   在任何线程读都安全（没有写者，就不需要同步）。
    const std::string name_;
// 【设计】用 std::atomic_int 而不是普通 enum 变量：写者不止一个线程 ——
//   connectEstablished/handleClose/connectDestroyed 在 subLoop 写，shutdown 在调用者线程写；
//   而 send() 里“先读 state_ 再决定走不走 sendInLoop”可能在任意线程读。
//   atomic 保证读到的是某个完整状态值，不会出现撕裂。
// 【易错】状态机本身仍然不是“原子事务”：见上面 StateE 那段的【易错】。
// 【对比原版】原版 muduo 用普通 StateE 加“只在 loop 线程改状态”的约束（是否有 assertInLoopThread 保证，
//   以原版源码为准）；本仓库可核实：本项目把 state_ 定义成 std::atomic_int，因为确实允许跨线程调 shutdown/send。
    std::atomic_int state_;
// 【⚠问题】reading_ 是死成员：构造函数里初始化列表写了 reading_(true)，
//   但全库没有任何地方读它或再写它（grep 一下只有这两处）。
//   本仓库可核实：grep 全库 reading_ 只有“声明 + 初始化列表 reading_(true)”两处，没有任何读点、也没有第二个写点。
//   原版 muduo 是否也有这个废弃成员，以原版源码为准。**别把它当“当前是否监听读事件”的依据** —— 真相在
//   channel_->isReading() 里（events_ 的 EPOLLIN 位）。要问“这条连接在读吗”，
//   请调 conn->getLoop() 里那个 Channel 的 isReading()，或看 state_。
// 【易错】它还是个非 const 的 bool，和 state_ 一起说明“本类的布尔状态有两个来源”，
//   读代码时极易误判；面试问到就说“这是精简版的冗余成员，功能上无害但会误导”。
    bool reading_;//连接是否在监听读事件

    // Socket Channel 这里和Acceptor类似    Acceptor => mainloop    TcpConnection => subloop
// 【设计】两个“句柄”成员用 unique_ptr 而非值内嵌，是为了让 Socket / Channel 的类型
//   保持不完整（少 include 两个头，见上面 class Socket; 那段）。
//   所有权：TcpConnection 独占 connfd（→~Socket 里 close）和这条连接的 Channel
//   （→~Channel 不关 fd，只把自己从 Poller 摘掉的动作早在 connectDestroyed 做过）。
// 【线程】两个对象都在 subLoop 里创建（newConnection 的栈上其实在 mainLoop！注意区分：
//   构造发生在 mainLoop 的 newConnection 调用栈里，销毁发生在 subLoop —— 因为
//   connectDestroyed 是投递给 subLoop 执行的，它跑完后 functor 析构，最后一次减计数
//   也在 subLoop。跨线程构造/同线程析构是本项目生命周期最绕的一点，详见 .cc 注释）。
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

// 【设计】两个地址成员是 const，构造时一次填好（本端由 getsockname 查出来，对端由
//   accept4 填出来）。之后所有线程只读，所以打日志/查对端不需要加锁。
//   【易错】它们只是“快照”：连接断开后仍然能读到，别把它当成“连接还活着”的证据。
    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    // 这些回调TcpServer也有 用户通过写入TcpServer注册 TcpServer再将注册的回调传递给TcpConnection TcpConnection再将回调注册到Channel中
// 【设计】这里是“回调三级传递”的末端：用户 → TcpServer → TcpConnection（本处的拷贝）
//   → 要么在 handleXxx 里直接调（connectionCallback_ / messageCallback_ / closeCallback_），
//   要么在数据发完时 queueInLoop 调（writeCompleteCallback_ / highWaterMarkCallback_）。
//   同一个 ConnectionCallback 会被两处调用（connectEstablished 与 handleClose），
//   靠 connected() 区分上线/下线，所以**用户回调必须自己判 connected()**。
// 【易错】五个成员都是 std::function（可空）。调用点分两种风格：
//   判空的：writeCompleteCallback_、highWaterMarkCallback_（见 sendInLoop / handleWrite）；
//   不判空的：connectionCallback_（connectEstablished / handleClose 里裸调）、
//   messageCallback_（handleRead 里裸调）、closeCallback_（handleClose 里裸调）。
//   用户只 setMessageCallback 不 setConnectionCallback 的话，subLoop 里抛
//   std::bad_function_call 且无人 catch，进程直接 std::terminate。
// 【对比原版】本仓库可核实：本项目 TcpServer 构造函数里把 connectionCallback_/messageCallback_ 显式
//   初始化成**空** std::function，没有任何默认回调兜底；原版 muduo 是否提供 defaultConnectionCallback
//   一类的默认实现，以原版源码为准。
    ConnectionCallback connectionCallback_;       // 有新连接时的回调
    MessageCallback messageCallback_;             // 有读写消息时的回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完成以后的回调
    HighWaterMarkCallback highWaterMarkCallback_; // 高水位回调
    CloseCallback closeCallback_; // 关闭连接的回调
// 【设计】高水位阈值，单位字节，默认 64MB（在构造函数初始化列表里给）。
//   和 highWaterMarkCallback_ 一起由 setHighWaterMarkCallback 配。见 .cc 里
//   sendInLoop 的“跨越阈值才通知一次”那三个条件。
    size_t highWaterMark_; // 高水位阈值

    // 数据缓冲区
// 【设计】两个缓冲区按值内嵌（这就是必须 include Buffer.h 的原因）：
//   inputBuffer_ 只由 Buffer::readFd 在 subLoop 里写、用户回调读；
//   outputBuffer_ 只由 sendInLoop 写、handleWrite 读并 retrieve。
//   收发分离 → 一条连接的读路径和写路径不会互相等待（同线程，本来也不需要锁）。
// 【易错】发送缓冲在 muduo 里是**无上限**的：只有 highWaterMarkCallback_ 提醒，不会拒绝你
//   send。对端不收 + 你一直发 = 内存被 outputBuffer_ 吃光（原版的处置方式以原版源码为准；本仓库
//   可核实的处置只有一条：highWaterMarkCallback_ 通知，不拦截）。
    Buffer inputBuffer_;    // 接收数据的缓冲区
// 【流程】用户 send 时若一次 write 没发完，剩余字节就 append 到这里，然后 enableWriting，
//   由 handleWrite（⑩）接力发完。它非空时 channel 一定处于 isWriting() 状态 ——
//   这条不变式是 sendInLoop 里“能不能直接 write”判断的依据（见 .cc）。
//   它也是 shutdown 的挡路者：outputBuffer_ 没排空就不能 shutdownWrite，否则数据被丢。
    Buffer outputBuffer_;   // 发送数据的缓冲区 用户send向outputBuffer_发
};
