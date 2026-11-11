/* ===========================================================================
 * 【文件】include/Socket.h
 * 【一句话】给一个裸的 socket fd 套上"析构即 close"的所有权壳，并把 bind / listen /
 *          accept / shutdown / 四个 setsockopt 选项包装成成员函数。
 * 【Reactor 角色】基础设施 / OS 封装层 —— Channel、Acceptor、TcpConnection 底下那一层，
 *          它是"资源拥有者"（拥有 fd），Channel 只是"事件的引用者"（不拥有 fd）。
 * 【依赖】noncopyable.h（禁拷贝基类）；实现文件里还要 InetAddress.h（bind/accept 的地址）
 *          和 Logger.h（失败时打日志）。这里只前置声明 class InetAddress，不 include，
 *          因为头文件里只用它的引用/指针，不需要完整类型。
 * 【谁用它】② Acceptor::acceptSocket_（值成员，包住 listenfd）；⑤ accept 出 connfd；
 *          ⑥/⑦ TcpConnection::socket_（unique_ptr<Socket>，包住 connfd）；
 *          ⑪ TcpConnection 析构时才真正 close(connfd)；⑩/⑪ shutdownWrite。
 *          fd() 被 Channel 构造拿去注册 epoll。
 * 【先修知识】1) fd 是"进程全局小整数"，不是对象指针，会被复用；2) RAII：资源获取即初始化，
 *            离开作用域自动释放；3) listenfd 与 connfd 的区别；4) 为什么句柄不能拷贝。
 * 【难度】★★☆☆☆（接口简单，语义陷阱全在"谁拥有 fd"）  【建议耗时】20 分钟
 * 【自检】a) Socket 为什么必须 noncopyable？不写会发生什么具体事故？
 *        b) sockfd_ 为什么是 const int？它顺带禁掉了哪些操作？
 *        c) ⑪ 销毁链里，close(connfd) 发生在哪一步、在哪个线程？为什么必须这么晚？
 * ========================================================================= */

#pragma once

// 【依赖】noncopyable：只有 private/protected 的 =delete 拷贝构造与赋值（见该文件），
//          Socket 继承它之后就再也拷不出第二份 fd。这是本类存在的第一条理由。
#include "noncopyable.h"

class InetAddress;
// 【语法】前置声明（forward declaration）而不是 include：本头文件里 InetAddress 只以
//          const InetAddress& 与 InetAddress* 出现，编译器不需要知道它的大小和成员，
//          所以一句声明就够。好处：改 InetAddress.h 不会让所有 include Socket.h 的文件
//          重新编译（编译防火墙）；代价：用 Socket.h 的文件若要真的构造 InetAddress，
//          得自己 include InetAddress.h（src/Socket.cc 就是这么做的）。

// ── Socket = fd 的 RAII 拥有者 ──────────────────────────────────────────────
// 【设计】这个类只做两件事：1) 把"一个 fd + 它的关闭责任"绑在一起（析构里 ::close）；
//          2) 把只有 fd 才能调的内核选项（bind/listen/accept4/shutdown/setsockopt）
//          变成带类型的成员函数。它不管事件、不管读写数据、不管地址 —— 数据面在 Buffer，
//          事件面在 Channel，控制面（连接生命周期）在 TcpConnection。分层就卡在这儿。
// 【设计】为什么 noncopyable 加 析构 close 就是 RAII：fd 是纯 int，编译器给的默认拷贝
//          构造会把整数复制一份，于是两个 Socket 对象都认为自己拥有同一个 fd，
//          两个析构各 close 一次 = double close。危险不只是"第二次返回 EBADF"：
//          在两次 close 之间，另一个线程可能 accept 到一个新连接，内核把刚释放的
//          fd 号重新分配给它（fd 总是取当前最小可用值）—— 第二次 close 就会关掉这条
//          无辜的新连接。这就是"句柄型资源必须禁拷贝或引用计数"的经典事故模型。
//          本项目靠 unique_ptr<Socket>（TcpConnection）与值成员（Acceptor）保证唯一所有权，
//          不需要引用计数；原版 muduo 更进一步用 shared_ptr<Socket> 加 moveOne()。
// 【流程】② 出生（listenfd）；⑤ 生出 connfd 的 Socket（在 TcpConnection 构造里 new Socket(sockfd)）；
//          ⑪ 死亡（TcpConnection 引用计数归零）。
// 【线程】acceptSocket_ 一生都在 mainLoop；每个 connfd 的 Socket 一生都在它所属的 subLoop#N，
//          且创建与销毁都发生在 subLoop#N 的回调里（见 ⑥/⑪ 的三次线程跳转）。
// 【易错】close 之前必须已经把该 fd 从 epoll 里摘掉（Channel::disableAll + remove）。
//          否则：fd 号被复用后 epoll 里还挂着一条老注册，新连接的事件会被投递给老 Channel
//          —— 事件串台，现场极难复现。⑪ 的销毁链正是按这个顺序排的，不是随便写的。
// 【对比原版】原版类名 muduo::net::sockets::Socket，成员函数更多（connect、bindService、
//          getLocalAddr/getPeerAddr、setSendBufSize/setRecvBufSize、timeout 等），
//          且把"地址解析、fd 创建"放在 sockets 命名空间的自由函数里；本项目只留服务端最小集。
// 封装socket fd
class Socket : noncopyable
{
public:
    // 【做什么】"我接手这个 fd"。构造函数里只存一个 int，不调任何系统调用。
    // 【谁调用】② Acceptor::Acceptor 的初始化列表 acceptSocket_(createNonblocking())
    //            （静态函数里 socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, IPPROTO_TCP)
    //            拿到 listenfd，构造本身不负责创建，也不负责设非阻塞）；
    //            ⑥ TcpConnection 构造里 new Socket(sockfd)，sockfd 就是 ⑤ accept4 返回的 connfd。
    // 【线程】前者在 mainLoop，后者在 subLoop#N（connectEstablished 之前，newConnection 里）。
    // 【设计】"创建"与"接管"分开写：fd 的创建失败要 LOG_FATAL，而构造 Socket 不该失败，
    //            所以构造极轻、不抛异常，异常安全上很干净（对象一旦存在就必然会被析构 close）。
    // 【语法】explicit：没有它，任何 int 都能隐式变成 Socket，
    //            比如写 socket_.fd() == someInt 这种比较时编译器会造出一个临时 Socket
    //            并立刻把 someInt 当成自己的 fd 给 close 掉 —— 一个隐蔽到可怕的 bug。
    //            所以凡是"单参数构造 + 持有资源"的类，explicit 几乎是强制的。
    explicit Socket(int sockfd)
        : sockfd_(sockfd)
    {
    }
    // 【做什么】::close(sockfd_)，唯一一句资源释放（定义在 src/Socket.cc）。
    // 【流程】⑪ 链路终点：TcpConnection 引用计数归零 → ~TcpConnection → unique_ptr 成员
    //            socket_ 析构 → 这里才真正关闭 connfd；进程退出时 Acceptor 的 listenfd 也走这里。
    // 【线程】在 TcpConnection 所属的 subLoop#N 里执行（最后一个 shared_ptr 是在 ⑪ 的
    //            connectDestroyed 回调栈上释放的）—— 这是"对象在哪个 loop 创建就在哪个 loop 销毁"
    //            原则的落地点，也正是 TcpServer 析构里要绕那么一圈 reset 的原因。
    // 【设计】析构是 virtual 吗？不是（本类没有虚函数，也没打算被继承），
    //            靠 unique_ptr 记录的准确类型释放，安全。
    // 【易错】close 会释放文件描述符但不会发 FIN 的完整语义：如果接收缓冲区还有没读走的
    //            数据，close 发的是 RST 而不是 FIN，对端正在读的连接会收到 ECONNRESET。
    //            想优雅关断要调用 shutdownWrite（⑩ 走的就是它）。
    ~Socket();

    // 【做什么】把裸 fd 交出去给"只看不管"的人用：Channel 构造要它、EPollPoller 的
    //            channels_ map 用它当 key、日志里打印它、TcpConnection::sendInLoop 里
    //            直接 ::write(channel_->fd(), ...)。
    // 【谁调用】② acceptChannel_(loop, acceptSocket_.fd())；⑥ Channel(loop, sockfd)；
    //            ⑧⑨⑩ 经由 Channel::fd() 间接使用。
    // 【设计】返回 int 而不是 const Socket*，因为 fd 就是个值，交出去也不会导致所有权转移 ——
    //            所有权只在"谁析构 Socket"上，谁拿到这个 int 都不许 close 它。
    // 【易错】拿到 fd 的人必须明白自己不拥有它：Acceptor::handleRead 里若没有回调
    //            （NewConnectionCallback_ 为空）就直接 ::close(connfd)，那是关"还没交给
    //            Socket 的裸 fd"，此时还没有任何 Socket 对象拥有它，所以不违反这条规则；
    //            一旦包成 Socket 就不能再手动 close 了。
    int fd() const { return sockfd_; }
    // 【做什么】::bind，把 listenfd 与"IP:端口"绑死。参数 const& 表示只读借用地址对象。
    // 【谁调用】② Acceptor 构造（在 setReuseAddr/setReusePort 之后 —— 顺序有讲究，
    //            SO_REUSEADDR 必须在 bind 之前设才对本次 bind 生效）。
    // 【线程】mainLoop。【失败会怎样】LOG_FATAL 直接 exit(-1)，进程自杀（详见 .cc）。
    void bindAddress(const InetAddress &localaddr);
    // 【做什么】::listen，把"主动 socket"变成"被动 socket"：内核开始建半连接/全连接队列。
    // 【谁调用】③ Acceptor::listen（由 TcpServer::start 通过 loop_->runInLoop 投递到 mainLoop）。
    // 【线程】mainLoop。【流程】listen 之后紧接着 acceptChannel_.enableReading() 才会真正
    //            被 epoll 关注；顺序反了就会漏掉最早的那几个 SYN（事件不会补发）。
    void listen();
    // 【做什么】::accept4 拿一个新连接：返回 connfd，并把对端地址写进 *peeraddr。
    // 【谁调用】⑤ Acceptor::handleRead（listenfd 上 EPOLLIN 之后，mainLoop）。
    // 【线程】mainLoop（单 mainReactor 模型下所有 accept 都在主线程）。
    // 【设计】返回裸 int 而不是 Socket：因为所有权要交给调用方去"包装"，
    //            Acceptor 拿到 connfd 后通过 NewConnectionCallback_(connfd, peerAddr)
    //            交给 TcpServer::newConnection，最终在 subLoop 里 new Socket(connfd)。
    //            这样"fd 从哪个线程生、由哪个线程管"这条链路是明写出来的。
    // 【易错】peeraddr 是出参且没判空：调用方必须传有效指针（本项目 Acceptor 传 &peerAddr，
    //            没传 nullptr 的情况；原版 muduo 允许传 0 表示不关心对端地址）。
    int accept(InetAddress *peeraddr);

    // 【做什么】::shutdown(fd, SHUT_WR) —— 只关写半边：向对端发 FIN，本端仍可继续读。
    // 【谁调用】⑩ TcpConnection::shutdownInLoop（用户 conn->shutdown() 之后，
    //            且等 outputBuffer_ 里的数据全部发完之后才敢调）。
    // 【线程】connfd 所属的 subLoop#N。
    // 【设计】为什么不是 close：close 会把没发完的数据丢掉、还可能发 RST；
    //            半关闭让"我说完了但还在听"成为可能，这是 echo/HTTP keep-alive 正确收尾的基础。
    //            对端 read 到 0 就知道我们写完了。
    void shutdownWrite();

    // ── 四个选项 setter 的共同点（细节在 src/Socket.cc 每个函数旁边）──────────────
    // 【设计】四个函数形状完全一样：int optval = on ? 1 : 0; setsockopt(fd, 层, 选项, &optval, sizeof)；
    //            全部通过 ::setsockopt 修改"内核里这个 fd 的属性"，不改变程序内任何状态。
    // 【谁调用】② Acceptor 构造里 setReuseAddr(true) + setReusePort(true)（mainLoop，作用于 listenfd）；
    //            ⑥ TcpConnection 构造里 socket_->setKeepAlive(true)（subLoop#N，作用于每个 connfd）；
    //            setTcpNoDelay 在本仓库没有任何调用点（备而未用，见 .cc 说明）。
    // 【易错】调用时机：SO_REUSEADDR/SO_REUSEPORT 必须在 bind 之前；TCP_NODELAY 任何时候
    //            （包括连接已建立后）都能设；SO_KEEPALIVE 对已经空闲的连接也立刻生效。
    // 【⚠问题】四个函数的 setsockopt 返回值全被丢弃，选项没设上不会有任何提示（详见 .cc）。
    void setTcpNoDelay(bool on);
    // 【做什么】SO_REUSEADDR：允许 bind 一个还处在 TIME_WAIT 的本地端口。解决重启失败。
    // 【流程】② Acceptor 构造（必须在 bindAddress 之前）；【线程】mainLoop，作用于 listenfd。
    void setReuseAddr(bool on);
    // 【做什么】SO_REUSEPORT：允许多个 socket 绑同一个 IP:端口，各自有独立 accept 队列。
    //            解决惊群 与 多进程/多线程负载均衡。需要 Linux 3.9 以上。
    // 【流程】② Acceptor 构造（bind 之前）；【线程】mainLoop，作用于 listenfd。
    void setReusePort(bool on);
    // 【做什么】SO_KEEPALIVE：让内核在连接空闲很久后自发探测对端是否还活着。
    //            默认参数极慢（2 小时才开始），详见 .cc 里三个 proc 参数。
    // 【流程】⑥ TcpConnection 构造（connfd 刚被接管时就设）；【线程】subLoop#N，作用于 connfd。
    void setKeepAlive(bool on);

private:
    // 【设计】为什么是 const int：
    //            1) 语义：这个对象从生到死代表的就是这一个 fd，不许"换一个 fd 继续用"。
    //               一旦能改，close 的时机、epoll 里的注册、Channel 里的 fd 就全对不上了。
    //            2) 语法强制：const 成员只能在构造初始化列表里赋值，因此"赋值即接管"
    //               这条唯一路径由编译器保证，别人写不出 socket_ = otherFd 这种代码。
    //            3) 副作用（很多人不知道）：含 const 非静态数据成员的类，隐式拷贝赋值与
    //               隐式移动构造/移动赋值都不可用（const 对象没法赋值）—— 等于在
    //               noncopyable 之外又加了一道保险，所以 TcpConnection 只能用
    //               unique_ptr<Socket> 传所有权，不能 Socket a = std::move(b)。
    // 【易错】不要把"const 成员"和"const 成员函数"搞混：这里的 const 修饰 int 成员；
    //            fd() const 后面的 const 修饰的是 this。
    // 【对比原版】原版是 int fd_（非 const），并提供 sockets::shutdownWrite、
    //            sockets::getSoKeepAlive 等自由函数，TcpConnection 里还缓存了
    //            local/peer 地址字符串；本项目把一切收到最小。
    const int sockfd_;
};
