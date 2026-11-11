/* ============================================================================
 * 【文件】src/Acceptor.cc
 * 【一句话】造一个非阻塞 listenfd 并绑到指定地址，注册进 mainLoop 的 epoll；
 *          它一可读就 accept4 出 connfd，把 (connfd, 对端地址) 交给上层回调。
 * 【Reactor 角色】Acceptor（新连接接收器，只活在 mainLoop）
 * 【依赖】Acceptor.h；Logger.h（LOG_FATAL/LOG_ERROR）；InetAddress.h（peerAddr 要写它）；
 *        <sys/socket.h>（socket/accept 相关声明）、<errno.h>（EMFILE）、<unistd.h>（close）。
 * 【谁用它】TcpServer（构造函数里 new、start 里 listen、析构里随 unique_ptr 释放）。
 * 【先修知识】① socket/bind/listen/accept4 与 backlog；② SOCK_NONBLOCK/SOCK_CLOEXEC；
 *        ③ 水平触发下“一次事件只处理一个请求”的后果；④ EMFILE 是什么。
 * 【难度】★★☆☆☆   【建议耗时】25 分钟
 * 【自检】1) socket() 的三个 flags 各自防的是什么事故？
 *        2) 为什么 bind 在构造函数里做、listen 却留到 listen() 里做？
 *        3) accept 返回 -1 且 errno==EMFILE 时，本文件的做法会导致什么现象？
 * ============================================================================ */
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>

// 【设计】include 的选择解释了两个“完整类型”需求：InetAddress.h 是为了 handleRead 里
//   那个局部 peerAddr 对象（要 sizeof、要构造）；Channel 的完整定义由 Acceptor.h 带来。
#include "Acceptor.h"
#include "Logger.h"
#include "InetAddress.h"

// 【语法】static → 内部链接，只有本 .cc 看得见。它不做成 Acceptor 的成员函数，
//   因为调用点在**成员初始化列表**里（见构造函数那段），那时对象还没开始构造，
//   调成员函数语义上就错了（尤其如果它是非 const 成员，还会写 this）。
//   【易错】同类风格的 CheckLoopNotNull 在 TcpConnection.cc 与 TcpServer.cc 各有一份 static 副本，
//   靠 static 才没撞名（两份都叫 CheckLoopNotNull）。
// 【设计】把“创建 + 检查”包成函数，唯一目的是让返回值能直接出现在初始化列表里
//   （acceptSocket_(createNonblocking())）—— 因为 const/无默认构造的成员只能用初始化列表。
// 【流程】② 的第一个动作（造 listenfd）。
// 【线程】随 TcpServer/Acceptor 构造发生，实际在 mainLoop（用户主线程）。
static int createNonblocking()
{
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 【系统调用】socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP) │
// └────────────────────────────────────────────────────────────────────────────┘
// 【系统调用】三个实参的位置含义：domain=协议族（AF_INET 即 IPv4），type=语义类型，
//   protocol=具体协议。这里第二个参数写成“按位或”是 Linux 扩展：
//   SOCK_STREAM 值是 1；SOCK_NONBLOCK 值 04000000（八进制）= O_NONBLOCK；
//   SOCK_CLOEXEC 值 02000000 = O_CLOEXEC。后两者特意取高位，绝不与 type 的基本值冲突，
//   所以可以 OR 进同一个参数里 —— 这不是“碰巧能用”，而是设计好的位段。
//   等价的老写法：fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
//                  fcntl(fd, F_SETFL, O_NONBLOCK);  fcntl(fd, F_SETFD, FD_CLOEXEC);
// 【设计】flags 语义各防一种事故（规范点名要讲的）：
//   SOCK_NONBLOCK —— 非阻塞。listenfd 非阻塞的意义：accept 在无连接时返回 -1/EAGAIN
//     而不是卡住；一旦卡住，mainLoop 就不能再处理任何事件（EventLoop 是单线程串行）。
//     它同时还保证后续 accept4 不带 flags 时新 fd 继承 O_NONBLOCK（Linux 特性），
//     不过本项目 Socket::accept 又显式传了 SOCK_NONBLOCK，等于双保险。
//   SOCK_CLOEXEC —— exec 时自动关 fd。防的是“多线程进程里 A 线程刚 socket()、
//     B 线程立刻 fork+exec 某程序，那个 fd 就被泄漏给子进程”的竞态：
//     以前只能靠 fcntl 设 FD_CLOEXEC，两步之间就有窗口；flags 版一步到位、原子。
//   IPPROTO_TCP —— 值 6（/etc/protocols 里的 tcp）。对 SOCK_STREAM 来说填 0 也行，
//     显式写出来是给自己看的：“我要的是 TCP 流式套接字”。
// 【易错】SOCK_NONBLOCK/SOCK_CLOEXEC 是 **Linux 特有**（Windows/BSD 不支持），
//   这也是本库只能在 Linux 跑的原因之一。
// 【系统调用】失败返回 -1 并把 errno 置成 EMFILE/ENFILE/EPROTO/ENOMEM 等；
//   本项目 <0 就 LOG_FATAL（见下一行）→ exit(-1)，进程直接结束。
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
// 【设计】判 <0 是对的：fd 0/1/2 是标准输入输出，正常情况下 socket 不会返回 0，
//   但严格说如果 stdio 被关过就可能拿到 0 —— 用 <0 判失败比 <1 严谨。
//   【⚠问题】LOG_FATAL 内部是 exit(-1)，此时刚创建的 fd 还没人拥有、不会 close；
//   进程退出让内核回收，所以现实无害，但“构造函数里退出进程”这条路径要心里有数。
    if (sockfd < 0)
    {
        LOG_FATAL("%s:%s:%d listen socket create err:%d\n", __FILE__, __FUNCTION__, __LINE__, errno);
    }
    return sockfd;
}

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 构造函数：初始化列表的顺序问题（规范点名）                                   │
// └────────────────────────────────────────────────────────────────────────────┘
// 【语法】C++ 规则：**成员的实际初始化顺序 = 类内声明顺序**，与初始化列表的书写顺序无关。
//   Acceptor.h 里的声明顺序是 loop_ → acceptSocket_ → acceptChannel_ →
//   NewConnectionCallback_ → listenning_。本函数的初始化列表恰好也按这个顺序写，
//   所以没有 -Wreorder 警告（对照：src/TcpServer.cc 的初始化列表顺序与声明顺序不一致，
//   编译器就真报了警告）。
// 【易错】这里存在一条**跨成员的依赖**：acceptChannel_(loop, acceptSocket_.fd())
//   读的是 acceptSocket_。它能成立**只因为** acceptSocket_ 声明在 acceptChannel_ 之前。
//   一旦有人重排 Acceptor.h 里的两行声明（看起来很无害的整理动作），
//   acceptSocket_.fd() 读到的就是未初始化对象 —— 编译照样通过、运行期拿到垃圾 fd，
//   然后 Channel 拿着这个 fd 去 epoll_ctl，属于最难查的一类 bug。
// 【线程】mainLoop —— 由用户在主线程构造 TcpServer 间接创建本对象；构造里那些 socket/bind
//   系统调用本身与线程无关（哪个线程做都一样），但为了“之后所有 loop 相关操作都在 mainLoop”这条规矩，本项目统一放在主线程。
//   规矩：① 初始化列表里跨成员依赖必须“被依赖者先声明”；② 最好只依赖构造函数形参
//   （本项目 TcpConnection 里就用形参 loop/sockfd 而不是 loop_，见那里注释）；
//   ③ 打开 -Wall（含 -Wreorder）当护栏。
// 【语法】NewConnectionCallback_ 没出现在初始化列表里：它默认构造 = 空的 std::function，
//   等 TcpServer 构造函数来填。声明顺序上它排在 Channel 之后，所以“晚于 acceptChannel_
//   构造”，不影响 —— 但注意它的**赋值**发生在构造体里，晚于本列表。
// 【流程】②：socket → setReuseAddr/Port → bind → acceptChannel_ 绑 handleRead。
Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
// 【设计】loop_ 只存不用（handleRead 里没碰它）：真正的注册动作在 listen() 里
//   通过 acceptChannel_ 自己的 ownerLoop() 走到 Poller。留着它是为了让“这个
//   Acceptor 属于哪个 loop”在调试时可见，也方便将来加 assertInLoopThread。
    : loop_(loop)
// 【流程】② 的第一步：拿到 listenfd。调用上面的 createNonblocking()，非阻塞 + CLOEXEC。
//   【线程】构造发生在 mainLoop（用户在主线程构造 TcpServer），但严格说这跟 loop 无关，
//   socket/bind 是线程安全的系统调用，哪个线程做都行。
    , acceptSocket_(createNonblocking())
// 【设计】② 的第二步：为 listenfd 建 Channel，把 fd 与 loop 登记进去。
//   【易错】Channel 构造 ≠ 注册进 epoll：此时 events_=0、index_=-1，epoll 里没有它。
//   【语法】acceptSocket_.fd() 是“跨成员读取”，见上面那段声明顺序的说明。
    , acceptChannel_(loop, acceptSocket_.fd())
    , listenning_(false)
{
// 【流程】② 的第三步：设置 socket 选项 + 占坑。三行都在构造函数里做，一次到位，
//   之后再也改不了（对象已交给 mainLoop 的 epoll，改选项要重连才生效）。
//   【设计】为什么这些动作在构造而不是 listen()：它们是“绑地址”的前置条件，
//   顺序必须是 setsockopt → bind → listen。放到 listen() 里也行，但失败会推迟暴露。
// 【系统调用】SO_REUSEADDR：允许 bind 一个处于 TIME_WAIT 的本地端口（服务重启时最常见，
//   否则要等 2MSL≈60 秒才能重新起）。它是“重启不报 EADDRINUSE”的关键，
//   注意它**不**允许 bind 一个仍在 LISTEN 的同端口（那是 SO_REUSEPORT 的活）。
    acceptSocket_.setReuseAddr(true);
// 【⚠问题】这里无条件 setReusePort(true)，完全没看构造函数的 reuseport 形参 ——
//   TcpServer 那边 Option kReusePort/kNoReusePort 的区分因此失效（-Wunused-parameter
//   已在报这件事）。SO_REUSEPORT 的语义是“多个 socket 可绑同一 ip:port，内核按连接做
//   负载均衡”，本意是给“多进程/多 listenfd”用的；对单 listenfd 的本项目影响只是
//   “意外允许别的进程再绑同一端口”（端口冲突变成静默）。
//   正确写法应是 acceptSocket_.setReusePort(reuseport)。通行说法里的“bind/setReuseAddr
//   只在构造做一次”这条说的就是这三行的位置，而 reuseport 的失效是它的伴生问题。
    acceptSocket_.setReusePort(true);
// 【系统调用】bind(2)：把 listenfd 与 (ip, port) 绑定。Socket::bindAddress 里判
//   0 != bind(...) → LOG_FATAL，所以“端口被占/权限不足(<1024 需要 root)”都在构造时炸。
//   【设计】listenAddr 通常只给端口（InetAddress(8080) → INADDR_ANY 0.0.0.0），
//   表示“本机所有网卡都接”。给具体网卡地址则是“只在这块卡上接客”。
//   【易错】bind 之后不 listen，SYN 会没人管（内核直接不回 SYN-ACK，客户端超时），
//   所以构造完成到 start 之间有一个“端口看着被占但连不上”的窗口。
    acceptSocket_.bindAddress(listenAddr);
    // TcpServer::start() => Acceptor.listen() 如果有新用户连接 要执行一个回调(accept => connfd => 打包成Channel => 唤醒subloop)
    // baseloop监听到有事件发生 => acceptChannel_(listenfd) => 执行该回调函数
// 【流程】② 的第四步（构造体最后一件事）：把 handleRead 绑到 Channel 的 readCallback_。
//   【设计】这是 Acceptor 唯一的连线动作。之后整条链：
//   epoll_wait → Channel::handleEvent → readCallback_ → handleRead → NewConnectionCallback_
//   → TcpServer::newConnection（⑥）。
//   【语法】std::bind(&Acceptor::handleRead, this)：handleRead 无参，所以不写占位符；
//   而 Channel::readCallback_ 是 std::function<void(Timestamp)> —— bind 结果被调用时
//   会收到一个 Timestamp，但 bind 里没有 _1 占位符，该实参被**丢弃**（bind 允许多余实参）。
//   对照 src/TcpConnection.cc 构造函数：handleRead 收 Timestamp，就必须写
//   std::placeholders::_1 才能接上。
//   【易错】这里绑的也是裸 this，Acceptor 不参与 shared_from_this 体系；
//   它安全的原因是 Acceptor 的生命周期不短于 Channel（Channel 是它的值成员，
//   析构顺序里 Channel 一定比 Acceptor 先死）—— 这就是“值成员 + 裸 this”成立的场景，
//   和 TcpConnection/Channel 那种“跨线程、寿命不固定”的情况完全不同。
    acceptChannel_.setReadCallback(
        std::bind(&Acceptor::handleRead, this));
}

// 【流程】进程退出路径（mainLoop 线程）。
//   【设计】disableAll → epoll_ctl MOD 把事件清零；remove → EventLoop::removeChannel →
//   Poller::removeChannel：epoll_ctl DEL 并把 Channel 从 Poller 的表里摘掉。
//   两步都要做：只 disableAll 会留下一个“kNoneEvent 的表项”（本项目靠 EPollPoller 的 index_ 区分新旧，
//   状态是必须的，否则 Poller 里表项与 Channel 生命周期不一致）。
//   【易错】析构顺序：先跑本函数体（此时 acceptChannel_ / acceptSocket_ 都还活着），
//   然后才按声明反序销毁 NewConnectionCallback_ → acceptChannel_ → acceptSocket_，
//   最后 ~Socket 才 close(listenfd)。所以“从 epoll 摘掉”严格早于“close fd”，
//   这是正确的顺序（反过来 close 后再 DEL 可能报 ENOENT，且 epoll 可能已经把它
//   错认成新的同号码 fd）。
//   【对比原版】原版 muduo 的 Acceptor 析构里还有一句 ::shutdown(acceptSocket_.fd(), SHUT_RDWR)
//   （或等价的 sockets::shutdown），原因就是：直接 close 一个仍有“接受队列积压”的
//   listenfd，内核会对队列里那些已完成握手但没被 accept 的连接发 **RST** 而不是 FIN，
//   客户端表现为“连上就断、read 报 ECONNRESET”。本项目只 close（随 ~Socket），
//   所以退出时正在排队的连接会收到 RST。规范点名的就是这一条。
// 【线程】mainLoop（由 ~TcpServer 销毁 unique_ptr<Acceptor> 触发，通常在主线程）。
Acceptor::~Acceptor()
{
    acceptChannel_.disableAll();    // 把从Poller中感兴趣的事件删除掉
    acceptChannel_.remove();        // 调用EventLoop->removeChannel => Poller->removeChannel 把Poller的ChannelMap对应的部分删除
}

// 【流程】③ 的执行体：真正开始接客。由 TcpServer::start 通过
//   loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get())) 投递，
//   所以它一定跑在 mainLoop 线程（哪怕 start 被别的线程调用）。
//   【设计】三步的顺序讲究：先记标志（此刻还没有任何 IO 事件可能被处理，安全），
//   再 listen(2)（内核开始接受 SYN），最后 enableReading（把 listenfd 交给 epoll）。
//   反过来先 enableReading 就可能在一个还没 listen 的 fd 上收到 EPOLLIN ——
//   语义上不可能发生（没 listen 就不可能有新连接事件），但顺序讲清楚有助于理解状态。
//   【易错】enableReading 是本文件里唯一一次把 listenfd 注册进 epoll。
//   【面试】“muduo 的 listenfd 什么时候进 epoll？”→ Acceptor::listen() 里，
//   由 TcpServer::start 投递到 mainLoop 执行。
// 【线程】mainLoop —— 由 TcpServer::start 的 loop_->runInLoop 投递，绝不会在 subLoop 跑。
void Acceptor::listen()
{
    listenning_ = true;
// 【系统调用】listen(2)：把被动套接字变成 LISTEN，并建立 backlog（ Socket::listen 里
//   写的是 1024）。backlog 的语义（Linux）：限制的是**已完成三次握手、等待 accept**
//   的“全连接队列”上限（半连接队列由 tcp_max_syn_backlog 控制），
//   backlog 太小会在连接风暴时丢 SYN-ACK/回 RST。
//   【易错】accept 不及时（mainLoop 卡在别的事件上）会让全连接队列堆满，
//   新连接的表现是“TCP 层连上但迟迟没有 onConnection”，不是失败。
    acceptSocket_.listen();         // listen
    acceptChannel_.enableReading(); // acceptChannel_注册至Poller !重要
}

// listenfd有事件发生了，就是有新用户连接了
// 【流程】⑤ 的执行体，跑在 mainLoop。上游：Channel::handleEvent（readCallback_）；
//   下游：NewConnectionCallback_ → TcpServer::newConnection（⑥）。
//   【易错】“listenfd 可读”≠“只有一个新连接”：接受队列此刻可能积压了 N 个，
//   本函数一次只 accept 一个，剩下的靠 LT 下一轮再报 EPOLLIN 逐个处理（详见下面 accept4 返回值三种情况那段说明）。
// 【线程】mainLoop（listenfd 的 Channel 注册在 mainLoop 的 Poller 上）。
void Acceptor::handleRead()
{
// 【设计】peerAddr 是个**默认构造**的局部 InetAddress（未初始化内容也无所谓，
//   accept4 会在成功时把对端 sockaddr_in 写进来，Socket::accept 再 setSockAddr 填好）。
//   【易错】它只在 accept 成功时被填；失败分支里它是“垃圾值”，本函数没用它就没问题。
//   【对比原版】原版 muduo 会把 sockaddr_storage + 一个 inet_pton 缓存的
//   “接受即填充”小函数拆开写，并用 sockets::sockaddr_storage_cast 兼容 IPv6。
    InetAddress peerAddr;
// 【系统调用】Socket::accept 内部是 accept4(sockfd, addr, &len, SOCK_NONBLOCK|SOCK_CLOEXEC)：
//   返回值三种情况：① >=0 新连接 fd（已非阻塞，所以 subLoop 拿去做 LT 读写没问题）；
//   ② -1/EAGAIN：队列里没有完成的连接（虚假可读，或者已经被上一次 accept 取完）—— 正常；
//   ③ -1/EINTR（被信号打断，可重试）、EMFILE/ENFILE（进程/系统 fd 耗尽）、
//     ECONNABORTED（对端在 accept 前发了 RST，属于“正常损耗”）、EPERM（防火墙规则）。
//   【易错】accept4 比老式 accept+fcntl 少了竞态窗口（多线程时新 fd 的 O_NONBLOCK
//   可能在两步之间被别人观察到阻塞状态）。原版 muduo 的注释里专门提过这一点。
//   【对比原版】规范 A2 项里说的“accept 后没有处理 EAGAIN/ECONNABORTED”：本函数
//   对 -1 一律只 LOG_ERROR，不区分 EAGAIN。LT 模式这样能工作（下一次还会报可读，
//   只是白打一条日志）；**ET 模式绝对不行**，必须 while (true) accept 到 EAGAIN 为止。
//   原版 muduo 用循环 accept（书里 11.2 节讨论过：一次事件 accept 多个连接能减少
//   epoll_wait 次数、降低“接受队列堆满”的概率）。
    int connfd = acceptSocket_.accept(&peerAddr);
// 【流程】⑤ → ⑥ 的分界：拿到 connfd 就交给上层。
//   【语法】判据写的是 connfd >= 0 而不是 > 0。fd==0 理论上可以是合法值（若 stdin 被关掉，
//   socket/accept 就能拿到 0），所以 >=0 更正确；代价是 0 号 fd 被当成连接时
//   printf 之类会写错地方，实际项目不会关 stdio，因此两种写法都算安全。
    if (connfd >= 0)
    {
// 【设计】判空是必要的：万一 TcpServer 构造没设回调（或用户自建 Acceptor 忘了设），
//   下面 else 分支会关掉 connfd 而不是泄漏。见下面 else 分支那句 ::close(connfd)。
        if (NewConnectionCallback_)
        {
// 【流程】⑥ 的入口：这一行就是规范说的“Acceptor 不认识 TcpServer”的实现现场 ——
//   调用的是一个**值语义的 std::function**，里面装着 TcpServer::newConnection
//   （由 TcpServer 构造函数 std::bind 好塞进来）。
//   【线程】仍在 mainLoop（同步调用栈：handleEvent → handleRead → 这里 → newConnection）。
//   真正的【线程跳转 1】发生在 newConnection 末尾的 ioLoop->runInLoop。
//   【易错】回调是同步执行的，所以 newConnection 里任何阻塞都会卡住 mainLoop 的 accept；
//   它的实现刻意只做“new 对象 + 填表 + 投递任务”，不做 IO。
            NewConnectionCallback_(connfd, peerAddr); // 轮询找到subLoop 唤醒并分发当前的新客户端的Channel
        }
        else
        {
// 【易错】这一句 close 是**唯一**防 fd 泄漏的护栏：没有回调却 accept 成功了，
//   connfd 就变成一个没人管的 fd。原版 muduo 里对应写法是 sockets::close(sockfd)。
//   客户端此时看到的是“连接建立成功 → 立刻被关”，排障要点就是查有没有设回调。
            ::close(connfd);
        }
    }
    else
    {
// 【⚠问题】失败分支一律 LOG_ERROR，把 EAGAIN/ECONNABORTED 也当错误刷屏：
//   虚假可读（队列已空）在非阻塞 accept 里是**常见**事件，不该按错误级别打。
//   【易错】errno 在被 LOG_ERROR 使用之前仍是 accept4 设置的那个值 ——
//   日志函数本身在 LOG_ERROR 里会 write 到 stderr，若那次 write 失败可能污染 errno，
//   所以下面 EMFILE 的判断严格说来“可能读到被改掉的 errno”，稳妥做法是先存再判
//   （本项目 TcpConnection::handleRead 用了 savedErrno 的手法，这里没有）。
        LOG_ERROR("%s:%s:%d accept err:%d\n", __FILE__, __FUNCTION__, __LINE__, errno);
// 【系统调用】EMFILE：本进程 fd 已达 RLIMIT_NOFILE 上限。此时 accept 失败、
//   connfd 拿不到，但**接受队列里那个连接还在** → listenfd 仍然可读 →
//   LT 模式下 epoll_wait 立刻再返回 → 再 EMFILE → 再打日志…… 主线程陷入
//   “日志洪水 + CPU 100%”，而且没有任何自愈机会（fd 一直不释放，除非有连接断开）。
// 【⚠问题】本项目只多打一条“sockfd reached limit”就结束，没有补救手段。
// 【对比原版】陈硕原版的处理（书里专门一节）：进程启动时先 open("/dev/null", O_RDWR)
//   拿住一个 100MB 的“紧急 fd”，accept 返回 EMFILE 时 close 掉它腾出一个槽位，
//   再 accept 一次拿到 connfd 立刻 close 并回写一条告警，最后把紧急 fd 再占回去。
//   这样既不会死循环，也能让客户端拿到一个 FIN 而不是挂着等超时。
//   更工程化的做法还有：EMFILE 时 disableAll 这个 Channel 并挂一个定时器，稍后再恢复。
        if (errno == EMFILE)
        {
            LOG_ERROR("%s:%s:%d sockfd reached limit\n", __FILE__, __FUNCTION__, __LINE__);
        }
    }
}