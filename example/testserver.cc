/* ============================================================================
 * 【文件】example/testserver.cc
 * 【一句话】用户视角的入口示范：写一个 Echo 服务器只需要“造 loop → 造 server 并注册两个
 *          回调 → setThreadNum → start + loop.loop()” 四步，业务代码全在两个回调函数里。
 * 【Reactor 角色】业务 Handler（用户的 ConcreteHandler：onConnection 处理上下线，
 *          onMessage 处理可读数据）
 * 【依赖】TcpServer.h（门面，顺带把 EventLoop/InetAddress/Buffer/Callbacks/TcpConnection
 *        全带进来 —— 所以用户只需 include 这一个头）、Logger.h（LOG_INFO 宏）、<string>。
 * 【谁用它】不被任何人用；它是 main 所在的可执行程序入口（CMake 里的 testserver 目标）。
 * 【先修知识】① ①→⑫ 全链路（本文件是 ①②③④ 的实际发生地，⑤~⑪ 由客户端触发后发生）；
 *        ② std::bind 绑成员函数（this + 占位符）；③ 栈上对象的析构时机；
 *        ④ 非阻塞 socket 的“read 返回 0 == 对端关闭”。
 * 【难度】★☆☆☆☆   【建议耗时】20 分钟（但要顺着它回看 TcpServer/TcpConnection 才划算）
 * 【自检】1) setThreadNum(3) 之后进程里一共有几个线程、几个 EventLoop？各自负责什么？
 *        2) 如果把 main 里最后那句 loop.loop() 删掉，会发生什么？为什么？
 *        3) 把 onMessage 里被注掉的 conn->shutdown() 打开，服务变成什么形态？
 *           关闭链上依次发生哪 6 件事（⑪ 全链）？
 * 【四步速记】①EventLoop loop（主线程栈上）②EchoServer(&loop, addr, name) 里注册回调
 *   ③setThreadNum(N) ④server.start() 之后 main 里必须 loop.loop()。
 *   这四步之外，用户不需要知道 epoll、线程、锁、缓冲区 —— 这就是 muduo 想给你的东西。
 * ============================================================================ */
#include <string>
#include "TcpServer.h"
#include "Logger.h"

// 【设计】为什么用户要自己包一层 EchoServer：把“业务状态 + 回调函数 + TcpServer 成员”
//   关在同一个对象里，回调就能用 std::bind(&EchoServer::onXxx, this, ...) 绑成员函数，
//   业务数据不需要全局变量。这是 muduo 官方示例的标准写法（继承 TcpServer 反而是反例）。
//   【易错】回调里通过 this 访问的成员，寿命必须不短于服务器；
//   本文件里 EchoServer 是 main 的栈对象、TcpServer 是它的成员，二者同生死，安全。
class EchoServer
{
public:
// 【流程】①→③ 都在这里发生：构造 server_（→ Acceptor 构造 → socket+bind，即 ②）、
//   注册两个回调、setThreadNum(3)。
// 【线程】mainLoop（此刻还没有任何 subLoop 线程；线程要到 start() 才起）。
// 【语法】构造参数里传 EventLoop* loop（不是引用）：muduo 全线用裸指针表达“不拥有”，
//   loop 的主人是 main 栈上那个对象。
    EchoServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
// 【语法】初始化列表里 server_(loop, addr, name) 只给三个实参，第四个 Option 用声明处的
//   默认值 kNoReusePort（默认实参只能写在声明里，本文件这样调用是合法的）。
//   【易错】成员初始化顺序 = 声明顺序（server_ 先、loop_ 后，见本类底部两个成员）；
//   这里写法与声明顺序一致，没有 -Wreorder 风险。
        : server_(loop, addr, name)
        , loop_(loop)
    {
// 【设计】“注册回调函数”就是 muduo 的全部编程模型：
//   你不写循环、不建线程、不碰 epoll，只提供“发生 X 事件时该干什么”的函数。
        // 注册回调函数
// 【流程】上线/下线通知的注册点。这一句把函数对象塞进 TcpServer::connectionCallback_，
//   再由 TcpServer::newConnection 拷贝给每条新连接（三级传递），最终在
//   TcpConnection::connectEstablished（⑦）与 handleClose（⑪）里被调用。
// 【线程】注册动作在 mainLoop；回调执行在 subLoop#N。
        server_.setConnectionCallback(
// 【语法】std::bind(&EchoServer::onConnection, this, std::placeholders::_1)：
//   成员函数指针的“第 0 个参数”是对象本身，所以必须先补 this；
//   _1 表示“将来 ConnectionCallback 被调用时传进来的第 1 个实参（const TcpConnectionPtr&）
//   填在这里”。填完之后的可调用对象签名正好是 void(const TcpConnectionPtr&)，
//   与 Callbacks.h 里的 ConnectionCallback 匹配，才装得进 std::function。
//   【易错】不写 _1 就变成“零参数可调用对象”，装不进 function<void(const TcpConnectionPtr&)>
//   —— 这是初学者最常见的编译错误（“no matching function for call to ...”）。
//   【易错】bind 不做任何生命周期管理：它只记 this 这个裸地址。
            std::bind(&EchoServer::onConnection, this, std::placeholders::_1));
        
// 【流程】可读数据通知的注册点；最终在 TcpConnection::handleRead（⑧）里被调用。
        server_.setMessageCallback(
// 【语法】三个占位符对应 MessageCallback 的三个参数（连接、Buffer*、时间戳），
//   顺序必须与函数签名一致：_1→conn、_2→buf、_3→time。写反就是类型不匹配编译错误。
//   【易错】bind 的实参是**拷贝**语义：这里拷的是 this（指针）与三个占位符，
//   没有拷贝任何数据，所以没有额外开销；换成 lambda 捕获 [this] 效果等价。
//   【设计】为什么用 lambda 也完全可以（本行只是风格示范）：
//   server_.setMessageCallback([this](const TcpConnectionPtr& c, Buffer* b, Timestamp t){ onMessage(c,b,t); });
//   —— 只要最终类型是 std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)> 就行。
            std::bind(&EchoServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

// 【设计】setThreadNum 必须在 start() 之前调用（start 里会 threadPool_->start()，
//   之后改数字已经太晚）。它决定的是 **IO 线程个数**，不是“连接数上限”。
//   【流程】③ 的前置。
        // 设置合适的subloop线程数量
// 【流程】setThreadNum(3) 之后进程里一共 **4 个线程 / 4 个 EventLoop**：
//   ┌──────────────┬───────────────┬────────────────────────────────────┐
//   │ 线程          │ loop          │ 职责                                │
//   ├──────────────┼───────────────┼────────────────────────────────────┤
//   │ 主线程        │ mainLoop(loop)│ 跑 ④ 的 loop()；accept 新连接（⑤⑥）；│
//   │              │               │ 独占 connections_；处理跨线程投递    │
//   │ IO 线程 #1   │ subLoop#1     │ 分到它的 connfd 的读写（⑦⑧⑨⑩⑪）     │
//   │ IO 线程 #2   │ subLoop#2     │ 同上                                │
//   │ IO 线程 #3   │ subLoop#3     │ 同上                                │
//   └──────────────┴───────────────┴────────────────────────────────────┘
//   分配规则：TcpServer::newConnection 里 threadPool_->getNextLoop() 轮询（round-robin）。
//   【易错】主线程**自己也跑一个 loop**，它只做 accept 和“表”的维护，不做业务 IO；
//   所以别以为主线程会被业务卡住 —— 但如果用户在 onConnection 里干重活，卡的是 subLoop。
//   【易错】写 0 或不写就是单 Reactor 单线程模式：所有连接都由 mainLoop 处理
//   （EventLoopThreadPool::getNextLoop 永远返回 baseLoop），这时线程数 = 1。
//   【面试】“muduo 的线程模型？”→ one loop per thread + MainReactor accept、
//   SubReactor 做 IO，连接与线程一一绑定，因此一条连接内部完全无锁。
        server_.setThreadNum(3);
    }
// 【流程】③ 的触发点（薄封装，只是为了让 EchoServer 的对外接口像一个服务器）。
//   【易错】start() 返回**不等于**已经能接客：listen 是 runInLoop 投递给 mainLoop 的，
//   而此刻 mainLoop 还没进 loop()（下一行 main 里才进），所以真正生效要等到 loop.loop()
//   跑起来、doPendingFunctors 执行 Acceptor::listen 之后。
    void start()
    {
        server_.start();
    }

// 【设计】private 里放回调实现是刻意的：这两个函数只应由库调，不该被业务直接调
//   （直接调就绕过状态机，等于在错误的线程里改连接状态）。
private:
    // 连接建立或断开的回调函数
// 【流程】⑦（上线）与 ⑪（下线）共用的回调。
//   【线程】subLoop#N —— 注意：这是**该连接所属的那个 IO 线程**，不是主线程！
//   【易错】在这里做耗时操作会阻塞该 subLoop 上**所有**连接的读写。
//   【设计】想按连接存业务状态，应该用 conn->name() 当 key 或 setContext 之类外挂
//   （本项目 TcpConnection 没有 context 接口 —— 可核实的退化点），
//   不要往 EchoServer 里塞全局计数器（那要加锁，因为多个 subLoop 会并发进来）。
    void onConnection(const TcpConnectionPtr &conn)   
    {
// 【设计】同一个回调函数用 connected() 区分两种时机，这是 muduo 的固定套路：
//   true = 刚 connectEstablished（能收发）；false = 刚 handleClose（已 disableAll，
//   再 send 也没用；但对象还活着，可以读它的地址信息打日志）。
//   【易错】false 分支里**不要**调 conn->send()（会走进 sendInLoop 那条
//   “state_==kDisconnected 只打日志不 return”的可疑路径，见 src/TcpConnection.cc）。
        if (conn->connected())
        {
// 【流程】上线日志。toIpPort() 每次构造一个 std::string，所以必须 .c_str() 才能喂给 %s
//   （LOG_INFO 是 printf 风格的可变参数宏，传 std::string 是未定义行为）。
//   【易错】printf 家族 + std::string 是新手事故高发区；
//   正确写法只有 .c_str()，或者把宏换成流式输出。
//   【设计】这里显示的地址只用于运维观察：注意 conn->peerAddress() 是构造时的**快照**。
            LOG_INFO("Connection UP : %s", conn->peerAddress().toIpPort().c_str());
        }
        else
        {
// 【流程】下线日志（⑪ 的可见证据）。
//   【易错】这条日志在 subLoop 打，而紧随其后的 TcpServer::removeConnectionInLoop
//   那条日志在 mainLoop 打，所以这两行之间确实发生了一次线程跳转。
//   【易错】别指望从日志文字看出线程：本项目日志格式没有 tid/线程名字段（带 tid 的那句是
//   LOG_DEBUG，没定义 MUDEBUG 就是空宏），日志只能给你**先后顺序**（DOWN 先、removeConnectionInLoop 后）。
//   要眼见为实请用 gdb 的 info threads（docs/experiments.md 第 3 节），或自己给 LOG_INFO 加上
//   CurrentThread::tid() 参数（那属于改造练习，本仓库现在没有）。
//   【设计】想知道当前在线人数，只能像这样自己在 UP/DOWN 两边维护计数，
//   并且必须用 atomic（多个 subLoop 线程会并发进这里）。
            LOG_INFO("Connection DOWN : %s", conn->peerAddress().toIpPort().c_str());
        }
    }

// 【流程】⑧ 的执行体（用户侧）。
//   【线程】subLoop#N。【设计】三个参数由 TcpConnection::handleRead 传下来：
//   shared_from_this()、&inputBuffer_、pollReturnTime_。
//   【易错】第三个参数 time 在本实现里**没被使用**（g++ -Wextra 会报 unused parameter），
//   它存在的意义是“消息到达时间”：做超时/协议时间戳校验才用它。
    // 可读写事件回调
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time)
    {
// 【设计】retrieveAllAsString() = 构造一个 std::string 拷出全部可读数据 + 推进 readerIndex
//   到 writerIndex（即“读走并清空”）。两步必须一起做：只 peek 不 retrieve，
//   inputBuffer_ 会无限膨胀。单位：字节；返回值是**拷贝**，所以这里有一次内存分配。
//   【易错】“一次 onMessage ≠ 一行/一个包”：TCP 是字节流，客户端发 2 次可能被合并成 1 次回调，
//   也可能 1 次发送被拆成 2 次回调（拆包/粘包）。Echo 服务器只是原样退回所以看不出来，
//   真实协议必须自己加分帧（长度前缀最常用）。
//   【易错】若业务只取前 N 字节，应调 retrieveAsString(N) / retrieve(N)，
//   剩下的留在 buffer 里等下一段 —— 别用 retrieveAllAsString 再手工截断（多拷一份）。
        std::string msg = buf->retrieveAllAsString();
// 【流程】⑨ 的起点。【线程】就在 subLoop（handleRead 的调用栈上）→
//   TcpConnection::send 里 loop_->isInLoopThread() 为真 → 直接 sendInLoop →
//   先尝试一次 ::write；内核发送缓冲区塞不下才 append 进 outputBuffer_ + enableWriting（→ ⑩）。
//   【易错】如果这个 send 是从**别的线程**（业务线程池）发起的，同一段代码会变成
//   queueInLoop + wakeup，跨线程把数据搬到 subLoop 再写 ——
//   而本项目的跨线程分支只拷了 buf.c_str() 这个指针（悬垂风险，详见 src/TcpConnection.cc
//   里 send 的那条【⚠问题】），所以这段示例“恰好在 subLoop 里 send”才是安全的。
//   【设计】Echo 的正确姿势（大流量场景）不要用 string 中转，
//   应该 Buffer::retrieveAll 之类零拷贝手段，或者用 write 后原样搬移；这里为了教学清晰而选择最直白的写法。
        conn->send(msg);
// 【流程】★这一行是被注掉的，但它才是“短连接 Echo”的灵魂★
// 【设计】打开它，服务器行为从“长连接”变成“短连接”：每收一条消息就回发并关写端。
//   完整关闭链（⑪ 的全套六步，逐步对应到源码）：
//   1) conn->shutdown() → state_ 由 kConnected 迁到 kDisconnecting，并把 shutdownInLoop
//      投递给 subLoop（若此刻已在 subLoop 则就地执行）。
//   2) shutdownInLoop：!channel_->isWriting() 时 socket_->shutdownWrite()，
//      即 shutdown(fd, SHUT_WR) —— **只关写端**，给对端发 FIN；读端仍然打开，
//      所以还能收到对端发来的最后数据（这才是“优雅”，直接 close 会把没读完的数据 RST 掉）。
//   3) 对端 read 返回 0（收到 FIN），它按惯例也关掉自己那端 → 回一个 FIN。
//   4) 本端内核把该 fd 标记为“对端已关”，于是 epoll 上报 EPOLLHUP（不带 EPOLLIN），
//      Channel::handleEventWithGuard 里那条 (EPOLLHUP && !EPOLLIN) 命中 → closeCallback_
//      → TcpConnection::handleClose。（若对端是 FIN 且我们读到 0，则走 handleRead 的 n==0 → handleClose。）
//   5) handleClose：state_→kDisconnected、disableAll、connectionCallback_（打印 DOWN）、
//      closeCallback_（= TcpServer::removeConnection，【线程跳转 2】到 mainLoop）。
//   6) removeConnectionInLoop：connections_.erase（mainLoop）→ ioLoop->queueInLoop
//      connectDestroyed（【线程跳转 3】回 subLoop）→ disableAll + channel_->remove()
//      → functor 里的 shared_ptr 析构 → 引用计数归零 → ~TcpConnection → ~Socket 才 close(connfd)。
//   【易错】如果此时 outputBuffer_ 还有没发完的数据，shutdownInLoop 会**什么都不做**，
//   FIN 要等 handleWrite 排空后在末尾补一次 shutdownInLoop 才发出去
//   —— 所以“shutdown 之后一定立刻断”是误解，正确理解是“发完再断”。
//   【易错】短连接场景下客户端数量巨大时，服务端会积累大量 TIME_WAIT（主动关的一方承担），
//   这正是本项目无条件开 SO_REUSEADDR 的原因；真要上线还得考虑 TCP_CORK/延迟关闭或改回长连接。
//   【面试】“shutdown 和 close 的区别”“muduo 怎么优雅关闭连接”—— 答这六步。
        // conn->shutdown();   // 关闭写端 底层响应EPOLLHUP => 执行closeCallback_
    }
// 【设计】值成员 TcpServer（不是指针）：服务器与业务对象同生死，构造顺序上
//   server_ 先构造（先 bind 端口）、后设回调；析构时反序 —— server_ 最后死，
//   于是 ~TcpServer 会先把所有连接送回各自 loop 线程销毁。
//   【易错】这个成员**必须在 mainLoop 所属线程（即主线程）里销毁**：
//   ~TcpServer 里用 runInLoop 投递，如果调用者不是 loop 线程，投递会排队，
//   而那时 loop 可能已经退出 → 连接永远不被回收。main 里“先 server 出作用域、
//   后 loop 停止”的顺序（见下面 main 的说明）恰好保证了 runInLoop 就地执行。
    TcpServer server_;
// 【设计】loop_ 在本类里其实**没被使用**（两个回调只用 conn->getLoop()），
//   留着它是示例的惯例写法：表明“业务对象知道自己挂在哪个 loop 上”。
//   【易错】别把它当“可以随便从别的线程调 conn->send()”的依据；真要跨线程，
//   用 conn->getLoop()->runInLoop(...) 把整段业务动作投过去。
    EventLoop *loop_;

};

// 【流程】①：进程入口，四步全部体现在这 6 行里。
//   【线程】下面整个 main 都跑在主线程，而主线程就是 mainLoop 线程 ——
//   这是 muduo 的硬性约定：baseLoop 必须由主线程创建（EventLoop 构造里用
//   __thread t_loopInThisThread 检查“一个线程只能有一个 EventLoop”）。
int main() {
// 【设计】EventLoop 是**栈对象**：地址稳定、生命周期由 main 的作用域决定，
//   所有 EventLoop* 裸指针都指向它（mainLoop 的 loop_）。
//   【易错】不要 new EventLoop 再随便 delete：EventLoop 内含 Poller/Channel/epoll fd，
//   它的销毁必须在自己的线程里做（原版 EventLoop 有 assertInLoopThread 类约束，
//   本项目则靠“它活在栈上、main 结束时同线程销毁”这个事实）。
//   【易错】同线程创建第二个 EventLoop 会 LOG_FATAL 退出（t_loopInThisThread 检查）。
    EventLoop loop;
// 【语法】InetAddress addr(8080)：只给端口 → ip 是 INADDR_ANY（0.0.0.0），
//   意思是“本机所有网卡都接客”。要限定网卡就写 InetAddress(8080, "127.0.0.1")。
//   【易错】端口 <1024 需要 root（bind 失败 → TcpServer 构造里 LOG_FATAL → exit）。
//   【易错】端口被占 → bind 失败也是直接结束进程，不会返回错误码，排障要看日志。
    InetAddress addr(8080);
// 【流程】②+注册回调+setThreadNum：这一行里发生了 socket/bind/setsockopt/Acceptor 建 Channel，
//   以及 4 个线程数上限的记录（还没起线程）。
//   【易错】构造完成时**还没 listen**：此刻 connect 上来会失败（内核没进 LISTEN 状态）。
//   所以要紧接着 server.start()。
    EchoServer server(&loop, addr, "EchoServer");
// 【流程】③：起 3 个 subLoop 线程 + 把 Acceptor::listen 投给 mainLoop。
//   【设计】start 之后 mainLoop 还没进 loop()，投递的 listen 动作正躺在 pendingFunctors_ 里，
//   等下一行 loop.loop() 的第一轮 doPendingFunctors 执行 —— 顺序上完全正常。
//   【易错】多次调用 start 无害（started_.fetch_add 只让首次生效）。
    server.start();
// 【流程】④：主线程从此进入事件循环（epoll_wait 阻塞，kPollTimeMs=10000ms 超时）。
//   这一句是整个服务器“活着”的原因，**绝对不能省**。
//   【易错】如果删掉这句：main 继续往下 return 0 → 栈上对象按声明反序销毁
//   （server 先死 → ~TcpServer 里给每条连接投 connectDestroyed，而此刻 mainLoop 已无人跑 loop；
//   接着 loop 死 → ~EventLoop 关掉 wakeupFd_）→ 服务器一启动就退出，
//   客户端表现为“连不上/立刻断”，而且不会有任何 accept/onConnection 日志。
//   更细的一点：~TcpServer 的 runInLoop 在“当前就是该 loop 线程”时就地执行，
//   所以退出路径仍能优雅拆连接（前提是没有别的线程还在动它）。
//   【设计】loop.loop() 只有在有人调 EventLoop::quit() 时才返回（本项目没有内置退出条件，
//   所以靠 Ctrl-C 杀进程；⑫ 的正规退出姿势要在某个回调里调 loop.quit()）。
//   【易错】主线程一旦卡在自己的业务函数里（比如在 onConnection 里 sleep），
//   整个 mainLoop 停摆：不再 accept 新连接（已建立的连接由各 subLoop 仍能继续收发）。
    loop.loop();
    return 0;
}