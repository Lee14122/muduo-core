/* ===========================================================================
 * 【文件】include/InetAddress.h
 * 【一句话】把内核用的 struct sockaddr_in（16 字节的 IPv4 地址+端口）包成一个能构造、
 *          能打印、能和内核双向转换的值类型，让上层把"网络地址"当普通对象传来传去。
 * 【Reactor 角色】基础设施 / OS 封装层。纯数据：不含 fd、不含线程、不碰事件循环。
 * 【依赖】<netinet/in.h> 提供 sockaddr_in / in_addr / in_port_t / AF_INET；
 *        <arpa/inet.h> 提供 inet_addr / inet_ntop / htons / ntohs（在 .cc 里用）；
 *        <string> 提供 ip 构造参数与 toIp()/toIpPort() 的返回值。
 * 【谁用它】① main() 里 InetAddress addr(8080) 造监听地址；② TcpServer/Acceptor 把它
 *          当 const InetAddress& 传给 Socket::bindAddress；⑤ Acceptor::handleRead 先造一个
 *          空的 peerAddr 交给 Socket::accept 用 setSockAddr 填；⑥⑦⑪ TcpConnection 把它当
 *          const 值成员 localAddr_/peerAddr_ 存着，用户在 onConnection 里 toIpPort() 打日志。
 * 【先修知识】1) TCP 四元组；2) sockaddr（通用地址）与 sockaddr_in（IPv4 具体地址）靠强转
 *            打交道；3) 网络字节序是大端；4) 只装值的类默认可拷贝，不需要 noncopyable。
 * 【难度】★★☆☆☆   【建议耗时】20 分钟
 * 【自检】a) 为什么端口要 ::htons，IP 却用 ::inet_addr？两者返回的字节序分别是什么？
 *        b) 这个类为什么同时要 getSockAddr()（只读出口）和 setSockAddr()（受控入口）？
 *           少了 setSockAddr，⑤ 这一步会怎么写得难受？
 *        c) 两个 explicit 构造分别对应"我监听的地址"和"对端来的地址"，调用时机各在链路哪一步？
 * ========================================================================= */

#pragma once

// 【依赖】arpa/inet.h：地址"文本格式 与 二进制格式"互转的工具集（inet_addr / inet_ntoa /
//          inet_pton / inet_ntop），顺带把 htons / ntohs / htonl / ntohl 一起带进来。
//          这些函数全部是无状态的用户态计算：不加锁、不进内核、不阻塞，任何线程随便调。
// 【易错】inet_ntop / inet_pton 里的 n 与 p：ntop = numeric-to-presentation（二进制 变成
//          人看的点分十进制），pton = presentation-to-numeric（人看的 变成 二进制）。
//          本项目"写入用老的 inet_addr，读出用新的 inet_ntop"，是个不太一致的退化。
#include <arpa/inet.h>
#include <netinet/in.h>

// 【依赖】sockaddr_in 的真身在 netinet/in.h，不在 sys/socket.h（POSIX 分工：socket.h 只给
//          通用的 sockaddr）。它的 16 字节布局（x86-64 Linux）：
//            sa_family_t sin_family;     //  2B 地址族，IPv4 填 AF_INET（值为 2），内核靠它认类型
//            in_port_t   sin_port;       //  2B 端口号，必须存"网络字节序"（大端）
//            struct in_addr sin_addr;    //  4B IPv4 地址，里面只有一个 in_addr_t s_addr，大端
//            char        sin_zero[8];    //  8B 纯填充，只为把结构撑到和 sockaddr 一样大，必须置 0
//          sizeof(sockaddr_in) == 16 == sizeof(sockaddr)，这是二者能互相强转的前提。
//          sockaddr 只有 sa_family 加 14 字节 char sa_data[14]，这 14 字节怎么解释完全由
//          sa_family 决定；内核 API（bind / accept / connect / getsockname）签名一律收
//          sockaddr*，所以我们必须随时能交出 &addr_ 并把它当成 (sockaddr*)。
// 【面试】"sockaddr 和 sockaddr_in 什么关系？" —— 通用 与 具体；前者是内核 API 的统一
//          门面（靠 sa_family 做运行时类型判断，相当于手写的 tagged union），后者是 IPv4 的
//          解释方式；强转不改变内存，只改变编译器怎么看这 16 字节。
#include <string>
// 【对比原版】原版这里靠 const std::string& 用地址串，本项目把 ip 写成按值 std::string，
//          所以头文件必须真的带 <string>（不然声明都编译不过）。

// ── InetAddress 是"地址的值类型"，不是"socket" ────────────────────────────────
// 【设计】它不持有 fd，所以：1) 没必要继承 noncopyable，拷贝和赋值都是 16 字节 memcpy，
//          按值传参完全安全（TcpServer、TcpConnection 就是各存了一份 const 值成员）；
//          2) 不需要写析构函数，没有任何资源要归还。要对比的是 Socket：那个类管一个 fd 的
//          生命周期，必须 noncopyable，否则两份对象各 close 一次就是 double close，
//          可能把别人刚拿到的同号 fd 关掉（同一个仓库里两种"封装内核对象"的取舍）。
// 【流程】①② 提供监听地址；⑤ 承载 accept 回来的对端地址；⑦⑪ 只服务连接日志。
// 【线程】本身不属于任何线程，谁调用就在谁的线程里跑；实际调用点：mainLoop（造监听地址、
//          Acceptor::handleRead 填 peerAddr）与 subLoop#N（onConnection 打印 peerAddress）。
// 【易错】它不是 const 快照：getSockAddr 与 setSockAddr 并发使用没有任何锁。本项目靠
//          "一个 InetAddress 对象只被它所在 loop 的线程碰"这条约定保安全
//          （TcpConnection 里干脆把它们声明成 const 成员，从编译期掐死并发改的可能）。
// 【对比原版】原版还有 sockaddr_storage 版本与 IPv6 支持（toIp 里判 AF_INET6，
//          用 inet_ntop(AF_INET6,...)），并且提供 getAddrInfo 解析域名；本教学版只留 IPv4 最小集。
// 封装socket地址类型
class InetAddress
{
public:
    // 【做什么】构造"本端监听地址"（服务器打算用哪个 IP 加哪个端口）；定义在 src/InetAddress.cc。
    // 【谁调用】example/testserver.cc 的 main 里 InetAddress addr(8080)（①）；② 用户把
    //            造好的监听地址以 const InetAddress& 传给 TcpServer 构造 —— 注意它的
    //            listenAddr 参数没有默认值，必须由调用方显式传入（只有末尾 Option 参数
    //            有默认值 kNoReusePort），TcpServer 再把它原样转交给 Acceptor 去 bind；
    //            另外 Acceptor::handleRead 里那句 InetAddress peerAddr 也走本构造（⑤，
    //            靠两个参数的默认值造一个待填的占位对象）。
    // 【线程】mainLoop（主线程）。只在启动阶段跑，一次性的，怎么费都不影响性能。
    // 【流程】① main 里造监听地址 → ② 交给 TcpServer/Acceptor 去 bind；它不参与 ⑤ 之后
    //            的链路（对端地址走 setSockAddr）。
    // 【语法】explicit 关掉"单参数构造可以做隐式转换"这条 C++ 规则。去掉它会怎样：
    //            Socket::bindAddress(8080) 会被编译器悄悄补一个"用 int 造临时 InetAddress"，
    //            端口和 IP 参数写反这种致命错也能编译通过。explicit 把
    //            "必须写明我是有意构造一个地址对象"这件事交给编译器检查。
    // 【语法】两个参数都有默认值，所以 InetAddress addr; 也是合法的（port=0，ip=127.0.0.1）。
    // 【易错】port=0 的内核语义是"随便给我一个空闲端口"，但 bind 之后必须用 getsockname
    //            把内核选中的端口读回来才知道连哪儿；本项目没有这条回填逻辑，
    //            所以在这个仓库里 port=0 只能当占位，别指望它真能对外服务。
    // 【设计】ip 按值收 std::string：调用点写 "127.0.0.1" 字面量要构造临时 string，
    //            按值接住再用最直白；不在热路径，多一次堆分配无所谓。
    // 【对比原版】原版 muduo 的构造重载更多（能区分"只监听回环"与"监听所有网卡"、
    //            能接管 sockaddr_storage 以支持 IPv6），并提供 InetAddress::resolve()
    //            走 getaddrinfo 做域名解析；本教学版只留 IPv4 最小集，且默认 IP 是 127.0.0.1。
    //            （原版具体有哪些重载，以原版 InetAddress.h 为准 —— 本仓库里没有它的源码，
    //            下面的说法只描述本项目，别把两者混起来背。）
    //            【易错】本项目"没有无参默认构造"这个东西：前面说的 InetAddress addr;
    //            能编译，靠的是 (port, ip) 两个参数各自带默认值，不是存在一个默认构造函数；
    //            TcpServer 也没有用任何"默认地址"，监听地址一定由用户传进来。
    explicit InetAddress(uint16_t port = 0, std::string ip = "127.0.0.1");
    // 【做什么】"接管"一个内核已经填好的裸 sockaddr_in：地址从内核格式进入我们的对象格式。
    // 【谁调用】任何手上已经有 sockaddr_in 的地方。注意本项目的热路径（⑤ accept）实际用的是
    //            setSockAddr 而不是这个构造（因为 Acceptor 先声明了 peerAddr 再交给 accept 填）。
    //            留着它的价值是把"地址有两种来源"写进接口：人写的走 port/ip 构造，
    //            内核填的走 sockaddr_in 构造。
    // 【语法】: addr_(addr) 是结构体整体赋值（16 字节拷贝），不调系统调用、不分配堆、
    //            不碰 fd，因此这个 inline 构造函数零副作用、不抛异常、不需要析构配合。
    // 【语法】参数用 const sockaddr_in& 而不是按值：省一次拷贝；语义是"我只读你，不改你"。
    // 【易错】两个构造都 explicit，所以 InetAddress a = someSockaddrIn; 编译不过，
    //            必须写 InetAddress a(someSockaddrIn); —— 这正是我们要的"显式转换"。
    // 【流程】⑤ 的"内核格式 变成 对象格式"这一步（本项目实际由 setSockAddr 完成，
    //            本构造留给"手上已经有 sockaddr_in"的调用者，例如以后加 getsockname 回填）。
    // 【线程】⑤ 在 mainLoop；若上层把对端地址复制给 subLoop（TcpConnection 的 peerAddr_
    //            值成员就是复制过来的），构造发生在 subLoop#N 的 newConnection 里。
    explicit InetAddress(const sockaddr_in &addr)
        : addr_(addr)
    {
    }

    // 【做什么】把 sin_addr 打印成点分十进制串，例如 "127.0.0.1"。定义在 src/InetAddress.cc。
    // 【谁调用】日志与诊断代码；本项目上层实际用得多的是 toIpPort()。
    // 【线程】调用者线程（mainLoop 打启动日志；subLoop#N 打连接日志）。
    // 【流程】不在 ⑧⑨⑩ 数据通路上，只服务 ②（打印监听地址）和 ⑥⑦⑪（打印对端）。
    // 【设计】返回值按 std::string 传出，调用方要么接住、要么 .c_str() 塞给日志宏，
    //            每次都有一次堆分配：禁止在每包必跑的 onMessage（⑧）里调它。
    std::string toIp() const;
    // 【做什么】"IP:端口"串，例如 "127.0.0.1:8080"；端口先 ::ntohs 还原成主机数值再格式化。
    // 【谁调用】example/testserver.cc 的 onConnection 里 conn->peerAddress().toIpPort()（⑦ 上线、
    //            ⑪ 下线）；TcpServer/TcpConnection 构造与析构日志里 localAddress().toIpPort()（②）。
    // 【线程】②/⑥ 阶段在 mainLoop，⑦/⑪ 的连接日志在 subLoop#N；函数无状态，可并发调用。
    // 【易错】它内部在同一块栈 buf 上先写 IP、再用 strlen 找回串尾、再用 sprintf 往后拼
    //            ":port"，看着危险，安全性论证写在 src/InetAddress.cc 的实现旁边。
    // 【面试】"muduo 的连接日志为什么长这样：127.0.0.1:52344 到 127.0.0.1:8080"？
    //            因为服务端手上只有 accept 填回来的对端地址（对端 IP 加对端的临时端口）和
    //            本机监听地址，四元组两边各调一次 toIpPort 就拼出来了。
    std::string toIpPort() const;
    // 【做什么】拿"人类数值"的端口：把大端存的 sin_port 转回主机字节序。
    // 【谁调用】上层判断或打印端口；本项目调用很少（example 里没用）。
    // 【线程】调用者线程。【流程】诊断辅助，不在 ①→⑫ 关键路径。
    // 【易错】别直接读 getSockAddr()->sin_port：那是大端裸值，8080 会读成 36895
    //            （字节序怎么变的推导见 src/InetAddress.cc 构造函数旁边）。
    uint16_t toPort() const;

    // 【做什么】把内部地址"借出去"给内核 API 当入参 —— 只读出口。
    // 【谁调用】Socket::bindAddress 里 (sockaddr*)localaddr.getSockAddr() 交给 ::bind（②）。
    // 【设计】为什么返回 const 指针（三件事一次说全）：
    //            1) ::bind 只需要读这块内存，给它可写指针等于邀请内核或别人改我的对象；
    //            2) 返回 const 让外部无法绕过类直接改 sin_port/sin_family，地址的写路径
    //               被收束到唯一入口 setSockAddr，类的不变量（memset 干净、family 正确、
    //               填充区为 0）就不会被悄悄破坏；
    //            3) 返回指针而不是值，让内核直接对我这块内存读写，省一次拷贝，也保证
    //               ::bind 看到的地址和我交出去的是同一份（不会悬垂在临时对象上）。
    // 【语法】函数名后面的 const 修饰 this（成员函数不许改对象），因此 TcpConnection 里
    //            const InetAddress localAddr_ 也调得动它；返回类型是"指向 const sockaddr_in 的指针"，
    //            意思是"不能通过这支指针改地址"，不是"指针本身是常量"。
    // 【易错】返回的裸指针不拥有对象：把 InetAddress 放进临时量再留存这个指针就是悬垂。
    //            本项目所有调用点都是"当场用完就扔"（直接传给 ::bind），所以安全。
    const sockaddr_in *getSockAddr() const { return &addr_; }
    // 【做什么】地址的写入口：整体覆盖 addr_（16 字节赋值）。
    // 【谁调用】Socket::accept（⑤）：::accept4 把对端地址写进一个局部 sockaddr_in，然后
    //            peeraddr->setSockAddr(addr) 把它搬进 InetAddress 对象。
    // 【设计】这一对 get/set 就是"和 ::accept 怎么配合"的答案：
    //            ::accept(sockfd, (sockaddr*)&addr, &len) 要求调用者交出一块 sockaddr 大小的
    //            内存当出参，并且必须把 len 事先设成这块内存的真实大小。我们不该让内核直接
    //            写类的内部（类型不同，而且内核可能截断或部分写），于是流程是：
    //              裸 sockaddr_in addr（可写，交给内核）→ 内核填 → setSockAddr 拷进对象（受控写）。
    //            有了 const 出口 get 加受控入口 set，类的其余部分看到的永远是"一个合法 sockaddr_in"。
    // 【易错】len 必须等于结构真实大小：传小了内核会截断地址并回报"地址被截"（对端信息不全），
    //            传大了越界写。这里 sizeof(sockaddr_in)=16 与结构等大所以正确；
    //            将来若支持 IPv6 必须换成 sockaddr_storage，否则同样写法直接越界。
    // 【对比原版】原版 muduo 的 Socket::accept 直接 return InetAddress(acceptAddr)（用带 const
    //            sockaddr_in& 的构造函数），压根没有 setSockAddr；本项目加 set 是为了配合
    //            "Acceptor 先声明空 peerAddr，再让 accept 填"这种写法。
    // 【流程】⑤ Acceptor::handleRead → Socket::accept(::accept4 填裸结构) → 本函数 →
    //            ⑥ NewConnectionCallback_(connfd, peerAddr) 把它一路带进 subLoop。
    // 【线程】mainLoop（accept 只发生在主线程）；对象被拷进 TcpConnection 的值成员后，
    //            后续读取发生在 subLoop#N，两者不再是同一块内存，因此没有跨线程写。
    void setSockAddr(const sockaddr_in &addr) { addr_ = addr; }

private:
    // 【设计】唯一成员：16 字节纯值。不是指针、不在堆上、不含 fd —— 所以这个类不需要
    //            析构函数、不需要 noncopyable、不需要智能指针，一切默认行为都对。
    // 【易错】它没有类内默认初始值，全靠构造函数里的 ::memset 清零。如果哪天有人加一个
    //            "不 memset 的构造函数"，sin_zero 填充区和未赋值的字节就成了栈垃圾；
    //            内核 bind 会看整个结构（部分 libc 校验填充区必须为 0），可能直接 EINVAL。
    //            这就是"构造第一件事是 memset"的原因，也是它不能被随手重排的原因。
    sockaddr_in addr_;
};