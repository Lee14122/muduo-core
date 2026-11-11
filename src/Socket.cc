/* ===========================================================================
 * 【文件】src/Socket.cc
 * 【一句话】把 bind / listen / accept4 / shutdown / setsockopt 这五个内核调用各包一层，
 *          并在析构里 close(fd) —— 全库唯一真正"关闭 socket"的地方。
 * 【Reactor 角色】基础设施 / OS 封装层。它不参与事件分发，只负责"控制面"：
 *          建立监听（②③）、接收新连接（⑤）、半关闭（⑩）、选项开关（②⑥）。
 * 【依赖】unistd.h(::close) sys/socket.h(::bind ::listen ::accept4 ::shutdown ::setsockopt
 *          sockaddr) netinet/tcp.h(TCP_NODELAY) string.h(::memset) —— 加上本项目的
 *          Socket.h、Logger.h（失败日志）、InetAddress.h（bind/accept 的地址双向转换）。
 * 【谁用它】② Acceptor 构造：setReuseAddr + setReusePort + bindAddress（mainLoop）；
 *          ③ Acceptor::listen → listen()（mainLoop）；⑤ Acceptor::handleRead → accept()
 *          （mainLoop）；⑥ TcpConnection 构造 → setKeepAlive（subLoop#N）；
 *          ⑩ TcpConnection::shutdownInLoop → shutdownWrite()（subLoop#N）；
 *          ⑪ TcpConnection 引用计数归零 → ~Socket → ::close（subLoop#N）。
 * 【先修知识】1) listenfd 与 connfd；2) 非阻塞 fd 与 epoll/LT 的关系；3) TCP 四次挥手与
 *            TIME_WAIT；4) Nagle 算法与延迟确认；5) fork/exec 时 fd 的继承规则。
 * 【难度】★★★★☆（每行都短，每行背后都是一个内核机制）  【建议耗时】40 分钟
 * 【自检】a) 为什么用 accept4 而不是 accept？两个 flag 各自防的是什么事故？
 *        b) SO_REUSEADDR 和 SO_REUSEPORT 有什么区别？各解决哪个具体问题？
 *        c) 四个 setter 里哪个在本项目从来没被调用过？哪个错误处理最危险？
 * ========================================================================= */

// 【依赖】unistd.h：::close（析构里唯一的资源释放）。
#include <unistd.h>
#include <sys/types.h>
// 【依赖】sys/types.h：socklen_t、ssize_t 等历史类型（POSIX 规定它们在这里）。
//          在现代 glibc 上 include sys/socket.h 就够，这一行是"照着老写法抄"的残留。
// 【依赖】sys/socket.h：socket 家族的全部 API 与 sockaddr 通用地址类型。
#include <sys/socket.h>
#include <string.h>
// 【依赖】string.h：::memset，accept 前把出参 sockaddr_in 清零。
#include <netinet/tcp.h>
// 【依赖】netinet/tcp.h：TCP_NODELAY 这个常量在这里（IPPROTO_TCP 层唯一的选项）。
//          【易错】下面紧跟着又 include 了一遍 sys/socket.h（与前面重复）。头文件有
//          include guard（或本项目 Socket.h 用 #pragma once），重复无害，但读代码时
//          会误以为"后面那次带的是别的东西"。本项目不改（铁律），只要认得它是噪声即可。
#include <sys/socket.h>

#include "Socket.h"
#include "Logger.h"
// 【依赖】LOG_FATAL / LOG_ERROR 宏。注意 LOG_FATAL 内部会 exit(-1)（见 Logger.h），
//          所以 bind/listen 一旦失败就是"进程当场自杀"，后面的代码根本不会执行。
#include "InetAddress.h"

// ── 全库唯一 close(connfd) 的地方 ───────────────────────────────────────────
// 【流程】⑪ 的最末尾：客户端关闭 → handleClose → removeConnection（线程跳转 2 到 mainLoop）
//          → connectDestroyed（线程跳转 3 回 subLoop）→ disableAll + channel_->remove()
//          → TcpConnection 引用计数归零 → ~TcpConnection → unique_ptr socket_ 析构 → 这里。
// 【线程】connfd 所属的 subLoop#N（"在哪个 loop 创建就在哪个 loop 销毁"）。
// 【设计】为什么 close 必须排在 channel_->remove() 之后：epoll 的注册表里记的是 fd 号。
//          如果先 close 后 remove，epoll_ctl(DEL) 会对一个已失效的 fd 操作（返回 EBADF），
//          更糟的是这个 fd 号可能已被新 accept 的连接复用，于是新连接的注册被误删；
//          反过来（先摘 epoll 再 close）任何时刻都不会出现"epoll 里挂着无人拥有的 fd"。
// 【系统调用】::close 对 socket 而言是"关掉这一端"：引用计数减一，归零时内核发 FIN
//            （若接收队列里还有未读数据，则发 RST）。close 可能返回 EINTR（被信号打断），
//            但 Linux 上 fd 一定已经被释放，不需要重试 —— 这点和 read/write 的 EINTR 不同。
// 【易错】不做错误检查：EBADF（说明 fd 已经不是我们的了，往往是 double close 的现场）
//            在这里会被静默吞掉，fd 泄漏或误关别人的 fd 都不会报出来。
// 【对比原版】原版 muduo 的 Socket 析构同样 ::close，但另有 sockets::close 自由函数并
//            用 LOG_SYSERR 打失败；原版还区分 shutdown 与 close 的使用场景（TcpServer 析构
//            前会先 shutdown，避免对端看到 RST）。
Socket::~Socket()
{
    // 【设计】析构函数里只做这一件事，因此 ~Socket 不抛、不死锁、不依赖别的对象存在，
    //            在异常展开或进程退出路径上也安全。
    // 【面试】"muduo 里谁负责关 connfd？" —— Socket。Channel 只引用 fd 不拥有它，
    //            EventLoop/Poller 更不关；所以"Socket 析构 = 连接资源真正回收"。
    ::close(sockfd_);
}

// 【做什么】把 listenfd 绑到一个具体的 IP:端口上，绑定成功后这个端口就归本进程独占。
// 【流程】② Acceptor 构造里调用，位置在 setReuseAddr(true) 与 setReusePort(true) 之后
//          —— 这个顺序不是风格问题：SO_REUSEADDR 必须在 bind 之前设，本次 bind 才能
//          用上它；bind 完再设，只对"下一次 bind"起作用。
// 【线程】mainLoop（Acceptor 属于 baseLoop）。
// 【设计】参数是 const InetAddress&：地址在这里只被读，::bind 拿到的指针来自
//          getSockAddr()（只读出口），所以 const 一路传下来很自然。
void Socket::bindAddress(const InetAddress &localaddr)
{
    // 【系统调用】::bind(fd, addr, addrlen)：把"未命名的 socket"与一个本地地址关联。
    //            三个参数：fd 必须是已创建且未绑定过的；addr 是 (sockaddr*)强转的
    //            sockaddr_in（内核按 sa_family 决定怎么读后面 14 字节）；
    //            addrlen 必须等于 sizeof(sockaddr_in)=16，写小了截断、写大了 EINVAL。
    //            成功返回 0，失败返回 -1 并设 errno。它是同步的、不阻塞（对 AF_INET）。
    //            常见 errno：EADDRINUSE（端口已被活着的 listener 占用 或 上一个进程留下的
    //            TIME_WAIT 且没设 SO_REUSEADDR）、EACCES（绑 1~1023 的保留端口却没有
    //            root 或 CAP_NET_BIND_SERVICE）、EADDRNOTAVAIL（这个 IP 不在本机任何网卡上，
    //            比如容器里写了一个宿主机 IP）、EBADF/ENOTSOCK（fd 不是 socket）、
    //            EINVAL（已经绑过一次，或 sa_family 不对）。
    // 【易错】"0 != 判失败" 与 "小于 0 判失败" 的语义差别：
    //            bind/listen 这类"纯状态型"调用只可能是 0 或 -1，两种写法等价；
    //            但 0 != 更贴合 POSIX 惯例（"非 0 即失败"），并且如果哪天换成某个
    //            "失败时返回正错误码"的接口，写成 < 0 就会把失败当成功。
    //            反过来，返回值有"数量含义"的调用必须分开判：本文件 accept 用 connfd >= 0
    //            （0 和正数都是有效 fd），shutdownWrite 用 < 0（只有出错才是负数），
    //            Buffer::readFd/writeFd 则要把 0（对端关闭/没数据）和负数（错误）分三路走。
    //            —— 这就是值得留意的"0 != 与 < 0 的差别"在同一个文件里的三种写法。
    // 【设计】强转 (sockaddr*) 而不是 (const sockaddr*)：内核 API 是 C 的，签名不带 const，
    //            所以我们只能把只读的地址伪装成可写传给内核。
    if (0 != ::bind(sockfd_, (sockaddr *)localaddr.getSockAddr(), sizeof(sockaddr_in)))
    {
    // 【⚠问题】失败即 exit(-1)：LOG_FATAL 宏内部先打日志再 exit(-1)，
    //            1) 不走任何 RAII 析构：已经建好的 listenfd、wakeupfd、线程都不会被清理，
    //            2) 不刷新 std::cout 缓冲区，这一条错误信息有可能根本没打印出来进程就没了
    //               （include/Logger.h 里也提了同一件事，两处连起来看），
    //            3) 若别的线程已经在跑，它们会继续跑一小段才被进程退出打断。
    //            教学上可以接受（bind 失败本来也起不来），但要能说出"为什么不能用 abort/exit"
    //            的差别：原版用 abort 是为了留 core dump 便于 gdb 定位，exit 是干净退场，
    //            而带缓冲的日志在两种情况下都可能丢。
    // 【易错】这里的 sockfd_ 值是"进程内 fd 号"，写进日志才有用；单看 errno 更关键，
    //            而本条日志没打 errno（对比 Acceptor 里 createNonblocking 的 LOG_FATAL 打了
    //            __FILE__/__FUNCTION__/__LINE__/errno），排查 bind 失败时要靠外部 strace。
        LOG_FATAL("bind sockfd:%d fail\n", sockfd_);
    }
}

// 【做什么】把已经 bind 的 socket 变成"被动 socket"：内核开始收 SYN、建半连接队列，
//            并把完成握手的连接放进 accept 队列等着被取走。
// 【流程】③ TcpServer::start → loop_->runInLoop(&Acceptor::listen) → acceptSocket_.listen()，
//            紧接着 enableReading() 把 listenfd 交进 epoll。
// 【线程】mainLoop。【失败会怎样】LOG_FATAL → exit(-1)，和 bind 一样直接自杀。
void Socket::listen()
{
    // 【系统调用】::listen(fd, backlog)：成功 0，失败 -1。常见 errno：EBADF/ENOTSOCK、
    //            EOPNOTSUPP（SOCK_DGRAM 之类不可监听的类型）、EADDRINUSE（UDP 才常见）、
    //            EINVAL（还没 bind 就 listen 一个 AF_INET 流式 socket 其实允许，
    //            但会随机分配端口，所以本项目的顺序 bind→listen 是刻意的）。
    // 【设计】backlog = 1024 的含义：它是"accept 队列（全连接队列）长度的上限请求"，
    //            不是"同时能有多少连接"，也不是半连接队列大小。
    // 【易错】内核并不会照单全收：实际长度取 min(backlog, /proc/sys/net/core/somaxconn)。
    //            somaxconn 在老内核默认 128（Linux 5.4 之后默认 4096），所以这个 1024 在
    //            多数机器上会被压到 128 —— 写了 1024 不代表真有 1024。
    //            半连接（SYN 已收、握手未完）由 tcp_max_syn_backlog 控制，与这里无关。
    // 【系统调用】队列满了会发生什么：默认 tcp_abort_on_overflow=0 时内核直接丢弃这个 ACK，
    //            客户端以为 connect 成功了（三次握手在服务端没记为完成），发数据也得不到
    //            响应，表现成"连上但卡住"；把 tcp_abort_on_overflow 设成 1 才会回 RST，
    //            客户端立刻拿到 ECONNRESET。高并发压测时这两类现象要能分辨。
    // 【对比原版】原版 muduo 用 sockets::listen(sockfd) 且 backlog 是常量 1024，一致；
    //            陈硕的建议是把它做成可配置项，并配合调大 somaxconn。
    if (0 != ::listen(sockfd_, 1024))
    {
    // 【⚠问题】同 bindAddress：exit(-1) 不析构、不刷缓冲；这里额外注意 listen 失败
    //            几乎只可能是 fd 状态不对（已被别的线程关掉/重复 listen），而不是端口问题
    //            —— 端口占用在 bind 那一步就已经死了。所以看到这条日志要怀疑代码路径。
        LOG_FATAL("listen sockfd:%d fail\n", sockfd_);
    }
}

// 【做什么】从 accept 队列头取一个已完成握手的连接，拿到一个新 fd（connfd），
//            并把对端地址填进 *peeraddr。listenfd 本身不变，继续监听。
// 【流程】⑤ 客户端 connect → 内核回 SYN-ACK → 客户端 ACK → 连接进 accept 队列 →
//            listenfd 上 EPOLLIN → Channel::handleEvent → Acceptor::handleRead → 这里。
// 【线程】mainLoop（one loop per thread 的 mainReactor；本项目所有 accept 都在主线程，
//            connfd 拿到后立刻通过 NewConnectionCallback 分派到 subLoop#N）。
// 【设计】这一层是"fd 出生"的地方：返回的是裸 int，所有权交给调用方，最终由
//            TcpConnection 里的 new Socket(connfd) 接管（⑥）。这样"fd 从哪来、归谁管"
//            在代码里是可追溯的，不会有两个所有者。
int Socket::accept(InetAddress *peeraddr)
{
    /**
     * 1. accept函数的参数不合法
     * 2. 对返回的connfd没有设置非阻塞
     * Reactor模型 one loop per thread
     * poller + non-blocking IO
     **/
    // ── 原作者在上面的块注释里点出的两个问题（本函数就是要解决它们）───────────────
    //    1) 参数不合法：老写法把 InetAddress* 直接强转成 sockaddr* 交给内核，
    //       类型/大小都可能不一致；现在的写法是先给一个真正的裸 sockaddr_in，
    //       让内核写它，再靠 setSockAddr 把结果受控地搬回对象（见 include/InetAddress.h）。
    //    2) connfd 没有设非阻塞：老写法 ::accept 返回的 fd 默认是阻塞的，即使 listenfd
    //       非阻塞也不继承 —— 必须用 accept4 的 SOCK_NONBLOCK 显式指定。
    sockaddr_in addr;
    // 【系统调用】第三个参数是"输入 与 输出"双向的：调用前必须设成"我能接受多大地址"，
    //            调用后内核把它改成"实际写了多少字节"。IPv4 时始终 16。
    //            socklen_t 是无符号类型（typedef uint32_t），不要用 int 代替 ——
    //            传成 int 在部分平台上会被解读成负数从而 EINVAL。
    socklen_t len = sizeof(addr);
    // 【设计】清零是"洁癖式防御"：内核会整体覆盖这 16 字节（包括 sin_zero 填充），
    //            所以严格说不是必须；但不清零时若 accept 失败/截断，后面 setSockAddr
    //            搬进去的就是栈垃圾，日志里会打出诡异的端口。
    ::memset(&addr, 0, sizeof(addr));
    // ── accept 与 accept4 的区别，这一行注释就是本项目最有价值的一处修改 ──────────
    // 【系统调用】::accept(fd, addr, len) 等价于 ::accept4(fd, addr, len, 0)。
    //            accept4 多一个 flags 参数，可以带上：
    //              SOCK_NONBLOCK —— 新 fd 一开始就是非阻塞（等价于 accept 后 fcntl(F_SETFL, O_NONBLOCK)）
    //              SOCK_CLOEXEC  —— 新 fd 带上 close-on-exec 标志（等价于 accept 后 fcntl(F_SETFD, FD_CLOEXEC)）
    //            两者的关键不只是"少写两行"，而是"少一个竞争窗口"，见 SOCK_NONBLOCK 与 SOCK_CLOEXEC 两段的说明。
    //            需要 Linux 2.6.28 以上；失败 errno 可能多一个 EINVAL（flags 里有本内核
    //            不支持的位，或该地址族不支持 SOCK_NONBLOCK）。
    // fixed : int connfd = ::accept(sockfd_, (sockaddr *)&addr, &len);
    // 【系统调用】::accept4(sockfd, (sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC)
    //            非阻塞 listenfd 上队列为空时：立刻返回 -1，errno = EAGAIN/EWOULDBLOCK，
    //            绝不挂起线程。阻塞 listenfd 上：线程睡在内核里等新连接（本项目 listenfd
    //            在 createNonblocking 里就是非阻塞的，所以走前者）。
    //            返回 0 或正数都是有效 connfd（0 是合法的 fd 号，只是通常 0/1/2 被
    //            stdin/stdout/stderr 占了，所以看起来"从没见过 0"）。
    //
    // 【为什么 SOCK_NONBLOCK 是 Reactor 的命脉】
    // 【设计】muduo 是"一个 loop 线程管成千上万条连接"：EventLoop 里 epoll_wait 拿到就绪
    //            列表后，逐个调 Channel::handleEvent → TcpConnection::handleRead → read。
    //            如果 connfd 是阻塞的，只要某一条连接的内核接收缓冲区是空的（对端没发数据，
    //            或者只是 EPOLLOUT/EPOLLHUP 触发），那一次 read 就会把整个 loop 线程睡死
    //            在内核里 —— 同一条 loop 上其余几千条连接一起停摆，而且再也没人 epoll_wait，
    //            连"别的连接来数据"都无法被发现。一个慢客户端就能让一个线程的服务全瘫。
    //            所以 Reactor 的前提是"每个 fd 都不阻塞"，非阻塞之后 read/write 的语义变成
    //            "能读多少读多少，读不到返回 EAGAIN"，事件循环才能靠 EPOLLIN/EPOLLOUT 驱动。
    // 【易错】ET（边缘触发）模式下非阻塞是硬要求（必须循环读到 EAGAIN 才不算漏事件）；
    //            LT（本项目 Poller 的工作模式）下阻塞 fd 不是"立刻错"，而是"偶尔卡死"，
    //            所以更隐蔽 —— 测试环境流量小看不出来，一上线就爆。
    // 【设计】fd 的非阻塞属性是"每个 fd 各自一份"，不会从 listenfd 继承到 connfd，
    //            也不会从一个 socket 复制到另一个 socket —— 这正是"必须用 accept4 或
    //            记得 fcntl"的根本原因，也是原作者注释里第 2 条的点。
    //
    // 【为什么 SOCK_CLOEXEC 防 fd 泄漏】
    // 【设计】exec 一个新程序时，凡是没设 FD_CLOEXEC 的 fd 都会被子进程继承下去。
    //            服务端如果有 fork/exec（调外部命令、CGI、热重启），被继承的 connfd
    //            会让这条 TCP 连接"在两个进程里都开着"：我们这边 close 了，内核也不发 FIN
    //            （引用计数还在），客户端看到连接不断，端口一直被占，连接数统计也不对。
    // 【易错】为什么不能"accept 之后再 fcntl 设"：这两步之间有窗口。多线程程序里
    //            另一个线程可能正好 fork+exec，此刻 flags 还没设上，新 fd 就被泄漏进子进程；
    //            而 accept4 把"创建 fd"和"设 CLOEXEC"合成一次原子操作，内核里不存在中间状态。
    //            这也是 accept4、socket(SOCK_CLOEXEC)、epoll_create1、open(O_CLOEXEC)
    //            这一批"带 4 / 带 _CLOEXEC 的新接口"被发明的唯一理由。
    // 【对比】本项目同样风格的地方：Acceptor 的 createNonblocking（socket 加 SOCK_NONBLOCK|
    //            SOCK_CLOEXEC）、EventLoop 的 createEventfd（eventfd 加 EFD_NONBLOCK|EFD_CLOEXEC），
    //            三处合起来才是"整个库的 fd 都非阻塞 且 不会漏进 exec 的子进程"。
    //
    // 【⚠问题】返回值没被检查的错误码：EAGAIN/EWOULDBLOCK、ECONNABORTED、EINTR 都只被
    //            当成"失败"往上抛给 Acceptor::handleRead 打一条 LOG_ERROR。
    //            1) EAGAIN（队列空）在 LT 下完全正常，不该报错，也不该重试；
    //            2) ECONNABORTED（对端在完成握手后马上 RST，比如 SYN flood、健康检查探针、
    //               客户端超时主动断）也不该算错误，直接忽略继续等下一个事件；
    //            3) EINTR（被信号打断）应该重试。
    // 【对比原版】原版这里也是"一次 accept4 返回 connfd"，但陈硕在《Linux 多线程服务端
    //            编程》里明确建议：即便用 LT，也应当循环 accept 直到 EAGAIN，因为
    //            每次"从 epoll_wait 返回来处理一个连接"都有固定开销，一次事件里把队列
    //            清空能把 epoll_wait 的调用次数摊薄到接近"每连接一次系统调用"；
    //            而 ET 模式下循环 accept 是强制的。本项目为教学简化只取一个连接，
    //            靠 LT 会重复通知来保证不丢 —— 正确，但高频连接场景吞吐会吃亏。
    int connfd = ::accept4(sockfd_, (sockaddr *)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    // 【易错】判成功的正确写法是 "connfd >= 0"，不是 "> 0"：fd 0 是合法值。
    //            也和 bind/listen 的 "0 != 才失败" 形成对照 —— 返回值有"数量/句柄"含义时，
    //            0 与负数是两种完全不同的事，这是本文件反复出现的判断风格差异的根源。
    if (connfd >= 0)
    {
    // 【设计】内核格式 变成 对象格式的那一步，也是 include/InetAddress.h 里
    //            为什么需要 setSockAddr（受控写入口）的直接答案。
    // 【易错】只在成功时写：connfd 失败时 *peeraddr 保持调用者的原值（Acceptor 那边是
    //            一个用默认参数构造的占位地址 127.0.0.1:0），调用方必须先看返回值
    //            再决定信不信 peeraddr —— 顺序不能反。
        peeraddr->setSockAddr(addr);
    }
    // 【设计】返回裸 int 的意义：让上层有机会在"接管它"之前先做决定（Acceptor 判断
    //            有没有注册回调，没有就直接 ::close(connfd)，避免把 fd 交出去又没人管）。
    // 【流程】⑤ 的终点；下游是 ⑥ TcpServer::newConnection（把 connfd 打包进 TcpConnection，
    //            再 new Socket(connfd) 从此由 RAII 管）。
    return connfd;
}

// 【做什么】::shutdown(fd, SHUT_WR)：只关写方向，发一个 FIN 给对端，本端仍可继续读。
// 【流程】⑩：用户 conn->shutdown() → setState(kDisconnecting) → runInLoop(shutdownInLoop)
//            → 若 outputBuffer_ 已发空（channel 不在 writing 状态）才调到这里。
// 【线程】connfd 所属的 subLoop#N。
// 【设计】为什么是"半关闭"而不是直接 close：直接 close 会有两个后果 ——
//            1) 本进程 outputBuffer_ 里还没发出去的数据被丢弃；
//            2) 若内核接收队列里还有没读走的数据，close 发的是 RST 而不是 FIN，
//               对端正在进行的 read 立刻收到 ECONNRESET，看不见最后那段数据。
//            半关闭让"我说完了，但还在听你说"成为可能，客户端能正常读到 EOF(0)。
// 【易错】example/testserver.cc 的 onMessage 里那句被注释掉的 conn->shutdown()：
//            打开它，echo 服务器立刻变成"回一句就关"的短连接服务（⑩→⑪ 全流程走一遍），
//            对应 example/testserver.cc 的 onMessage 里被注释掉的 conn->shutdown()。
// 【面试】"close 和 shutdown 的区别？" —— close 是"这个 fd 我不要了"（引用计数减一，
//            可能不发 FIN，还会丢未读数据发 RST）；shutdown 是"这条连接的方向性语义改变"，
//            与 fd 引用计数无关，可以只关一个方向，还会把未发数据发完。
void Socket::shutdownWrite()
{
    // 【系统调用】::shutdown(fd, how)：how 可取 SHUT_RD（关读，丢弃接收队列并回 RST）、
    //            SHUT_WR（关写，发 FIN）、SHUT_RDWR。成功 0，失败 -1。
    //            这里用 < 0 判失败（因为它只可能返回 0 或 -1，和 bind/listen 同类）。
    //            常见 errno：ENOTCONN（连接已经断了/还没连上，重复 shutdown 时会遇到）、
    //            EBADF/ENOTSOCK、EINVAL（how 值非法）。
    // 【易错】shutdown 后 epoll 会怎样：对写端而言 EPOLLOUT 会一直就绪（"写不阻塞了"），
    //            对端则看到 EPOLLIN + read 返回 0。本项目在 ⑩ 里是"发完再 shutdown"，
    //            所以 shutdown 之后不存在还在等的 EPOLLOUT 注册（channel 已 disableWriting）。
    if (::shutdown(sockfd_, SHUT_WR) < 0)
    {
    // 【设计】这里不 LOG_FATAL 是对的：关闭写端失败不影响服务继续运行（连接本来也可能
    //            已经断了），只记录不致命。
    // 【易错】ENOTCONN 会走到这条日志，属于正常竞态（对端先断了），不是程序 bug；
    //            看到它不要急着去找 shutdown 的调用逻辑。
        LOG_ERROR("shutdownWrite error");
    }
}

// ── TCP_NODELAY：禁用 Nagle 算法，echo/RPC 类服务的必选项 ─────────────────────
// 【做什么】开/关 Nagle。on=true → optval=1 → 禁用 Nagle → 每次 write 立刻发包。
// 【谁调用】本仓库没有任何调用点（接口备而未用）。想给 connfd 开，得在 ⑥ TcpConnection
//            构造里加一句 socket_->setTcpNoDelay(true)（那里已经开了 setKeepAlive）。
// 【对比原版】原版 muduo 把这个开关上移成 TcpConnection 的公开接口（用户可以
//            conn->setTcpNoDelay(true)），低延迟服务一般就在连接建立阶段自己打开；
//            本项目 TcpConnection 构造只调了 setKeepAlive(true)，NODELAY 留给用户手动开，
//            而且 TcpConnection 没有对外暴露这个开关 —— 这是"接口面比原版窄"的一处。
//            面试口径：「框架提供 setTcpNoDelay，但示例 EchoServer 没开，我读代码时
//            发现这是个可以改进的点」。注意不要断言"原版默认就开着 NODELAY"，
//            本仓库不含原版源码，默认值这类细节以原版源码为准。
// 【线程】按调用点而定（若在 TcpConnection 构造里就是 subLoop#N）。
// 【流程】不参与 ①→⑫ 的具体某步，它是"⑧/⑩ 的延迟特性"的开关。
void Socket::setTcpNoDelay(bool on)
{
    // TCP_NODELAY 用于禁用 Nagle 算法。
    // Nagle 算法用于减少网络上传输的小数据包数量。
    // 将 TCP_NODELAY 设置为 1 可以禁用该算法，允许小数据包立即发送。
    // ── Nagle 算法到底在干什么（这段要读懂）────────────────────────────────
    // 【系统调用】规则：只要还有"已发出但还没被 ACK"的小于 MSS 的数据在网络上飞，
    //            新 write 的数据就不立刻发，先在内核发送缓冲区里攒着，直到
    //              a) 攒够一个 MSS（以太网典型 1448 字节 = 1500 MTU - 20 IP - 20 TCP），或
    //              b) 之前发出去的数据被全部 ACK（说明链路没在等确认，可以再发一段）。
    //            目的：避免"小包灾难"—— 1 字节的交互数据也发一个 40 字节头部的包，
    //            带宽利用率 2%，而且路由器/交换机要处理几十倍数量的包。
    // 【易错】40ms 延迟是怎么来的（Nagle 与 delayed ACK 互相等，面试高频）：
    //            接收端 TCP 默认不立刻回 ACK，而是等最多 40ms（Linux 上从 40ms 起，
    //            RFC 1122 允许上限 200ms；Linux 还有 quickack 模式可立刻确认），
    //            指望"捎带"下一个反向数据包一起确认。发送端 Nagle 又规定"没被 ACK
    //            就不能发新的小包"。于是 echo 这种一问一答的场景两边互等：
    //              客户端发 5 字节 → 服务端 Nagle 想攒但马上 write 回 5 字节 →
    //              服务端的 5 字节要等客户端 ACK 才能发 → 客户端的 ACK 要等 40ms 定时器
    //              → 于是每个来回多出最多 40ms 延迟。抓包上看就是"每条连接隔一会儿憋一下"。
    //            解法：任一端设 TCP_NODELAY（禁用 Nagle），或接收端打开 TCP_QUICKACK。
    // 【设计】为什么 echo 服务器/短消息 RPC/数据库必须禁用 Nagle：延迟敏感，包本来就只有
    //            几十字节，"省包"省不了多少，延迟却是硬伤。
    //            为什么文件传输/视频/大批量推送不要禁：带宽受限，小包会显著浪费网络，
    //            40ms 在连续数据流里根本不会触发（每次都立刻满足 MSS 条件）。
    //            更彻底的做法是在应用层攒批（写够一块再 send），这也是 muduo 用 outputBuffer_
    //            的另一个好处：sendInLoop 一次 write 就是一大块，Nagle 影响自然变小。
    int optval = on ? 1 : 0;
    // 【系统调用】::setsockopt(fd, level, optname, optval, optlen)：
    //            level 用 SOL_SOCKET（socket 层通用）或 IPPROTO_TCP（TCP 层选项）。
    //            optval 传的是"指针 加 长度"，内核把 sizeof(int) 字节拷走 —— 所以这里
    //            必须是 int 变量的地址，写 char 或 bool 会被内核按 4 字节解读（读越界栈垃圾）。
    //            成功 0 / 失败 -1。TCP_NODELAY 在 Linux 上任何时候都能设（包括连接已建立）。
    // 【⚠问题】返回值被完全丢弃：如果内核不认识这个选项（ENOPROTOOPT）、fd 不是 TCP
    //            socket（ENOTSOCK）、optlen 不对（EINVAL）、内存地址非法（EFAULT），
    //            程序不会有任何提示，你会以为"已经禁用 Nagle 了"，实际延迟还在 ——
    //            这类"配置没生效但没报错"的问题最难查（本文件四个 setXxx 全是这类问题）。
    //            正确姿势：拿返回值判负并 LOG_SYSERR 打印 errno；或者干脆用 getsockopt
    //            回读确认。四个 setter 同此问题，下面只提示一次。
    ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

// ── SO_REUSEADDR：解决"重启服务器 bind 同一个端口失败" ──────────────────────
// 【做什么】允许 ::bind 一个还处在 TIME_WAIT 的本地端口（不是"抢别人正在监听的端口"，见下）。
// 【流程】② Acceptor 构造里、bindAddress 之前设；【线程】mainLoop（只作用于 listenfd）。
// 【谁调用】② Acceptor 构造里 acceptSocket_.setReuseAddr(true)，位置在 bindAddress 之前
//            （顺序必须如此，本次 bind 才用得上）。【线程】mainLoop。
// 【系统调用】为什么需要它：服务器主动 close 的那一端会进 TIME_WAIT（Linux 固定 60 秒，
//            即 2MSL）。这期间"这个本地端口"上还挂着一个 2MSL 计时器的孤儿连接，
//            默认情况下内核不允许新 socket 再 bind 到同一个 addr:port，于是你
//            Ctrl-C 重启一次服务，就得到 EADDRINUSE，只能等一分钟。
//            TIME_WAIT 不能省掉的原因：一是让"这次连接的迟到重复段"在网络上自然消亡，
//            二是保证 2MSL 内四元组不会被新连接复用而收错包 —— 这是 TCP 可靠性的设计。
// 【易错】Linux 上 SO_REUSEADDR 语义比 Windows 保守得多：它只允许复用 TIME_WAIT 的端口，
//            绝不允许把"别人正在 LISTEN 的同一个 addr:port"抢过来（Windows 的
//            SO_REUSEADDR 可以劫持，脚本语言文档常照抄那句话，别被误导）。
//            原作者注释里写的"强制绑定到一个已被其他套接字使用的端口"就是这个不精确表述，
//            真正被解决的是"重启时上一个进程留下的 TIME_WAIT"，不是"端口被别人占着"。
// 【易错】反过来也有代价：开了它之后，同一个 addr:port 上"老进程残留的孤儿连接"和新连接
//            混在一起，若服务被重启两次且客户端复用同一四元组，迟到包有极小概率进错连接
//            （PAWS/时间戳机制在很大程度上兜住这件事）。
// 【面试】"EADDRINUSE 怎么排查？" —— ss -lntp 看是谁在 LISTEN（真被别人占），
//            ss -tan state time-wait 看是不是 TIME_WAIT（该开 SO_REUSEADDR）。
void Socket::setReuseAddr(bool on)
{
    // SO_REUSEADDR 允许一个套接字强制绑定到一个已被其他套接字使用的端口。
    // 这对于需要重启并绑定到相同端口的服务器应用程序非常有用。
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

// ── SO_REUSEPORT：解决惊群 与 多进程/多线程负载均衡 ─────────────────────────
// 【做什么】允许多个"活的"socket 绑完全相同的 IP:端口，各自拥有独立的 accept 队列；
//            内核按 (源IP, 源端口) 做哈希，把新连接固定地散到其中一个 socket 上。
// 【流程】② Acceptor 构造里、bindAddress 之前设；【线程】mainLoop（只作用于 listenfd）。
void Socket::setReusePort(bool on)
{
    // SO_REUSEPORT 允许同一主机上的多个套接字绑定到相同的端口号。
    // 这对于在多个线程或进程之间负载均衡传入连接非常有用。
    // 【系统调用】SO_REUSEPORT 需要 Linux 3.9 以上（glibc 也要有该常量），
    //            老内核上 setsockopt 直接返回 ENOPROTOOPT —— 而本项目不检查返回值，
    //            所以会出现"代码里开了，其实没开"的情况（这正是上面那条 ⚠问题 的活样本）。
    // 【易错】它解决的两件事要分清：
    //            1) 惊群（thundering herd）：多个线程/进程在同一个 listenfd 上等事件，
    //               一个连接到达时全部被唤醒，但只有一个能 accept 成功，其余拿到 EAGAIN，
    //               白烧 CPU 还可能引起调度抖动。Linux 的 epoll LT 对同一 fd 只唤醒一个
    //               等待者，已经缓解了同进程内的惊群，但"多进程共用一个 listenfd"仍是问题。
    //               SO_REUSEPORT 让每个进程各有一个 listenfd 和自己的 accept 队列，
    //               内核在 SYN-ACK 阶段就把连接选定了归属 —— 天然不惊群，也不互相抢锁
    //               （accept 队列锁争用消失，这是它在高并发下最实际的收益）。
    //            2) 负载均衡：哈希是按流（四元组）做的，同一客户端的新连接倾向落到
    //               同一个 socket/线程，缓存局部性好；nginx 的 reuseport 就是这个用法。
    // 【与 SO_REUSEADDR 的区别（面试常问）】REUSEADDR 对付"TIME_WAIT 端口"，
    //            REUSEPORT 对付"多个活跃 socket 抢同一个 addr:port"；两者可以叠加使用。
    // 【易错】开了 REUSEPORT 有个危险副作用：如果两个不同的程序（或同一程序的两个实例，
    //            比如忘了端口占用就重启）都绑同一端口并都开了 REUSEPORT，内核会安静地
    //            把流量对半分给它们，不报任何错 —— 表现成"连接时好时坏"的灵异现象。
    //            灰度/多实例部署时必须明确这是特性还是事故。
    // 【对比原版】原版 Acceptor 构造里只有 reuseport=true 时才 setReusePort(true)，
    //            由 TcpServer 的 kReusePort 选项控制；本项目的 Acceptor 有 reuseport
    //            形参但无条件设 true（参数被忽略了，见 Acceptor.cc 的说明）。
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

// ── SO_KEEPALIVE：让内核替我们做"心跳探测"（但默认参数慢得离谱）───────────────
// 【做什么】开启后内核会在连接长时间空闲后自发往对端发探测包，连续失败就判定连接死亡，
//            关闭它并让 epoll 报 EPOLLHUP/EPOLLERR，于是 ⑧ 的 read 会拿到错误、走 ⑪。
// 【流程】⑥ TcpConnection 构造里对每个 connfd 设一次；【线程】subLoop#N。
// 【谁调用】⑥ TcpConnection 构造里 socket_->setKeepAlive(true) —— 所以本项目每条连接
//            都开了；【线程】subLoop#N（newConnection 时）。
// 【系统调用】三个可调的内核参数（procfs 全局值，也可用 TCP_KEEPIDLE/KEEPINTVL/KEEPCNT
//            对单个 socket 覆盖）：
//              net.ipv4.tcp_keepalive_time  = 7200 秒（空闲 2 小时后才开始第一个探测）
//              net.ipv4.tcp_keepalive_intvl =   75 秒（探测间隔）
//              net.ipv4.tcp_keepalive_probes =   9 个（失败次数上限）
//            所以对端"拔网线/断电"这种没有 FIN 的死亡，最坏要 2 小时 + 9 乘 75 秒
//            ≈ 2 小时 11 分钟才能被内核发现。这段时间里：连接占着 fd、内存、
//            TcpConnection 对象、还有 muduo 内部 connections_ map 的一项。
// 【设计】因此生产上的通用做法是"业务层心跳"而不是靠 SO_KEEPALIVE：
//            应用自己定一个更短的超时（比如 15 秒没收到心跳就 shutdown），
//            muduo 的 HeartBeat 示例就是这条思路；SO_KEEPALIVE 当作"兜底"开着就好。
// 【易错】SO_KEEPALIVE 只在"空闲"时起作用：如果链路上一直有数据在走，内核认为连接是活的，
//            不发探测；半开的连接（对端已崩，但我们还在写）则靠 send 失败/EPIPE 暴露。
// 【面试】"怎么发现死连接？" —— 三层答案：TCP 层 keepalive（慢）、应用心跳（可控）、
//            读事件返回 0 或 EPOLLHUP（快但依赖对端配合发 FIN/RST）。
void Socket::setKeepAlive(bool on)
{
    // SO_KEEPALIVE 启用在已连接的套接字上定期传输消息。
    // 如果另一端没有响应，则认为连接已断开并关闭。
    // 这对于检测网络中失效的对等方非常有用。
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}