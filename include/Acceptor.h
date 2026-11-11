/* ============================================================================
 * 【文件】include/Acceptor.h
 * 【一句话】声明“只干一件事”的类：把一个 listenfd 绑到 mainLoop 的 Channel 上，
 *          listenfd 可读就 accept 出新连接，然后把 (connfd, 对端地址) 交给上层回调。
 * 【Reactor 角色】Acceptor（Reactor 模式里专门“接收新连接”的那个特殊 Handler）
 * 【依赖】Socket.h（listenfd 的封装，**按值内嵌**成员）、Channel.h（listenfd 的事件通道，
 *        也按值内嵌）、noncopyable.h、<functional>（回调类型）；
 *        EventLoop / InetAddress 只需前向声明（只以指针/引用出现）。
 * 【谁用它】只有 TcpServer：TcpServer 构造函数里 new Acceptor，
 *        TcpServer::start 里调它的 listen()，
 *        TcpServer 构造函数里把自己的 newConnection 设进 setNewConnectionCallback。
 * 【先修知识】① socket/bind/listen/accept4 四个系统调用；② 非阻塞 fd 与 LT 触发；
 *        ③ std::function 做“反向通知”实现解耦；④ 成员初始化顺序 == 声明顺序。
 * 【难度】★★☆☆☆   【建议耗时】20 分钟（配合 src/Acceptor.cc 读）
 * 【自检】1) 为什么 acceptSocket_/acceptChannel_ 是**值成员**，而 TcpConnection 里同样的
 *           东西却是 unique_ptr？（提示：完整类型是否已经在头里 include）
 *        2) NewConnectionCallback 的参数为什么是 (int sockfd, const InetAddress&)
 *           而不是 TcpConnectionPtr？这带来什么解耦效果？
 *        3) listenning_ 什么时候被写、什么时候被读？
 * ============================================================================ */
#pragma once

// 【语法】<functional> 只为本类的 std::function 服务。注意本文件**不** include Callbacks.h：
//   Acceptor 处在“连接对象还不存在”的那一层，它只交出裸 fd，所以不需要 TcpConnectionPtr
//   这个别名。这是分层分得干净的一个证据（依赖方向：Acceptor ← TcpServer，而不是 ← TcpConnection）。
#include <functional>

// 【设计】Socket.h 和 Channel.h 在这里必须**完整 include**，因为下面是两个值成员
//   （acceptSocket_ / acceptChannel_）—— 编译器要算 Acceptor 的大小，非完整类型不行。
//   【对比】TcpConnection 里 socket_/channel_ 用 unique_ptr，所以它只需前向声明；
//   两种做法的取舍见 TcpConnection.h 里 class Channel; 那一段。
//   值成员的好处：无额外堆分配、无智能指针开销、生命周期完全跟随 Acceptor（Acceptor 又
//   跟随 TcpServer），符合“listenfd 和服务同生死”的语义。
#include "noncopyable.h"
#include "Socket.h"
#include "Channel.h"

// 【语法】两个前向声明：EventLoop 只以 EventLoop* 出现，InetAddress 只以 const 引用
//   出现在回调签名和函数形参里 —— 都是“指针/引用用法”，不需要完整定义（同 Callbacks.h 那条规则）。
//   【易错】Acceptor.cc 里构造 acceptChannel_(loop, acceptSocket_.fd()) 时要调 Socket::fd()，
//   那一刻需要 Socket 完整 —— 已由上面的 include 满足。
class EventLoop;
class InetAddress;

// 【设计】Acceptor 是 noncopyable：一个 listenfd 只能有一个主人（值成员 Socket 一旦
//   被拷贝就会 double close：两份 ~Socket 都 close 同一个 fd）。
//   一个进程里通常也只有一个 Acceptor（一个 TcpServer 一个），所以它没有“多实例”需求。
//   【Reactor 角色】它是 mainLoop 上唯一一个 Handler 之外的特殊 Handler：只关心 EPOLLIN。
class Acceptor : noncopyable
{
public:
// 【设计】这个回调是 Acceptor 与 TcpServer 之间**唯一**的连线（规范点名的解耦手段）：
//   参数故意给成 (int sockfd, const InetAddress &peerAddr) —— 一个裸 fd + 对端地址，
//   而不是 TcpConnectionPtr。于是：
//   ① Acceptor 不知道 TcpConnection、TcpServer、EventLoopThreadPool 中任何一个的存在，
//      它连“分给哪个 subLoop”都不关心；
//   ② 想换一套连接管理方式（比如自己做 epoll 池、或者加个 SSL 层）只要换掉这个回调，
//      Acceptor 一行不改；
//   ③ Acceptor 因此可以在没有 TcpConnection.h 的情况下独立编译、独立测试。
// 【语法】using 别名写在类内：它是 Acceptor::NewConnectionCallback（嵌套类型），
//   类内可直接用裸名，类外要写 Acceptor::NewConnectionCallback。
//   C++11 的 using 与 typedef 能力等价，但 using 在模板里不会被误当成值，且可读性更好。
// 【易错】别名 NewConnectionCallback（类型）与下面成员变量 NewConnectionCallback_
//   （变量，只差一个下划线）同名遮蔽，读代码时极易看混 —— 原版 muduo 的成员叫
//   newConnectionCallback_（小写下划线风格），本项目改成大驼峰加下划线，是隐患写法。
//   辨认口诀：带下划线结尾的是**对象**，不带的是**类型**。
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress &)>;

// 【设计】三个构造参数的分工：loop（这个 Acceptor 归属哪个事件循环，实际一定是 mainLoop）、
//   listenAddr（绑哪个 ip:port）、reuseport（本意是“是否开 SO_REUSEPORT”）。
// 【⚠问题】reuseport 形参在本项目里**完全没有被使用**：src/Acceptor.cc 的构造函数体里
//   无条件写死 setReusePort(true)，于是 TcpServer 的 Option 参数（kReusePort/kNoReusePort）
//   一路传下来就断了，编译器 -Wunused-parameter 也会报。通行说法里的“bind/setReuseAddr 只在
//   构造做一次”这条说的就是这里：所有 socket 选项和 bind 都在这一步完成，一次到位。
    Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport);
// 【流程】析构发生在 ~TcpServer 里 unique_ptr<Acceptor> 放手的那一刻（进程退出路径，
//   mainLoop 线程）。它会把 listenfd 一并 close（值成员 ~Socket）——
// 【对比原版】据原书讨论，原版关 listenfd 前会先 shutdown，好让接受队列里“已完成三次握手但还没 accept”
//   的连接被回 RST，详见 src/Acceptor.cc 析构处。
    ~Acceptor();
    //设置新连接的回调函数
// 【流程】② 的收尾动作：TcpServer 构造函数里调用一次，把自己的 newConnection 塞进来。
//   【线程】mainLoop（TcpServer 构造通常就在主线程）。
//   【易错】必须**先设回调再 listen**：listen 之后新连接随时可能触发 handleRead，
//   此时回调若为空就直接 close(connfd)（见 handleRead），客户端表现为“连上就被踢”。
//   本项目 TcpServer 构造里就设好了、start 里才 listen，顺序是安全的。
//   【易错】赋值不加锁也不判空，std::function 的拷贝赋值不是原子操作；跨线程热改是 UB。
    void setNewConnectionCallback(const NewConnectionCallback &cb) { NewConnectionCallback_ = cb; }
    // 判断是否在监听
// 【设计】listenning() 是给外部观察“服务器是否已开始监听”的只读接口。
// 【线程】写发生在 mainLoop 的 listen() 里；读（若有人读）就必须在 mainLoop，否则是数据竞争。
// 【⚠问题】本仓库里**没有任何地方调用**它（grep 全库只有本行和成员定义），
//   也就是说 listenning_ 目前是“只写不读”的半死状态；它的真实作用只剩下
//   “给测试/调试用的可观察标志”。原版 muduo 的 Acceptor 也没有这个 getter，
//   它把状态检查交给 assertInLoopThread + listen 一次即成事实。
// 【易错】它不是 atomic：写它的是 listen()（在 mainLoop 里被 runInLoop 投递执行），
//   读它的如果换线程就是数据竞争。目前没人读，所以没暴露问题。
//   拼写沿用原版 muduo 的 listenning_（正确英文应是 listening，属历史遗留）。
    bool listenning() const { return listenning_; }
    // 监听本地端口
// 【流程】③ 的执行体：listen(2) + enableReading，真正的“开始接客”动作。
//   【线程】必须在 mainLoop —— TcpServer::start 用 loop_->runInLoop 投递它。
//   【设计】为什么和构造函数分两步：bind 是“占坑”（不占坑就没法 accept），
//   放在构造里可以让“端口被占用”这种错误在 TcpServer 构造当场暴露（LOG_FATAL 退出）；
//   而 listen 是“开始接客”，一旦调用，内核就开始把 SYN 收进半连接/接受队列，
//   而此刻用户可能还要 setThreadNum、注册回调 —— 谁先谁后会有“来不及处理就被塞满队列”
//   的窗口，所以把 listen 留给 start()（并让它可重复调用无害）。
//   【对比原版】原版 muduo 的 Acceptor 构造函数同样只做 socket/bind/setsockopt，
//   listen() 由 TcpServer::start 投递给主线程 loop，顺序完全一致。
    void listen();

private:
// 【流程】⑤ 的执行体：listenfd 上 EPOLLIN == 有新连接完成三次握手。
//   【线程】mainLoop（唯一执行 epoll_wait 的循环之一）。
//   【设计】它是 private，且只在构造函数里被 std::bind 绑到 acceptChannel_ 的
//   readCallback_ 上 —— 全库没有第二处调用者。
//   【易错】bind 时**没有写 placeholders::_1**（handleRead 无参，而 readCallback_ 是
//   std::function<void(Timestamp)>）：多出来的那个时间戳被 std::bind 丢弃。
//   这与 TcpConnection 构造里 handleRead(Timestamp) 必须写 _1 形成对照，见那两个文件。
    void handleRead();//处理新用户的连接事件

// 【设计】loop_ 一定是 baseLoop/mainLoop（由 TcpServer 传入，而 TcpServer 的 loop_
//   又来自用户 main 里那个 EventLoop）。理由：accept 只做“拿 fd + 分发”，工作量极小，
//   放在主线程不会成为瓶颈；而真正的 IO 在 subLoop。
//   【线程】mainLoop。它是裸指针不拥有对象（EventLoop 在用户栈上）。
    EventLoop *loop_; // Acceptor用的就是用户定义的那个baseLoop 也称作mainLoop
// 【设计】值成员：Acceptor 一构造就 new 出 listenfd 的所有权（构造函数里
//   createNonblocking() 的返回值直接进它的初始化列表）。~Acceptor 时 ~Socket 负责 close。
//   【易错】const Socket 的构造要求初始化列表给值 —— 本项目没写 const，但 fd 的
//   “一次赋值终身不变”这个语义由 Socket::sockfd_ 的 const 保证（见 include/Socket.h）。
    Socket acceptSocket_;//专门用于接收新连接的socket
// 【设计】值成员 Channel：它只是“fd + 感兴趣事件 + 四个回调”的登记表，
//   构造时不注册 epoll（events_=0、index_=-1），注册动作留给 listen() 里的 enableReading。
//   【易错】Channel 不拥有 fd、析构也不 close fd（见 Channel 那份注释）——
//   本类里“谁关 fd”的答案只有一个：acceptSocket_。
    Channel acceptChannel_;//专门用于监听新连接的channel
// 【设计】空 std::function 是**可判空**的，handleRead 里就靠 if (NewConnectionCallback_)
//   决定是否分发。谁负责填它：TcpServer 构造函数。
//   【易错】名字与上面 NewConnectionCallback 那个别名只差末尾下划线，务必看清（本行是变量，别名是类型）。
    NewConnectionCallback NewConnectionCallback_;//新连接的回调函数
// 【流程】③ 的 listen() 里被置 true，之后不再改。含义是“已调用过 listen(2)”，
//   并不代表“当前仍有新连接进来”。
    bool listenning_;//是否在监听
};