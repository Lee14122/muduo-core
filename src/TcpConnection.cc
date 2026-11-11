/* ============================================================================
 * 【文件】src/TcpConnection.cc
 * 【一句话】实现“一条连接”的全部读写与生命周期：把 epoll 事件翻译成 handleRead/handleWrite/
 *          handleClose/handleError，把用户数据送进内核或暂存进 outputBuffer_，
 *          并用 shared_from_this 保证对象在“自己把自己删掉”的路上不死。
 * 【Reactor 角色】Handler（Reactor 四层里的“处理一个连接的业务读写入口”）
 * 【依赖】TcpConnection.h（自己的声明）；Socket.h + Channel.h（头里只前向声明，本文件必须
 *        看到完整定义 —— 因为 ~TcpConnection 在这里定义，unique_ptr 的删除器要 sizeof/调析构）；
 *        EventLoop.h（isInLoopThread / runInLoop / queueInLoop）；Logger.h（LOG_*）；
 *        <errno.h>（EWOULDBLOCK/EPIPE/ECONNRESET）、<sys/sendfile.h>（sendfile）、
 *        <sys/socket.h>（getsockopt/SO_ERROR）、<netinet/tcp.h>（TCP_NODELAY 等选项号）。
 * 【谁用它】TcpServer::newConnection 构造它；Channel 通过构造时绑好的四个成员函数回调
 *        把事件送进来；用户在 ConnectionCallback/MessageCallback 里通过 TcpConnectionPtr
 *        调 send / sendFile / shutdown。
 * 【先修知识】① 非阻塞 fd 上 write 的三种返回（==len / <len / -1 且 EWOULDBLOCK）；
 *        ② shared_ptr + enable_shared_from_this + weak_ptr；③ epoll 的水平触发与 EPOLLOUT；
 *        ④ runInLoop/queueInLoop 的线程语义（见 src/EventLoop.cc）。
 * 【难度】★★★★★   【建议耗时】60 分钟（本库最难的两个文件之一，注释量大于代码量）
 * 【自检】1) sendInLoop 为什么先尝试直接 write？“!isWriting() && outputBuffer_ 为空”
 *           这个条件为什么是必须的，去掉会出什么事故？
 *        2) 高水位回调的三个条件各挡住哪种情况，为什么能做到“只在跨越阈值时通知一次”？
 *        3) handleWrite 开头判 isWriting 是为了防什么？末尾为什么要 disableWriting？
 *        4) shutdown 和 handleClose 区别是什么？为什么 closeCallback_ 必须是最后一行？
 * ============================================================================ */
#include <functional>
#include <string>
// 【语法】errno 是一个宏，展开成某个“线程局部”int 变量的引用（glibc 里是 __errno_location()），
//   所以每个线程各有一份 errno —— 这是本项目大量“在 subLoop 里读 errno”能成立的前提。
//   【易错】errno 只表示“最后一次失败的调用”，任何成功的调用都不清零它；日志函数
//   LOG_ERROR 内部会 read/write/malloc，跑一次就可能改掉 errno，所以要先把值存进 savedErrno
//   （handleRead / handleWrite 就是这么做的，本项目唯一的 sendInLoop 反而没防这一手）。
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <fcntl.h> // for open
#include <unistd.h> // for close

// 【设计】include 顺序体现了一个硬性要求：本文件的 ~TcpConnection 定义在这里，
//   而它的成员是 unique_ptr<Socket> / unique_ptr<Channel>，删除器需要完整类型，
//   所以 Socket.h 和 Channel.h 必须在这儿出现（而不是只在头里前向声明）。
// 【对比原版】原版 muduo 在多数内部函数里带线程断言一类的保护（本环境没有原版源码可核对，具体以原版源码为准）。
//   本仓库可核实的事实：这里没有任何 InLoop 函数带线程断言，全靠调用方 runInLoop 分流保证线程正确。
#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

// 【语法】static 函数 = 内部链接(internal linkage)，只在本翻译单元可见。
//   TcpServer.cc 里有一个同名同体的 CheckLoopNotNull：如果都不加 static，链接期就会
//   duplicate symbol（同名非 static 函数违反 ODR）。加 static 后两份各归各的文件，互不打架。
//   【设计】“为什么不直接把 if 写在构造函数初始化列表里”：初始化列表里不能写语句，
//   所以用一个“边算边检查”的函数把检查塞进 loop_ 的初始化表达式里。
//   【⚠问题】LOG_FATAL 展开里是 exit(-1)（见 include/Logger.h），它不走 RAII 析构、
//   不刷新 std::cout 缓冲，日志可能丢；因此这个函数的 return loop 只在“非空”时可达 ——
//   这也是为什么它一定要 return：去掉 return 就是 UB（返回未定义值）。
// 【流程】②/⑥ 的前置检查工具（TcpServer 与 TcpConnection 各有一份）。
// 【线程】随调用者执行，实际都在 mainLoop（用户在主线程构造服务器）。
static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null!\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 构造函数：全库“对象生命周期最复杂”这条链的起点（流程 ⑥）                     │
// └────────────────────────────────────────────────────────────────────────────┘
// 【线程】执行在 mainLoop（TcpServer::newConnection 的调用栈上），但它服务对象的是 ioLoop。
//   这是本项目少见的“跨线程构造”：对象在 A 线程 new，之后所有事件/析构都在 B 线程。
//   之所以安全：构造过程只碰局部变量和成员初始化，不碰 epoll、不查 shared_from_this。
// 【设计】构造函数刻意只做三件事：填成员、把四个事件回调绑到 channel_、打开 SO_KEEPALIVE。
//   它**不**注册 EPOLLIN、**不**通知用户上线 —— 那是 connectEstablished 的活（流程 ⑦），
//   原因有两个：① 此刻还没有 shared_ptr 接管 this，shared_from_this() 会抛 bad_weak_ptr；
//   ② epoll 相关操作必须在 ioLoop 线程执行，构造函数在 mainLoop 线程。
// 【流程】⑥：TcpServer::newConnection 里 new TcpConnection 的那一刻（对象生命周期的起点）。
TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string &nameArg,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
// 【语法】注意这里传的是形参 loop 而不是 loop_（下面两行同理用 sockfd 而不是 socket_->fd()）。
//   看起来多余，但它躲开了一个真实陷阱：成员初始化按**声明顺序**进行，不按初始化列表书写顺序。
//   头文件里 loop_ 声明在最前，所以 loop_ 一定先被初始化；但若某个后声明的成员想引用
//   先声明成员，就必须依赖那条规矩。直接用工位上的形参 loop/sockfd，等于完全不依赖顺序，
//   读代码时不必回头数声明。
//   【易错】编译器 -Wreorder 会在“初始化列表顺序 != 声明顺序”时警告（本项目
//   TcpServer.cc 就真实触发了这个警告）。成员是 const 或引用时尤其要数顺序。
    : loop_(CheckLoopNotNull(loop))
    , name_(nameArg)
// 【设计】state_ 直接写 kConnecting 而不是 kDisconnected：对象已建好、connfd 已 accept 出来，
//   只是还没 enableReading。选 kConnecting 还带来一个副作用：connected() 返回 false，
//   于是“用户在 connectEstablished 之前拿到这个 conn 也发不了数据”（send 里 if (state_==kConnected)）。
//   【易错】kConnecting 在本项目里不是一个“有动作”的状态 —— 没有任何超时/重连逻辑处理它，
//   如果 connectEstablished 因故没被投递执行，这条连接会永远停在 kConnecting：
//   fd 不注册、不通知、不回收，直到进程退出。（原版里 kConnecting 主要服务于 TcpClient 主动连接，是否带超时以原版源码为准。）
    , state_(kConnecting)
// 【⚠问题】reading_(true) 是本类里唯一的“写了没人读”的初始化：全库没有任何地方使用
//   reading_（想判断是否在读，要用 channel_->isReading()）。见头文件 reading_ 处的说明。
    , reading_(true)
// 【设计】所有权交接的一刻：newConnection 从 accept4 拿到的裸 connfd 在这里被
//   unique_ptr<Socket> 接管。从此 fd 的 close 由 ~Socket 负责（唯一一处 close）。
//   【易错】new Socket(sockfd) 之后 sockfd 这个 int 值在调用者手里仍然是“可用”的，
//   但语义上已经归本对象；TcpServer::newConnection 之后确实也没再用它。
//   【⚠问题】如果 new Channel(...) 抛异常（比如 bad_alloc），socket_ 已构造、会自动析构并
//   close fd —— 这条“每个成员各自 RAII”的链是本项目唯一防泄漏的手段，所以成员顺序不能乱调。
    , socket_(new Socket(sockfd))
// 【设计】Channel 此刻只是“登记了 fd 和 loop”，events_ 还是 0，epoll 里根本没有它。
//   真正的注册在 connectEstablished → enableReading → update → Poller::updateChannel。
// 【对比原版】本仓库可核实：本项目 Channel 构造签名是 Channel(EventLoop*, int fd)，且 tie 只发生在 connectEstablished。
//   原版 muduo 的 tie 位置与写法以原版源码为准（本环境未能核对原版）。
    , channel_(new Channel(loop, sockfd))
    , localAddr_(localAddr)
    , peerAddr_(peerAddr)
// 【设计】高水位默认 64MB（单位：字节），和 sendInLoop 里三个条件配合使用。
//   为什么默认给这么大：muduo 的语义是“库替你缓冲”，太小会让正常的大响应频繁触发回调；
//   真正的目的是防“对端不收 + 自己猛发”导致内存爆掉，而不是做流控精度。
    , highWaterMark_(64 * 1024 * 1024) // 64M
{
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 四个事件回调的绑定：TcpConnection 与 Channel 之间唯一的连线                  │
// └────────────────────────────────────────────────────────────────────────────┘
// 【设计】为什么构造时要绑 4 个成员函数回调：Channel 是“哑管道”，它只知道自己有个 fd
//   和四个 std::function，不认识 TCP、不认识 EventLoop 之外的任何东西。谁想让 fd 上的
//   EPOLLIN/EPOLLOUT/RST/ERR 变成一次函数调用，就得把函数塞给它。
//   本类的四个函数是 private 的：bind 写在成员函数体内，属于“类内访问私有成员”，合法；
//   这也解释了“为什么这四个函数在 private 段而 Channel 能调它们”——Channel 调的是
//   std::function，不是直接点函数名。
// 【线程】绑定动作在 mainLoop 做（构造函数），调用发生在 subLoop（Channel::handleEvent）。
//   std::function 对象建好之后是只读的（连接存续期间没人再 set），所以跨线程读安全。
// 【易错】这四根线**永远指向 this 裸地址**，bind 不延长对象寿命。一旦对象先死而事件后到，
//   调用就是 use-after-free。兜底手段是 connectEstablished 里的 channel_->tie(shared_from_this())
//   ——Channel::handleEvent 会先 tie_.lock()，锁不上就直接跳过所有回调。
// 【对比原版】原版 muduo 不用裸 this，而是用
//   shared_ptr<TcpConnection> conn = shared_from_this(); 配合一个只存 weak_ptr 的
//   MemberFunction 包装器（muduo::bind(&TcpConnection::handleRead, weakConn, _1)），
//   在回调对象内部再提升一次，安全性更明确；本项目靠 Channel::tie 达到同样效果，
//   但依赖“tie 一定在 enableReading 之前”这条顺序规矩。
    // 下面给channel设置相应的回调函数 poller给channel通知感兴趣的事件发生了 channel会回调相应的回调函数
// 【语法】std::bind 与 std::placeholders::_1 到底在干什么：
//   Channel::setReadCallback 收的是 std::function<void(Timestamp)>（见 Channel.h 的
//   ReadEventCallback）。而 handleRead 的签名是 void(Timestamp)。取成员函数指针
//   &TcpConnection::handleRead 之后，它的“第一个隐含参数”是对象本身（this），
//   所以必须补两个实参：第 0 个是对象，第 1 个才是 Timestamp。
//     std::bind(&TcpConnection::handleRead, this, std::placeholders::_1)
//        含义：造一个可调用对象，调用它时执行 this->handleRead(第1个实参)
//   _1 不是“第一个参数”，它是一个占位符：“把将来调用 bind 结果时传进来的第 1 个实参
//   放到这里”。这样 bind 结果的签名就是 (Timestamp) → 正好塞进 function<void(Timestamp)>。
// 【语法】对比下面三个：handleWrite/handleClose/handleError 都是**无参**成员函数，
//   而 Channel 的 writeCallback_/closeCallback_/errorCallback_ 是 function<void()>，
//   所以 bind 只补 this，不写占位符 —— 签名刚好是 ()。
// 【易错】“占位符个数必须和被调函数的剩余参数个数一致”不是硬性规定！std::bind 的结果
//   可以接受比它用到的更多的实参，多出来的那些会被**丢弃**（只要求可隐式转换、可求值）。
//   这就是 src/Acceptor.cc 里 std::bind(&Acceptor::handleRead, this)（handleRead 无参，
//   而 readCallback_ 带一个 Timestamp）也能编过的原因：那个时间戳被丢掉了。
// 【流程】这四根线接上之后，事件流向变成：
//   epoll_wait → EPollPoller::poll 填 revents_ → EventLoop::loop 遍历 activeChannels_
//   → Channel::handleEvent → handleEventWithGuard 按 revents_ 位分发
//   → 这里的四个 bind 结果 → 下面四个 handleXxx。
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

// 【流程】⑥ 的日志（构造函数），打印在 mainLoop 线程；配对的析构日志在
//   ~TcpConnection（subLoop 线程）。两条日志的 fd 相同，可以用来确认“构造/析构各在哪个线程”。
    LOG_INFO("TcpConnection::ctor[%s] at fd=%d\n", name_.c_str(), sockfd);
// 【系统调用】SO_KEEPALIVE：让内核定期给对端发探测包（默认首次空闲 7200 秒、间隔 75 秒、
//   失败 9 次），对端已死时内核把连接置成错误，本端随后收到 EPOLLERR/EPOLLHUP 或 read 报错，
//   从而回收“僵尸连接”。这三次参数能用 TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT 调，本项目没调。
// 【设计】库在这里替用户开 keepalive 而不是交给用户，是因为 one-loop-per-thread 模型里
//   “连接不释放就一直占 fd 和内存”。
//   【易错】SO_KEEPALIVE 对“对端进程崩溃但主机还在”这种情况特别有用；对端主机断电时
//   只能靠它，7200 秒的默认值对业务来说太长，所以生产环境一般还要自己加应用层心跳。
// 【对比原版】本仓库可核实：这里只调了 socket_->setKeepAlive(true)；Socket::setTcpNoDelay 在全库**没有任何调用点**
//   （声明与实现都在，备而未用），TcpConnection 也没暴露改这两个选项的接口，于是连接上 TCP_NODELAY 是内核默认值（关，即 Nagle 生效）。
//   原版 muduo 是否默认打开某些选项，以原版源码为准。
    socket_->setKeepAlive(true);
}

// 【线程】“最后一个 shared_ptr 在哪个线程放手，析构就在哪个线程跑”。本项目通过
//   connectDestroyed 必须由 ioLoop->queueInLoop 投递（TcpServer::removeConnectionInLoop）
//   以及 ~TcpServer 里 conn->getLoop()->runInLoop 这两条路，把析构固定在它自己的 loop 线程。
//   【设计】为什么要固定：成员析构要跑 ~Channel（它不再碰 epoll，但 index_ 语义要求已经
//   remove 过）和 ~Socket（close(fd)）。fd 的 close 若与其他线程正在 epoll_wait 的同一 fd
//   并发，就是经典的“close 与 epoll 竞争”问题；限定在 loop 线程就没这个麻烦。
// 【易错】析构函数里还能安全地用 channel_->fd()，因为成员尚未销毁（析构函数体先跑，
//   成员后销毁）。但如果在这里调 shared_from_this()，std::bad_weak_ptr / UB ——
//   控制块计数已 0，shared_from_this 约定不允许在析构期间使用。
// 【语法】成员销毁顺序与构造相反：outputBuffer_/inputBuffer_ → highWaterMark_ → 五个
//   std::function → 两个 InetAddress → channel_ → socket_ → reading_ → state_ → name_ → loop_。
//   注意 socket_ 在 channel_ **之后**销毁（声明顺序 socket_ 在前），所以是“先销毁 Channel，
//   再销毁 Socket”，fd 的 close 在最后一刻发生，正好符合“Channel 的 remove 早已做完”。
// 【流程】⑪ 的最末梢（连接对象真正消失），以及 ~TcpServer 退出路径（⑫）的汇合点。
TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::dtor[%s] at fd=%d state=%d\n", name_.c_str(), channel_->fd(), (int)state_);
}

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ send：用户唯一的发送入口（流程 ⑨），核心是“线程分流”                        │
// └────────────────────────────────────────────────────────────────────────────┘
// 【流程】⑨ 起点。上游是用户的 onMessage/业务代码；下游是 sendInLoop。
// 【线程】调用线程不确定（subLoop 里的 onMessage、或用户自己的业务线程），
//   函数自己用 loop_->isInLoopThread() 分两路：
//   ① 在 loop 线程 → 就地调 sendInLoop（省一次投递、零原子操作、零 eventfd 写）；
//   ② 不在 loop 线程 → loop_->runInLoop(...)，它内部是 queueInLoop：加锁 push 进
//      pendingFunctors_ + 写 wakeupFd_（eventfd）唤醒那个线程的 epoll_wait。
// 【设计】“为什么一定要回到 loop 线程”：sendInLoop 会碰 channel_ 的 events_、会
//   epoll_ctl MOD、会读写 outputBuffer_ —— 这些容器和状态都没有锁，多线程同时动必炸。
//   muduo 的并发策略是“数据归属线程”，不是“给数据加锁”。
// 【易错】state_ != kConnected 时本函数**静默丢弃**你要发的数据，连 else 都没有：
//   不报错、不返回失败值。想确认是否真发出去，只能自己判 conn->connected() 或用
//   WriteCompleteCallback。面试问“muduo 怎么保证发送不丢”→ 答：它不保证，靠使用者。
// 【⚠问题】跨线程分支里 bind 的实参是 buf.c_str()（一个 const char*）和 buf.size()：
//   std::bind 把“指针的值”存进 functor，**不拷贝字符串内容**。而 buf 是 send 的形参，
//   send 一返回，那份 std::string 就析构、堆内存被释放；functor 之后在 subLoop 里执行时
//   拿着的是悬垂指针 —— 传给 ::write 就读到已释放内存（use-after-free）。
//   更糟的是 conn->send(“hello”) 这种临时串写法：临时 std::string 在整条语句结束就销毁，
//   悬垂几乎必然发生。只有 isInLoopThread 为真的那条路安全（同一个栈帧内同步执行完）。
// 【对比原版】跨线程发送的安全姿势是**把数据内容拷进 functor**（按值持有一份 std::string，代价是一次拷贝），
//   原版 muduo 即采用这一思路（其具体写法与注释以原版源码为准，本环境未能核对原版）。
//   本仓库可核实的事实：这里 std::bind 的实参是 buf.c_str()，functor 里存的是 const char*，只拷了指针。
// 【面试】“muduo 的 send 是线程安全的吗？”→ API 层面是（可跨线程调，内部 runInLoop 分流），
//   但“安全性来自把数据搬到目标线程”：要么同步拷数据进 functor（原版的思路），
//   要么要求数据在 loop 线程内当场写完（本项目这一版有悬垂风险）。
void TcpConnection::send(const std::string &buf)
{
    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread()) // 这种是对于单个reactor的情况 用户调用conn->send时 loop_即为当前线程
        {
            sendInLoop(buf.c_str(), buf.size());
        }
        else
        {
            loop_->runInLoop(
                std::bind(&TcpConnection::sendInLoop, this, buf.c_str(), buf.size()));
        }
    }
}

/**
 * 发送数据 应用写的快 而内核发送数据慢 需要把待发送数据写入缓冲区，而且设置了水位回调
 **/
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ sendInLoop：muduo 发送路径的全部智慧都在这 60 行（流程 ⑨/⑩ 的分界处）      │
// └────────────────────────────────────────────────────────────────────────────┘
// 【线程】只允许在 loop_ 所属线程执行（由 send 分流保证）。
//   本仓库可核实：这里没有线程断言（原版是否有 assertInLoopThread 一类的断言，以原版源码为准）。
// 【设计】算法总览（背下这张图）：
//
//   ① 状态检查   state_==kDisconnected? → 只打日志，不 return【⚠问题】，继续往下
//   ② 能否直接写  !isWriting() && outputBuffer_.readableBytes()==0
//        真 → ::write(fd, data, len)
//               >=0：remaining = len-nwrote；remaining==0 → queueInLoop(writeCompleteCallback_)
//               <0 ：nwrote=0；errno!=EWOULDBLOCK → 打日志；EPIPE/ECONNRESET → faultError=true
//        假 → 完全不调 write（因为有人排在前头，或已经在等 EPOLLOUT）
//   ③ 剩余处理   !faultError && remaining>0
//        先算 oldLen=outputBuffer_.readableBytes()
//        oldLen<hw && oldLen+remaining>=hw && 有回调 → queueInLoop(highWaterMarkCallback_)
//        outputBuffer_.append(data+nwrote, remaining)
//        !isWriting() → channel_->enableWriting()   ← 注册 EPOLLOUT，剩下交给 handleWrite
//
// 【设计】“为什么先尝试直接 write”：绝大多数场景一次调用就发完了（小报文：几十字节到
//   几 KB，内核发送缓冲区默认就有 2.5 页 ~ 几十 KB，远够用）。这条路：
//   零额外内存拷贝（不 append 进 outputBuffer_，省一次 memcpy）、零 Buffer 空间增长、
//   零 epoll_ctl 系统调用、零“等一轮 EPOLLOUT”的延迟。
//   只有在写不完时才付出“进缓冲区 + 注册 EPOLLOUT + 下轮再写”的代价 —— 这叫快路径优化。
// 【设计】“为什么 !isWriting() && outputBuffer_.readableBytes()==0 是必要条件”：
//   ① outputBuffer_ 里有数据，说明前面还有一批没发完的字节。TCP 是**字节流**，顺序是
//      协议的一部分；此刻若直接 write 新数据，新字节会插到旧字节前面 → 用户看到的
//      数据顺序错乱，协议解码直接废掉。这是“必须为假就不写”的第一原因。
//   ② isWriting() 为真说明 EPOLLOUT 已注册，handleWrite 正在/即将把缓冲区往外倒。
//      再直接 write 一次等于两个地方并发倒数据（虽然同线程不会真并发，但会重复发
//      同一段 buffer 内容 —— handleWrite 读的是 buffer，新 write 读的是 data，顺序仍乱）。
//   ③ 反过来，两个条件都成立 = “链路上没有任何在途数据” → 直写是唯一且安全的动作。
// 【易错】这两个条件缺一不可，也别以为“isWriting 为真就一定有数据”：
//   enableWriting 是在 append 之后做的，而 disableWriting 是在 buffer 排空之后做的，
//   所以不变式是“buffer 非空 ⇒ isWriting 为真”；反之不成立（isWriting 可能刚开、buffer 已空）。
//   代码里两处都判，是为了把“理论上不该发生但可能”的组合也挡住（防御性写法）。
// 【⚠问题】state_==kDisconnected 时缺一个 return（详见下面那条注释）。
// 【流程】⑨ 的执行体（用户 send 的落点）：能直写就直写，不能就进 outputBuffer_ + enableWriting 交给 ⑩。
// 【线程】只允许在 loop_ 所属线程（subLoop#N，单线程模式则 mainLoop）执行，由 send 的分流保证。
void TcpConnection::sendInLoop(const void *data, size_t len)
{
// 【设计】三个局部变量的角色（单位都是字节）：
//   nwrote = 本次 write 实际交给内核的字节数（失败时强制归 0，供后面 append 当偏移）；
//   remaining = 还没进内核的字节数，初值 len，是“要不要进缓冲区”的唯一判据；
//   faultError = 连接是否已经坏掉（EPIPE/ECONNRESET），坏掉就**不再往 buffer 里堆数据**。
// 【语法】用 ssize_t 而不是 int：write 返回 ssize_t（可为 -1），在有符号数里比较才安全；
//   size_t 是无符号，写成 size_t nwrote 再和 0 比会永远 >=0。
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

// 【⚠问题】这里只 LOG_ERROR 而没有 return（或 else）：判出“已断开、放弃写入”之后
//   代码继续往下跑，仍然会 write / append / enableWriting。后果：
//   ① 往已经 kDisconnected 的 fd 上写 —— fd 可能已被 ~Socket close，甚至已被别的连接
//      复用（fd 号回收）→ 轻则 EPIPE，重则把数据写进别人那条连接（串号，安全事故级）；
//   ② 给一条没人回收的连接继续堆 outputBuffer_，内存有涨无落。
//   正确写法是 打日志后 return。本项目保留原样只作提示（铁律：不许改代码）。
// 【对比原版】这个 kDisconnected 检查是精简版自己加的“一半防护”（原版有无同类检查，以原版源码为准）：
//   它只提示不拦截，等于加了检查却没加 return —— 规范点名的【⚠问题】正是这一半。
    if (state_ == kDisconnected) // 之前调用过该connection的shutdown 不能再进行发送了
    {
        LOG_ERROR("disconnected, give up writing");
    }

// 【设计】条件注释（原注释说的“第一次开始写数据”= 这条连接还没注册过 EPOLLOUT）。
    // 表示channel_第一次开始写数据或者缓冲区没有待发送数据
// 【流程】⑨ 的分岔点。【线程】subLoop。见上面函数头那段“必要条件”的完整推导。
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
// 【系统调用】write(fd, buf, count) 在非阻塞 socket 上的三种结果，一定要背：
//   ① 返回 count：全部字节已拷进内核发送缓冲区（注意：这只表示“进了内核”，
//      不代表对端收到，更不代表落盘/送达；TCP 会自己重传）。
//   ② 返回 0 < n < count（短写）：内核缓冲区只塞下这么多，n 之前的字节已经进内核，
//      n 之后的必须自己接着发 —— 本项目就是把剩下的 append 进 outputBuffer_。
//   ③ 返回 -1：此时 errno 说明原因。
//        EWOULDBLOCK/EAGAIN：缓冲区一个字节都塞不下（非阻塞的正常表现，不是错误）；
//        EPIPE/ECONNRESET：对端已关闭或已 RST（真错误）；
//        EINTR：被信号打断（可以重试；本项目不重试，交给下一轮 EPOLLOUT，行为仍然正确）；
//        EBADF：fd 无效 —— 正是本函数开头 state_==kDisconnected 那条缺 return 的分支可能引发的场面。
//   阻塞 fd 上 write 会一直睡到全部写完，所以这套代码要求 fd 一定是非阻塞的
//   （connfd 由 Socket::accept 的 accept4(SOCK_NONBLOCK) 拿到，listenfd 由 socket(SOCK_NONBLOCK)）。
// 【易错】对端已关闭时 write 除了返回 -1/EPIPE 还会发 SIGPIPE；本仓库没有任何地方
//   signal(SIGPIPE, SIG_IGN)，也没用 MSG_NOSIGNAL —— 默认动作是**终止进程**。使用者必须
//   自己在 main 第一行忽略 SIGPIPE（陈硕在书里也明确要求这一点）。
        nwrote = ::write(channel_->fd(), data, len);
// 【流程】写成功（含短写）分支。【线程】subLoop。
        if (nwrote >= 0)
        {
// 【设计】remaining = len - nwrote：一次发完 → 0；短写 → 剩余量，交给下面的 append。
            remaining = len - nwrote;
// 【设计】两个条件同时满足才通知用户“发完了”：真的剩 0，且用户注册了回调（判空！
//   对比 connectionCallback_/messageCallback_ 的调用点是不判空的）。
//   【易错】这条快路径通知的是“本次 send 的数据全进了内核”，不是“对端收到了”。
            if (remaining == 0 && writeCompleteCallback_)
            {
// 【设计】为什么用 queueInLoop 而不是就地调用 writeCompleteCallback_：
//   现在栈上是 sendInLoop ← send ← 用户的 onMessage。如果就地调用户回调，
//   用户很可能在回调里再 conn->send(...)（比如“发完这条再发下一条”），
//   于是发生同一函数递归重入，outputBuffer_/状态机在半个更新里被再次触碰。
//   排进 pendingFunctors_ 就等于“本轮 IO 事件全部处理完之后，再作为独立任务执行”。
//   【易错】用 queueInLoop 而不是 runInLoop 还有个好处：runInLoop 在“已经在 loop 线程”时
//   会立即执行，而这里正是在 loop 线程 —— 用了 runInLoop 反而会当场重入。
// 【对比原版】原版同样倾向把这类“发完通知”排到本轮事件处理之后再执行（原版的注释原文以源码为准，本环境未能核对）。
//   本仓库可核实：这里用的是 queueInLoop，不是就地调用，也不是 runInLoop。
                // 既然在这里数据全部发送完成，就不用再给channel设置epollout事件了
                loop_->queueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this()));
            }
        }
// 【设计】write 失败分支：先把 nwrote 归零，意思是“一个字节都没进内核”，
//   于是下面的 append((char*)data + nwrote, remaining) 自然是从 data[0] 开始全量缓存。
//   【易错】这一步很重要：如果忘了归零，nwrote 是 -1，data + (-1) 就是越界指针。
//   remaining 也保持 len（没执行 remaining = len - nwrote），所以缓存的是整个报文，不会少。
        else // nwrote < 0
        {
            nwrote = 0;
// 【系统调用】EWOULDBLOCK 与 EAGAIN 在 Linux 上是同一个值（-EWOULDBLOCK == -EAGAIN，
//   都是 11），写成 != EWOULDBLOCK 就同时挡掉了这两种“非阻塞没空间”的正常情况。
//   【设计】为什么 EWOULDBLOCK 不当错误：它只是“内核此刻塞不下”，数据应该走缓冲区，
//   由 EPOLLOUT 接力 —— 是流程 ⑩ 的正常入口，不是故障。
//   【易错】反过来，如果这里把 EWOULDBLOCK 也当错误打日志，繁忙服务的高频短写会把
//   日志刷爆（本项目 LOG_ERROR 每次都要格式化 + 写 stderr）。
            if (errno != EWOULDBLOCK) // EWOULDBLOCK表示非阻塞情况下没有数据后的正常返回 等同于EAGAIN
            {
                LOG_ERROR("TcpConnection::sendInLoop");
// 【系统调用】EPIPE：对端已经 close（收到过 FIN 后再写）；
//   ECONNRESET：对端 RST（比如它进程崩了/端口没人，或写触发了对端的复位）。
//   两者都说明“这条连接永远不可能再把数据送出去”。
// 【设计】faultError=true 的作用只有一个：让下面的 if (!faultError && remaining>0) 不成立，
//   于是不 append、不 enableWriting —— 不给死连接继续堆内存，也不会注册一个
//   “永远不会准备好”的 EPOLLOUT（LT 下对已断连接的 EPOLLOUT 会一直立刻就绪 → 空转）。
// 【易错】与 EWOULDBLOCK 的处理差异要讲清：
//   EWOULDBLOCK = “慢一点”，走缓冲区 + EPOLLOUT（正常路径，不打日志不置 faultError）；
//   EPIPE/ECONNRESET = “死了”，直接放弃这批数据（不打日志之外的任何补救）。
//   【⚠问题】置了 faultError 之后本函数也不做 disableAll / handleClose，连接状态还停在
//   kConnected：要靠随后 epoll 报 EPOLLHUP/EPOLLIN 读到 0 才会走 handleClose 回收。
//   如果 epoll 不报（某些对端半关场景），这条连接就一直挂着并“静默丢数据”。
//   本仓库可核实：这里只置 faultError，不做 disableAll/handleClose（原版对致命错误的注释原文以源码为准）。
                if (errno == EPIPE || errno == ECONNRESET) // SIGPIPE RESET
                {
                    faultError = true;
                }
            }
        }
    }
// 【设计】原注释已经把接力链条说清了，这里补上“谁调谁”的一句话索引：
//   enableWriting → epoll_ctl ADD/MOD EPOLLOUT → EPollPoller::poll 下轮返回该 channel
//   → Channel::handleEventWithGuard 看到 revents_ & EPOLLOUT → 构造函数里绑的
//   writeCallback_ → 本对象的 handleWrite → Buffer::writeFd 继续倒 outputBuffer_。
// 【易错】这条链成立的前提是 epoll 用**水平触发(LT)**：内核只要发送缓冲区有空就反复提醒。
//   若换成 ET，enableWriting 之后必须一次性写干净，否则剩余数据没人叫醒你。
    /**
     * 说明当前这一次write并没有把数据全部发送出去 剩余的数据需要保存到缓冲区当中
     * 然后给channel注册EPOLLOUT事件，Poller发现tcp的发送缓冲区有空间后会通知
     * 相应的sock->channel，调用channel对应注册的writeCallback_回调方法，
     * channel的writeCallback_实际上就是TcpConnection设置的handleWrite回调，
     * 把发送缓冲区outputBuffer_的内容全部发送完成
     **/
// 【流程】⑨ 的后半段：进缓冲区 + 开 EPOLLOUT（下一步就是 ⑩）。【线程】subLoop。
//   faultError 为真时整段跳过 —— 这就是“死连接不再堆内存”的实现。
    if (!faultError && remaining > 0)
    {
        // 目前发送缓冲区剩余的待发送的数据的长度
// 【设计】oldLen 必须在 append **之前**取，因为它代表“这次 send 之前的积压量”，
//   这是高水位“跨越判断”的左端点。取晚了（append 之后）就变成 oldLen>=hw 恒成立，
//   回调永远不触发或永远触发。单位：字节。
        size_t oldLen = outputBuffer_.readableBytes();
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 高水位三条件：为什么能做到“只在跨越阈值时通知一次”（边沿触发）              │
// └────────────────────────────────────────────────────────────────────────────┘
// 【设计】把三个条件拆开看（hw = highWaterMark_ = 默认 64MB）：
//   A. oldLen + remaining >= hw ：**新**积压量已经越过阈值（有资格报警）。
//   B. oldLen < hw               ：**旧**积压量还在阈值之下（说明这次是“第一次跨过去”）。
//   C. highWaterMarkCallback_    ：用户真的注册了（判空）。
//   A∧B 就是把“电平触发”改成“边沿触发”的标准写法：只在 false→true 的那一刻成立一次。
// 【流程】数值演算（hw=64MB，连续 send 10MB）：
//   第 1 次：oldLen=0,     remaining=10MB → 新值 10MB  ；A 假   → 不通知
//   第 6 次：oldLen=50MB,  remaining=10MB → 新值 60MB  ；A 假   → 不通知
//   第 7 次：oldLen=60MB,  remaining=10MB → 新值 70MB  ；A真B真 → 通知一次（参数 70MB）
//   第 8 次：oldLen=70MB,  remaining=10MB → 新值 80MB  ；A真但B假（70>=64）→ 不再通知
//   → 用户被提醒一次，应当立刻停止 send；等 handleWrite 排空触发 WriteCompleteCallback
//     后，oldLen 回到 0，下次再跨阈值才会再通知一次。
// 【易错】如果只写 A：积压一直在 64MB 以上时每 send 一次都通知 → 回调洪水；
//   如果只写 B：oldLen 恰好从 0 涨破阈值后，若某次 remaining 让新值仍 < hw 就不通知（正常），
//   但一旦积压稳定在阈值之上，B 恒假 → 永远不再提醒，也永远不通知第一次（取决于顺序）。
//   两个条件合起来才既是“第一次”又“确实跨过了”。
// 【设计】用 queueInLoop 而不是就地调，理由和 writeCompleteCallback_ 完全一样：
//   现在还在 sendInLoop 的栈上，用户在回调里 send 会重入本函数。
//   第二个实参是“跨越后的待写字节数”（不是增量），单位字节，方便用户打印/统计。
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_)
        {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
// 【语法】(char*)data + nwrote：把“已交给内核的前 nwrote 字节”跳过，只缓存剩下的。
//   nwrote==0（没写成 / 压根没尝试）时就是 data+0，全量缓存。
//   【易错】data 是 const void*，先强转 char* 才能做“按字节”的指针算术（void* 不能加减）；
//   这里丢掉 const 只是拿它当地址用，append 内部会再拷进 buffer，不会真改源内存。
//   【易错】append 之后 data 指向的原始内存就不再被引用 —— 这也是上面 send（本文件里紧邻本函数的那个）那条“悬垂指针”
//   问题必须靠“把内容拷进 functor”而不是“拷指针”解决的原因。
// 【设计】Buffer::append → ensureWritableBytes → makeSpace，可能 resize vector；
//   【⚠问题】本项目的 Buffer 只增不减（makeSpace 从不 shrink），所以一次 100MB 的大发送
//   会让 outputBuffer_ 的底层 vector 长期保持 100MB 不回落（长连接内存高水位常驻）。
        outputBuffer_.append((char *)data + nwrote, remaining);
// 【设计】为什么还要再判一次 !isWriting() 才 enableWriting：
//   ① 已经在写（EPOLLOUT 已注册）时重复 enableWriting = 多一次毫无意义的 epoll_ctl MOD
//      系统调用；② 更关键的语义：EPOLLOUT 已经在那儿等着了，handleWrite 会连着把
//      新 append 的数据一起倒出去，不需要重新“叫醒”。
// 【流程】这一步之后，本函数的活就干完了，接力棒交给 handleWrite（⑩）。
// 【易错】enableWriting → Channel::update → EventLoop::updateChannel → Poller::updateChannel
//   这一串都在改没有锁的容器，所以 sendInLoop 只能在 loop 线程跑（函数头那条规矩的落点）。
        if (!channel_->isWriting())
        {
            channel_->enableWriting(); // 这里一定要注册channel的写事件 否则poller不会给channel通知epollout
        }
    }
}

// 【流程】⑪ 的用户侧起点（主动关）。【线程】调用者线程；函数里只改 state_，动作投递给 subLoop。
// 【设计】“改状态在本地、做动作在 loop 线程”这种两步走，是为了让用户在任何线程调
//   shutdown 都不崩：状态是 atomic，写它安全；而真正的 shutdown(2) 系统调用和
//   epoll 状态读取必须回到 loop 线程。
// 【易错】只处理 kConnected：kDisconnecting 时再调是空操作（不会重复投递），
//   kDisconnected 时也什么都不做。
// 【易错】shutdown **不**关读端、**不**从 connections_ 里摘除、**不**立即析构。
//   要等对端也关闭 → 本端 read 返回 0 → handleClose → closeCallback_ 才走完回收。
//   如果对端一直不发 FIN，这条连接会带着 kDisconnecting 状态长期存在（原版 muduo 也是
//   这个行为，所以文档反复强调“shutdown 之后不要指望对象马上没了”）。
// 【对比原版】原版 muduo 的 shutdown() 第一行是 loop_->assertInLoopThread()，即要求
//   用户必须在 loop 线程调；本项目放宽成内部 runInLoop 分流，代价是状态改在调用者线程。
void TcpConnection::shutdown()
{
    if (state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(
            std::bind(&TcpConnection::shutdownInLoop, this));
    }
}

// 【流程】⑪ 的后半：真正把写端关掉（FIN）。【线程】subLoop。
// 【设计】“只在 !isWriting() 时才 shutdownWrite”的语义：isWriting 为真意味着
//   EPOLLOUT 还挂着、outputBuffer_ 还有没发完的数据。此刻 shutdown(SHUT_WR) 会把
//   发送队列里的数据直接丢掉（Linux 行为：已写入内核的会发完，但后续无法再补，
//   且未 flush 的用户态缓冲彻底没了）→ 数据丢失。所以要等排空。
// 【易错/⚠问题】如果 isWriting 为真，本函数什么都不做就返回 —— 关闭动作被“挂起”，
//   兜底在 handleWrite 末尾（那里 if (state_ == kDisconnecting) shutdownInLoop()）。
//   这条兜底路径能成立依赖一个不变式：“outputBuffer_ 非空 ⇒ isWriting 为真”
//   （见 sendInLoop 的 append 后立即 enableWriting）。于是：
//     buffer 有数据 → isWriting 真 → 等 handleWrite 排空 → 排空时 disableWriting → 同一段
//     代码里判 kDisconnecting → 补 shutdownInLoop（此时 isWriting 已假）→ 发 FIN。闭环。
//   如果有人绕过 sendInLoop 手工把 buffer 弄非空却不 enableWriting，这条链就断了，
//   连接永远关不掉（本项目没有这种调用路径，属于“理解不变式”的练习）。
// 【系统调用】Socket::shutdownWrite → shutdown(fd, SHUT_WR)：只关写方向，发 FIN；
//   读方向仍可继续收（所以还能读完对端剩余数据），直到对端也关读端后本端 read 返回 0。
//   与 close(fd) 的差别：close 两个方向一起关，且引用计数归零时若还有未读数据会发 RST。
void TcpConnection::shutdownInLoop()
{
    if (!channel_->isWriting()) // 说明当前outputBuffer_的数据全部向外发送完成
    {
        socket_->shutdownWrite();
    }
}

// 【流程】⑦ 的执行体。【线程】必须是 subLoop#N —— 由 TcpServer::newConnection 用
//   ioLoop->runInLoop 投递过来（【线程跳转 1】之后、由 doPendingFunctors 执行）。
// 连接建立
void TcpConnection::connectEstablished()
{
// 【设计】先改状态再注册事件：这样 enableReading 之后哪怕当场就有 EPOLLIN（LT 模式下
//   同一轮不会重入，但语义上更安全），connected() 也已经是 true，用户回调里 send 得通。
//   【易错】setState 顺序在 tie 之前：tie 需要 shared_from_this()，那时对象必须已在
//   connections_ 里（TcpServer::newConnection 里确实先 connections_[connName]=conn 再投递）。
    setState(kConnected);
// 【设计】Channel::tie 把 shared_from_this() 存进 Channel 的 std::weak_ptr<void> tie_，
//   并置 tied_=true。之后每次 Channel::handleEvent 都会先 tie_.lock()：
//   提升成功 → 临时强引用 guard 保对象活着跑完回调；失败 → 直接跳过所有回调。
//   解决的问题：“TcpServer 已经把这条连接从 connections_ 摘掉（最后一个强引用放手），
//   但同一轮 epoll 的 activeChannels_ 里还留着这个 Channel 指针” → 不 tie 就会调到
//   已析构对象的 handleRead。这就是原注释“Channel 的结束一定晚于 TcpConnection”的意思。
//   【语法】参数是 const std::shared_ptr<void>&：Channel 不认识 TcpConnection，
//   用 void 版 shared_ptr 做“类型擦除的生命周期锚”，weak_ptr<void> 与
//   weak_ptr<TcpConnection> 可互相转换，控制块是同一个。
// 【易错】tie 存的是 weak，不会造成循环引用（TcpConnection 强引用 Channel，
//   Channel 只 weak 引用 TcpConnection）；如果把 tie_ 写成 shared_ptr 就真成环，永远不析构。
    channel_->tie(shared_from_this());
// 【流程】⑦ 的关键动作：第一次把 connfd 的 EPOLLIN 注册进 epoll（epoll_ctl ADD）。
//   【线程】只能在 subLoop：这条规矩就是整个函数必须由 runInLoop 投递的根本原因 ——
//   Poller 的 channels_ 映射表和 epoll 实例都不是线程安全的。
//   【对比原版】原版 muduo 的 TcpConnection::connectEstablished 顺序与本文件完全一致
//   （setState → tie → enableReading → connectionCallback），并多一句 assertInLoopThread。
    channel_->enableReading(); // 向poller注册channel的EPOLLIN读事件

    // 新连接建立 执行回调
// 【流程】⑦ 的终点：通知用户“上线”，参数 shared_from_this() 让用户的 functor 里也能
//   安全持有这个连接。
// 【易错】调用点不判空：用户没 setConnectionCallback 就抛 std::bad_function_call（见头文件说明）。
// 【设计】把它放在最后一行，保证“用户回调里立刻调 conn->send()/shutdown()”时
//   事件已经注册完毕、状态已经稳定（用户看不到半初始化的连接）。
    connectionCallback_(shared_from_this());
}
// 【流程】⑪ 的收口（回收动作），由 TcpServer::removeConnectionInLoop 用
//   ioLoop->queueInLoop 投递（【线程跳转 3】），也由 ~TcpServer 用 runInLoop 投递。
//   【线程】subLoop#N。
// 连接销毁
void TcpConnection::connectDestroyed()
{
// 【设计】判 kConnected 才通知下线：handleClose 已经把 state 置成 kDisconnected 并
//   通知过一次用户的连接，这里再走一遍（同一对象）不能把“下线”打印第二遍。
//   而从 ~TcpServer 这条路进来的连接状态仍是 kConnected（进程退出，不是对端断），
//   于是会正确通知一次下线 —— 这一句 if 就是这两种场景的分水岭。
    if (state_ == kConnected)
    {
        setState(kDisconnected);
// 【设计】disableAll 只把 events_ 清成 kNoneEvent 并 update（epoll_ctl MOD/DEL 语义），
//   它**不**删除 Poller 里的 Channel 记录，也不 close fd —— 所以外面还要 remove。
//   【易错】disableAll 后如果本轮 epoll_wait 已经把该 channel 放进了 activeChannels_，
//   事件仍会被分发一次（快照效应），这正是 Channel::tie 存在的理由。
        channel_->disableAll(); // 把channel的所有感兴趣的事件从poller中删除掉
        connectionCallback_(shared_from_this());
    }
// 【设计】channel_->remove() → EventLoop::removeChannel → Poller::removeChannel：
//   epoll_ctl DEL + 把 Channel 从 Poller 的映射表里摘掉，index_ 置 kRemoved。
//   【易错】“先 disableAll 再 remove”是为了让状态迁移可被日志观察；功能上只 remove 也够
//   （原版 muduo 里只有 kConnected 分支执行 disableAll，然后无条件 remove，一样思路）。
//   【流程】走到这里，这条连接在 epoll 里彻底消失；functor 执行完、bind 里那份 shared_ptr
//   析构 → 引用计数归零 → ~TcpConnection（同线程）→ ~Socket 才 close(connfd)。
    channel_->remove(); // 把channel从poller中删除掉
}

// 【流程】⑧ 的入口注释（原注释）。把它翻译成完整链路：
//   客户端 send → 内核收包进该 socket 的接收缓冲区 → subLoop 的 epoll_wait 返回 EPOLLIN
//   → Channel::handleEventWithGuard 命中 revents_ & EPOLLIN → readCallback_（构造里绑的 bind）
//   → 本函数 → Buffer::readFd(readv) → messageCallback_（用户的 onMessage）。
//   【易错】“读”永远是相对服务器说的：客户端发来的数据 = 服务器的可读事件。
// 读是相对服务器而言的 当对端客户端有数据到达 服务器端检测到EPOLLIN 就会触发该fd上的回调 handleRead取读走对端发来的数据
// 【线程】subLoop#N（该连接所属 IO 线程）。【流程】⑧。
void TcpConnection::handleRead(Timestamp receiveTime)
{
// 【设计】savedErrno 先置 0：万一 readFd 内部因为异常路径没写它，0 表示“没错误”，
//   下面的 errno = savedErrno 就不会把无关的旧 errno 塞回去。
//   【易错】这是本项目里唯一把 errno “另存”的读路径：因为 LOG_ERROR / 用户回调内部
//   都会调 malloc/printf/write，跑一次就可能改掉 errno，必须第一时间存下来。
    int savedErrno = 0;
// 【系统调用】Buffer::readFd 用 readv 一次最多读两段（iovec）：先写进 Buffer 的可写区，
//   空间不够时第二段落在栈上 64KB 的 extrabuf 再 append 回来 —— 好处是一次系统调用
//   就能“读满 + 扩容”，避免两次 read 的往返。细节见 src/Buffer.cc。
//   【易错】本函数只 readv **一次**，不循环。LT 模式下单次没读完没关系：
//   内核接收缓冲区还有数据 → 下一轮 epoll_wait 立刻再报 EPOLLIN。ET 就必须 while 读到 EAGAIN。
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
// 【流程】⑧ 的正常分支。n>0 表示读到了字节（不保证是“一个包/一行”，TCP 是字节流）。
    if (n > 0) // 有数据到达
    {
        // 已建立连接的用户有可读事件发生了 调用用户传入的回调操作onMessage shared_from_this就是获取了TcpConnection的智能指针
// 【设计】三个实参的取法：shared_from_this() 给连接（+1 强引用，回调期间对象必然活着）、
//   &inputBuffer_ 给缓冲区（裸指针，不拥有）、receiveTime 给时间戳（Channel::handleEvent
//   一路传下来的 pollReturnTime_）。
//   【易错】调用点不判空 messageCallback_；另外用户如果不在回调里 retrieve，
//   inputBuffer_ 会一直涨（它只由 retrieve 系列推进 readerIndex）。
// 【⚠问题】这里没有 try/catch（本仓库可核实：全库 src/*.cc 没有任何 catch 子句）：用户 onMessage 抛异常
//   → 一路冒到 doPendingFunctors 之外 → subLoop 线程 std::terminate。原版是否 catch 以原版源码为准。
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
// 【流程】⑪ 的第一种入口：对端正常关闭（发过 FIN），内核读完数据后 read 返回 0。
    else if (n == 0) // 客户端断开
    {
// 【设计】直接调 handleClose（同线程同栈帧），而不是“注册一个任务稍后关”。
//   此时本函数的栈上还留着 n/savedErrno 等局部量，没有对象状态需要保护，所以安全。
//   【易错】handleClose 里会调 closeCallback_ → TcpServer::removeConnection，一路可能
//   让本对象析构；返回时又回到 handleRead 的栈帧 —— 这条“调用栈叠了三层析构路径”
//   之所以安全，全靠 handleClose 把 closeCallback_ 放在最后一行、而 handleRead 在
//   调用它之后什么都不做（只有 return）。任何在这两行之后新增“碰 this 成员”的代码都是雷。
        handleClose();
    }
// 【流程】错误分支（n<0）。
    else // 出错了
    {
// 【设计】把 savedErrno 回填进 errno：让紧随其后的 LOG_ERROR（内部可能改 errno）
//   和 handleError（要读 errno/SO_ERROR）看到“真正的失败原因”。顺序不能反。
//   【易错】EWOULDBLOCK/EAGAIN 在这里“不该出现”：EPOLLIN 已经说明有数据可读；
//   真出现通常是并发 read（本库没有）或 EPOLLHUP 与 EPOLLIN 同时到。EINTR 也可能：
//   被信号打断，本项目不重试，直接走 handleError 打日志（LT 下下一轮还会再来，无数据丢失）。
// 【对比原版】本仓库可核实：这里对 -1 不区分 EINTR/EAGAIN、也不重试，统一 LOG_ERROR 之后交给 handleError。
//   原版 muduo 是否在 read 路径上区分 EINTR 并重试，以原版源码为准。
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead");
// 【流程】错误处理：只打日志，不改状态、不关连接（见 handleError）。断开要靠同批或
//   下一批的 EPOLLHUP / read==0 触发 handleClose。
        handleError();
    }
}

// 【线程】subLoop#N。【流程】⑩ 的执行体：EPOLLOUT 到达后继续倒 outputBuffer_。
// 【设计】它是 sendInLoop 那条“剩余数据交给 EPOLLOUT”承诺的兑现者；两者合起来
//   才是 muduo 完整的发送状态机。
void TcpConnection::handleWrite()
{
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 为什么 handleWrite 开头必须判 isWriting（这是 muduo 最出名的一处“防御”）    │
// └────────────────────────────────────────────────────────────────────────────┘
// 【设计】EPOLLOUT 是水平触发：只要发送缓冲区有空就报。我们 disableWriting 之后
//   events_ 里已经没有 EPOLLOUT，但**已经取出来的事件是快照**：
//   Poller::poll 一次性把本轮所有就绪 channel 填进 activeChannels_（并 set_revents），
//   然后 EventLoop::loop 逐个调 handleEvent。排在前面那个 channel 的回调里完全可能
//   触发本 channel 的 disableWriting（例如 onMessage 里 send 一次就发完，或业务
//   在这轮里主动关连接）。于是本 channel 的 handleEvent 仍然会带着“revents_ 里的
//   EPOLLOUT”被调用 —— 事件已经过期。
// 【易错】过期不判的后果：对已经不再关心的 fd 再 write 一次本身不会坏，但
//   ① 逻辑上“我们已经不在写了”却跑进了写路径，outputBuffer_ 空时 writeFd(fd,0)
//   会让 handleWrite 走奇怪的分支；② 更严重的是若这条连接已经 handleClose/摘除，
//   写一个可能已被 close 复用的 fd 就是串号。“注册写事件前先确认没在写”是 LT 模型的通识
//   （原版的注释原文以源码为准，本环境未能核对）。本仓库可核实：Channel::enableWriting 没有重复注册检查，
//   重复调用只多一次 epoll_ctl MOD 系统调用。
// 【设计】else 分支只打日志（fd=%d is down, no more writing），不做任何状态修改 —— 这是
//   “陈旧事件”的正常归宿，不该当错误处理（虽然用的是 LOG_ERROR，级别偏【⚠问题】：
//   高频连接上这条日志会出现得很频繁（原版用什么级别以源码为准；本仓库可核实：这里是 LOG_ERROR，而本项目
//   的 LOG_DEBUG 在未定义 MUDEBUG 时是空宏，且三处 CMakeLists.txt 都没有定义 MUDEBUG）。
    if (channel_->isWriting())
    {
// 【设计】同 handleRead：先给 savedErrno 置 0，再由 writeFd 填真实错误码。
        int savedErrno = 0;
// 【系统调用】Buffer::writeFd 内部用 writev 把 outputBuffer_ 的“当前可读段”一次发出
//   （本项目实现见 src/Buffer.cc）。返回 n 可能小于可读长度（短写），也可能 -1/EAGAIN。
//   【易错】它**只从 readerIndex 处开始写**，写完必须由调用方 retrieve(n) 推进，
//   否则下一次 writeFd 会把同一批字节再发一遍（重复发送）。这就是下面那句 retrieve 的意义。
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
        if (n > 0)
        {
// 【设计】retrieve(n)：把 readerIndex 前移 n（等价于“这批字节已经离开本机”）。
//   顺序很关键 —— 先写成功、再推进读指针；反过来写失败就丢数据。
//   【语法】Buffer::retrieve 在 len>=readableBytes() 时会走 retrieveAll()，把两个
//   下标一起复位到 kCheapPrepend，于是缓冲区“看起来空了”，但 vector 容量不缩。
            outputBuffer_.retrieve(n);//从缓冲区读取reable区域的数据移动readindex下标
// 【流程】⑩ 的收口判断：缓冲区彻底排空才做后面三件事。还有剩余就什么都不做，
//   等下一次 EPOLLOUT（LT 会再报）继续。
            if (outputBuffer_.readableBytes() == 0)
            {
// 【设计】disableWriting 是**必须**的，不是“优化”：
//   EPOLLOUT 水平触发下，只要 fd 注册了 EPOLLOUT 而发送缓冲区有空（数据发完了必然空），
//   epoll_wait 就会**每次都立刻返回这个 fd** → loop 变成忙轮询，CPU 100%，
//   而且每轮都会跑一次 handleWrite 打“no more writing”之类的日志。
//   muduo 的规矩：EPOLLOUT 只在“有数据要发”时短暂注册，发完立刻摘掉（对比 EPOLLIN
//   是全程注册的）。
//   【易错】disableWriting 只清 EPOLLOUT 位，EPOLLIN 仍在（events_ &= ~kWriteEvent），
//   所以关掉写事件后这条连接依然能收数据 —— 不要以为 disableWriting == 断开。
                channel_->disableWriting();
// 【流程】⑩ 的“发完通知”（和 sendInLoop 快路径那处是同一个回调的两个触发点）。
                if (writeCompleteCallback_)
                {
// 【线程】subLoop；注意它是“排进 pendingFunctors_”，所以真正跑在 doPendingFunctors 里，
//   不在 Channel::handleEvent 的栈上 —— 用户的 writeComplete 回调里可以放心再 send。
                    // TcpConnection对象在其所在的subloop中 向pendingFunctors_中加入回调
                    loop_->queueInLoop(
                        std::bind(writeCompleteCallback_, shared_from_this()));
                }
// 【流程】⑪ 的兜底路径（那条“shutdown 只在 !isWriting 时关写端，靠这里补一次”）：
//   时间线：用户 shutdown() → state_=kDisconnecting → shutdownInLoop 发现 isWriting 为真
//   → 什么都不做返回 → 数据继续由 EPOLLOUT 排出 → 排空 → disableWriting（isWriting 变假）
//   → 紧接着这个 if 命中 kDisconnecting → 补一次 shutdownInLoop → 这次 !isWriting 成立
//   → shutdown(fd, SHUT_WR) 发 FIN。
//   【易错】这条链要求 disableWriting 与这段判断在同一个 if(readableBytes()==0) 里、
//   并且顺序是先 disable 后判状态 —— 反过来写会当场让 isWriting 仍为真，兜底失效。
//   【设计】只有“排空”这一刻才判断，所以如果 outputBuffer_ 一直排不空，FIN 就一直不发 ——
//   对端会看到“连接还活着但收不到数据”，这正是“优雅关闭”的代价（要超时得自己加定时器）。
                if (state_ == kDisconnecting)
                {
                    shutdownInLoop(); // 在当前所属的loop中把TcpConnection删除掉
                }
            }
        }
// 【流程】写失败分支。
// 【设计】注意这里**没有** retrieve，也没有 disableWriting：数据原地留在 outputBuffer_，
//   EPOLLOUT 仍然注册着，下一轮再来试。对 EAGAIN 完全正确（LT 下很快就有空间）。
//   【⚠问题】对 EPIPE/ECONNRESET 也是同样处理 → 永远重试、永远失败，
//   每轮 epoll_wait 都报 EPOLLOUT（对已断连接它通常恒就绪）→ 打日志刷屏 + 内存不释放。
//   正规做法是判断致命错误后走 disableAll + handleClose（或至少把 buffer 丢掉）。
// 【对比原版】本仓库可核实：这里同样只记日志、不清 buffer、不关连接；断开要靠 Channel 层的
//   EPOLLHUP/EPOLLERR 分发来收尾（见 handleClose 的两个入口）。原版 muduo 的具体处理以原版源码为准；
//   所以要理解成“错误分支不负责关闭，关闭是 epoll 事件驱动的”。
        else
        {
            LOG_ERROR("TcpConnection::handleWrite");
        }
    }
    else
    {
// 【流程】陈旧 EPOLLOUT 的归宿（见函数开头那个大段说明）。fd 数字打进日志便于确认是哪条连接。
        LOG_ERROR("TcpConnection fd=%d is down, no more writing", channel_->fd());
    }
}

// 【线程】subLoop#N。【流程】⑪ 的核心现场。
// 【设计】handleClose 与 shutdown 的区别，面试高频，一张表说清：
//   ┌────────────┬───────────────────────────┬───────────────────────────┐
//   │            │ shutdown（用户主动）       │ handleClose（被动发现对端断）│
//   ├────────────┼───────────────────────────┼───────────────────────────┤
//   │ 触发者      │ 业务代码 conn->shutdown()  │ ① handleRead 里 read==0    │
//   │            │                            │ ② Channel 见 EPOLLHUP(无EPOLLIN)│
//   │ 状态迁移    │ kConnected→kDisconnecting  │ *→kDisconnected            │
//   │ 动作       │ 只关**写端**(SHUT_WR, FIN) │ 关掉所有事件 disableAll     │
//   │ 读端       │ 仍可读（还能收对端余下数据）│ 不再关心                    │
//   │ 通知下线    │ 不通知（还没断完）          │ 立即通知 connectionCallback_ │
//   │ 回收       │ 等对端也关→read==0→handleClose│ 立即 closeCallback_ 回收  │
//   └────────────┴───────────────────────────┴───────────────────────────┘
//   【易错】“优雅关闭”必须是先 shutdown 再等 handleClose，直接 close(fd) 会把未 ACK 的
//   数据丢掉并给对端回 RST；muduo 全程不主动 close，只在对象析构时由 ~Socket close。
void TcpConnection::handleClose()
{
// 【流程】⑪ 的第一条日志。它在连接所属的 subLoop 里执行；紧随其后的
//   TcpServer::removeConnectionInLoop 在 mainLoop 里执行 —— 这正是一次线程跳转。
// 【易错】但**不能靠肉眼看日志来确认线程**：src/Logger.cc 的格式是 [级别]+秒级时间+msg，
//   没有任何 tid/线程名字段（EventLoop 构造那句带 tid 的日志是 LOG_DEBUG，默认是空宏）。
//   日志能给你的只有**先后顺序**；要确认线程请用 gdb 的 info threads（docs/experiments.md 第 3 节）。
    LOG_INFO("TcpConnection::handleClose fd=%d state=%d\n", channel_->fd(), (int)state_);
// 【设计】先改状态再通知：这样用户的 onConnection 里 conn->connected() 立刻变 false，
//   能正确打印“Connection DOWN”（testserver.cc 就是这么写的）。
//   【易错】state_ 也顺带让后续 send 变成“无效但不报错”（见 send 的静默丢弃说明）。
    setState(kDisconnected);
// 【设计】disableAll 把 events_ 置 0 并 update → Poller 里这个 fd 变成 kNoneEvent，
//   不再收任何事件。此刻还没从 epoll 删除，删除在 connectDestroyed 里做。
//   【易错】不 close fd！fd 的关闭要等对象析构（~Socket），也就是 connections_.erase
//   + connectDestroyed 之后引用计数归零的那一刻。
    channel_->disableAll();

// 【设计】先做一次**显式拷贝**成具名局部量 TcpConnectionPtr connPtr，而不是在两行里
//   分别写 shared_from_this()：
//   ① 少一次原子加减（两次 shared_from_this 就是两次 lock 前缀指令）；
//   ② 关键 —— 这个局部强引用保证“无论两个回调里发生什么（甚至把 connections_ 里的
//      那份 erase 掉了），对象都活到本函数返回”。这叫 self-deletion 防护。
//   【易错】若写成 connectionCallback_(shared_from_this()); closeCallback_(shared_from_this());
//   功能也对，但 shared_from_this 在计数已 0 时是 UB 风险区（本项目不会 0，因为
//   调用栈上必有 shared_ptr；写清原理即可）：正确姿势就是本行这种“取一份具名副本”。
    TcpConnectionPtr connPtr(shared_from_this());
// 【流程】⑪ 下线通知：同一个 connectionCallback_，靠 connected()==false 区分（见 Callbacks.h）。
//   【易错】用户在这个回调里**不能**调 conn->closeCallback 相关的事、也不要长时间阻塞；
//   它可以安全调 conn->getLoop()->queueInLoop 投递清理任务。
    connectionCallback_(connPtr); // 连接回调
// 【流程】⑪【线程跳转 2】的起点：closeCallback_ 是 TcpServer::removeConnection（绑了 this）。
// 【易错】行尾那句 “must be the last line” 是本仓库自带的提醒（在 src/TcpConnection.cc 里可直接看到），也是硬约束：这行可能间接
//   导致本对象在本函数返回前就析构（TcpServer 的强引用被 erase，若此时又刚好没有别的
//   shared_ptr 存活……本项目里至少还有 connPtr 这个局部强引用，所以真正析构发生在
//   handleClose 返回时）。也就是说：正因为有上面 TcpConnectionPtr connPtr 那份副本，这行才敢放在最后 ——
//   两个技巧是配套的，缺一个都不安全。
//   【设计】它一定不是用户设置的：只有 TcpServer::newConnection 会设（回收钩子）。
    closeCallback_(connPtr);      // 执行关闭连接的回调 执行的是TcpServer::removeConnection回调方法   // must be the last line
}

// 【流程】EPOLLERR 的归宿。【线程】subLoop#N。
void TcpConnection::handleError()
{
// 【系统调用】getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optlen)：读取“该 socket 上
//   最近一次异步错误”并**清空**它。为什么需要它：connect/异步操作失败不会让
//   epoll 给出错误码，只报 EPOLLERR/EPOLLOUT，具体原因（ECONNREFUSED/ETIMEDOUT/ENETUNREACH）
//   只能靠 SO_ERROR 问内核。
// 【语法】optlen 必须**先赋 sizeof**再传地址：它是 in/out 参数，未初始化直接传进去，
//   内核会看到“缓冲区大小为 0/垃圾值”，可能返回 EINVAL 或截断（本项目这里正确初始化了）。
//   sizeof optval 不加括号是合法的（一元运算符作用于类型时要写 sizeof(类型)，
//   作用于变量时括号可省）。
// 【易错】SO_ERROR 是“上一次错误”，可能是很早之前留下的；日志里的值要当线索而不是结论。
// 【⚠问题】本项目 handleError 只打日志：既不关闭连接也不通知用户，也不清 buffer。
//   真实效果是“错误被记录，断开靠后续 EPOLLHUP/read==0 触发 handleClose”。
//   如果内核只报 EPOLLERR 而不再报别的（少数场景），这条连接会卡在 kConnected 泄漏下去。
// 【对比原版】原版 muduo 的错误处理注释原文以源码为准（本环境未能核对）；
//   可核实的通用结论是：EPOLLERR 场景的收尾确实要靠 EPOLLHUP/EPOLLIN（读回 0）这两条路。
//   原版如何保证线程语义与对象存活，以原版源码为准。
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
// 【设计】getsockopt 本身失败（fd 已无效等）才退回 errno，否则用内核给的 optval。
//   两条路合成一个 err 只为打印，不参与逻辑判断。
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name:%s - SO_ERROR:%d\n", name_.c_str(), err);
}

// 【设计】sendFile/sendFileInLoop 属于“教学新增”接口（本仓库可核实：全库连 example 都没有任何调用点，
//   只有 include/TcpConnection.h 里的声明与本文件的实现）。目的是演示零拷贝。它和 send 是
//     send → outputBuffer_ → write/writev（数据必须先在用户内存里）
//     sendFile → sendfile（数据留在内核页缓存，直接进 socket 发送缓冲区）
//   【易错】正因为是两条独立路径，同一个连接上混用 send 与 sendFile 可能让字节乱序：
//   sendFileInLoop 只在“outputBuffer_ 空且不在写”时才真发，否则它不排队等待，
//   而是把自己重新 queueInLoop 再试（见下面的【⚠问题】）。要发送“文件 + 协议头”这类
//   混合内容，正确做法是 writev + iovec（一段用户内存 + 一段文件）一次提交 —— 这是内核文档推荐的做法
//   （原版 muduo 示例怎么组织，以原版源码为准）。
// 新增的零拷贝发送函数
// 【线程】和 send 一样做 isInLoopThread 分流。【流程】⑨ 的变种。
//   【语法】注意这里 bind 的第二个实参是 **shared_from_this()** 而不是 this：
//   成员函数指针 &TcpConnection::sendFileInLoop 的第一个隐含形参是对象，
//   std::bind 允许用 shared_ptr<TcpConnection> 充当它（调用时通过 operator-> 解引用），
//   而且 bind 会**按值保存这份 shared_ptr** → 引用计数 +1 → 对象保证活到 functor 执行完。
//   【对比】这与 send 里传 this（不保寿命）形成鲜明对比，是本项目内部不一致的地方：
//   sendFile 这条路更安全（对象不会在投递后消失），send 那条路更危险（this 可能失效）。
//   【面试】“muduo 怎么保证跨线程回调时对象还活着？”→ 三种手段：bind 里按值捕获
//   shared_ptr、Channel::tie 的 weak_ptr、调用点上的具名局部副本（handleClose 里的 connPtr）。
//   【易错】这里判的是 connected()（等价 state_==kConnected），比 send 更严：
//   kDisconnecting 时 sendFile 直接走 LOG_ERROR 分支，不投递。
void TcpConnection::sendFile(int fileDescriptor, off_t offset, size_t count) {
    if (connected()) {
        if (loop_->isInLoopThread()) { // 判断当前线程是否是loop循环的线程
            sendFileInLoop(fileDescriptor, offset, count);
        }else{ // 如果不是，则唤醒运行这个TcpConnection的线程执行Loop循环
            loop_->runInLoop(
                std::bind(&TcpConnection::sendFileInLoop, shared_from_this(), fileDescriptor, offset, count));
        }
    } else {
        LOG_ERROR("TcpConnection::sendFile - not connected");
    }
}

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ sendFileInLoop：零拷贝 + 一条真实的忙轮询缺陷                              │
// └────────────────────────────────────────────────────────────────────────────┘
// 【系统调用】ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
//   返回：本次写入 out_fd 的字节数（可能短写）；0 表示 count 为 0 或对端已关；-1 出错。
//   offset 是 **in/out 参数**：非 NULL 时它是“从 in_fd 的哪个字节开始读”，
//   调用成功后内核把它推进（等价于 lseek 效果，但**不动 in_fd 的文件偏移**，
//   所以多个线程共享同一个 fd 也不互相干扰 —— 传 NULL 才会用并推进 in_fd 自己的偏移）。
//   为什么快（内核里发生了什么）：
//     普通发送：read(fd,file_buf)  ← 内核把页缓存拷到用户空间（拷贝1，进用户态）
//               write(sockfd,file_buf) ← 再从用户空间拷到 socket 缓冲（拷贝2，再进内核）
//               = 4 次上下文切换 + 2 次数据拷贝
//     sendfile：文件数据留在**页缓存**，内核只把“页引用/scatter-gather 描述符”挂到
//               socket 发送缓冲区，网卡 DMA 直接从页缓存取走
//               = 2 次上下文切换 + 0 次（支持 SG-DMA 的网卡）或 1 次（老内核拷到 socket 缓冲）拷贝
//   限制（必须记住的四条）：
//   ① out_fd 必须是**流式 socket**（Linux 特定扩展，不可移植）；
//   ② in_fd 必须是普通文件（不支持 socket/pipe；2.6.33 之前不能是 pipe）；
//   ③ 需要“修改数据再发”的场景一律不能用：TLS/SSL（要加密）、压缩、加协议头 ——
//      那些变换必须在用户空间做，一做就破坏了零拷贝的前提；
//   ④ 非阻塞 out_fd 会短写并返回 EAGAIN，所以仍然需要“续传”状态机。
// 【⚠问题】本函数的续传状态机是错的（函数末尾 if (!faultError && remaining > 0) 那段详述）：正确写法是把“还有文件要发”做成
//   连接的状态（例如 file_offset_/file_fd_ 成员），在 !isWriting() 时 enableWriting()，
//   让 EPOLLOUT（LT 水平触发，缓冲区一有空就报）来叫醒下一次 sendfile —— 也就是
//   和 sendInLoop/handleWrite 完全同构的路子；
//   现在的写法既不注册 EPOLLOUT，也不等内核信号，而是把自己塞回 pendingFunctors_ 立刻再跑，
//   等于“忙轮询(wait/busy loop)”。
// 【易错】sendfile 不会自动处理 Nagle/Cork：小文件可能一批碎包出去，需要时自己
//   设 TCP_CORK；另外它在 out_fd 上写入时同样有 SIGPIPE 风险（本库未忽略该信号）。
// 在事件循环中执行sendfile
// 【流程】⑨ 的零拷贝变种（不经 outputBuffer_，直接 sendfile 到 socket）。
// 【线程】只允许在 loop_ 所属线程（subLoop#N）执行，由 sendFile 的分流保证。
void TcpConnection::sendFileInLoop(int fileDescriptor, off_t offset, size_t count) {
// 【设计】bytesSent 用 ssize_t（可 -1），remaining 用 size_t：注意 remaining 只会在
//   bytesSent>=0 时被减，无符号减法不会下溢（bytesSent<=remaining 由内核保证，见 remaining -= bytesSent 那句）。
    ssize_t bytesSent = 0; // 发送了多少字节数
    size_t remaining = count; // 还要多少数据要发送
    bool faultError = false; // 错误的标志位

// 【⚠问题】这里判的是 kDisconnecting，**没有**判 kDisconnected，和 sendInLoop（判
//   kDisconnected）恰好相反，两处都不完整：
//   断开的合法状态有两个 —— kDisconnecting（正在优雅关）与 kDisconnected（已断）。
//   结果：kDisconnected 时会继续往下 sendfile（写一个可能已关/已复用的 fd）。
//   【易错】好消息是这里**有** return（sendInLoop 里 state_==kDisconnected 缺 return 的毛病这里没有），
//   所以 kDisconnecting 的路径是正确的。读代码时别把两处当成同一个逻辑。
    if (state_ == kDisconnecting) { // 表示此时连接已经断开就不需要发送数据了
        LOG_ERROR("disconnected, give up writing");
        return;
    }

    // 表示Channel第一次开始写数据或者outputBuffer缓冲区中没有数据
// 【设计】条件和 sendInLoop 完全一致（同一个道理：不能在 buffer 有积压时插队发文件）。
//   【易错】但“不满足条件时”本函数的处理方式和 sendInLoop 不同：sendInLoop 会 append
//   进 buffer 交给 EPOLLOUT，本函数直接掉进自己函数末尾那个自我重排分支 —— 于是只要 outputBuffer_
//   非空或正在写，就会一直空转，直到对方排空为止（白烧 CPU）。
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
// 【系统调用】注意 &offset：把“推进文件读位置”的工作交给内核，避免自己维护偏移量。
//   这也是函数末尾那次 queueInLoop(std::bind(&TcpConnection::sendFileInLoop, ...)) 续传链能成立的原因。
//   【易错】count 单位字节；超过文件大小不会报错，读到 EOF 就返回短写/0。
        bytesSent = sendfile(socket_->fd(), fileDescriptor, &offset, remaining);
        if (bytesSent >= 0) {
// 【设计】remaining -= bytesSent：bytesSent 是“已经交给 socket 的字节”，减掉即剩余。
//   【易错】bytesSent==0 且 count>0（对端关闭或 EOF）时 remaining 不变，
//   下面就会立刻把自己重排 → 死循环空转。这是函数末尾自我重排那条缺陷里最凶的一种情形。
            remaining -= bytesSent;
// 【流程】文件全发完 + 用户有回调 → queueInLoop 通知（和 sendInLoop 同一手法：
//   排到本轮之后再执行，避免在 sendFileInLoop 的栈上重入 send/sendFile）。
//   【易错】这里的“发完”只表示进了内核，不代表对端收到。
            if (remaining == 0 && writeCompleteCallback_) {
                // remaining为0意味着数据正好全部发送完，就不需要给其设置写事件的监听。
                loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
            }
// 【流程】失败分支。
        } else { // bytesSent < 0
// 【设计】EWOULDBLOCK 单独放过（非阻塞 socket 此刻塞不下，属正常），其余 errno 打日志。
//   【易错】注意与 sendInLoop 的结构差异：那边“EPIPE/ECONNRESET → faultError”是
//   **嵌在** errno != EWOULDBLOCK 里面的，这里是**并列**的两个 if。功能上等价
//   （EWOULDBLOCK 与 EPIPE 不可能同时成立），但读代码时容易看错，属于风格不一致。
//   另外这里没有“失败就不重排”以外的补救：faultError 只用于让函数末尾不重排。
            if (errno != EWOULDBLOCK) { // 如果是非阻塞没有数据返回错误这个是正常显现等同于EAGAIN，否则就异常情况
                LOG_ERROR("TcpConnection::sendFileInLoop");
            }
// 【易错】EPIPE/ECONNRESET → faultError=true → 下面不重排，本函数的数据就地放弃。
//   【⚠问题】和 sendInLoop 一样：不 disableAll、不 handleClose、不通知用户 —— 死连接
//   仍停在原状态，等 epoll 的 EPOLLHUP 来收尾。
//   【设计】fileDescriptor 是**调用者的 fd**（用户自己 open 的），本函数不会 close 它 ——
//   谁打开谁关闭，这条规矩要在接口注释里对用户讲明（否则每发一个文件漏一个 fd）。
            if (errno == EPIPE || errno == ECONNRESET) {
                faultError = true;
            }
        }
    }
// 【流程】剩余数据的处理 —— 也就是本函数最需要批判的一段。
    // 处理剩余数据
// 【⚠问题】忙轮询烧 CPU 的确切原因（三个叠加）：
//   ① queueInLoop 把 sendFileInLoop 排进 pendingFunctors_，本轮 doPendingFunctors
//      结束时就会执行它（不需要等任何 IO 事件），所以“发 → EAGAIN → 立刻再发 → EAGAIN…”
//      在同一个 loop 迭代里就能循环几十万次；
//   ② 每次 queueInLoop 还会写一次 wakeupFd_（eventfd）→ 额外系统调用 + 让 epoll_wait
//      一直带着就绪的 eventfd 立刻返回，整个 subLoop 变成 100% CPU 空转；
//   ③ 它没有给自己“注册 EPOLLOUT”，所以完全绕开了 epoll 的节奏控制（LT 本该做的
//      “有空才叫醒”）。
//   正确做法（与 sendInLoop/handleWrite 同构）：
//     · 把 (fileDescriptor, offset, remaining) 存成连接成员，作为“文件待发状态”；
//     · 若 !channel_->isWriting() 就 channel_->enableWriting()，然后 return；
//     · 在 handleWrite（或一个专门的 writeFileCallback）里看到“还有文件要发”时继续 sendfile；
//     · 发完再 disableWriting()。
//   这样等待的是内核“发送缓冲区有空”的信号，而不是自己追着轮询。
// 【对比原版】本仓库可核实：sendFile/sendFileInLoop 在库里没有任何调用点，属于教学新增件；
//   原版 muduo 是否提供同类接口、其示例如何做零拷贝发送，以原版源码为准。
    if (!faultError && remaining > 0) {
        // 继续发送剩余数据
// 【语法/设计】offset 的接力链（规范点名的那条）：
//   sendfile(..., &offset, ...) 在成功时**就地推进**这个局部变量（值传递的形参），
//   于是本函数栈上的 offset 已经指向“下一个要发的字节”；
//   下面 bind 把**推进后的 offset 值**按值存进 functor，
//   下一次 sendFileInLoop 收到它作为形参，再交给 sendfile 的 &offset —— 续传不断点。
//   【易错】三个细节：① 若这条链里有人在推进之前就把 offset 用了（打印/比较）会得到旧值；
//   ② 因为是“按值接力”，即使中间隔着 loop 线程的排队也不会读到脏内存（比传指针安全）；
//   ③ 若 bytesSent 为 0（EOF/短写为 0），offset 不变 → 下次从同一位置重发，配合 remaining -= bytesSent 那句的
//   remaining 不减就是彻底的死循环。
//   【设计】count 传 remaining 而不是原 count，保证“只发还没发出去的部分”。
//   【语法】bind 的第二实参是 shared_from_this()：文件描述符 fileDescriptor 是个裸 int，
//   按值捕获，functor 里用的是同一个 fd —— 如果用户在发送途中 close 了它，
//   这里就会 sendfile 一个无效 fd（EBADF），这也是“接口把 fd 交给异步任务”的通病。
        loop_->queueInLoop(
            std::bind(&TcpConnection::sendFileInLoop, shared_from_this(), fileDescriptor, offset, remaining));
    }
}