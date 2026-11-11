/* ============================================================================
 * 【文件】src/TcpServer.cc
 * 【一句话】把 Acceptor、线程池、连接表和四份用户回调组装成一台服务器：
 *          收到新连接就起名、建 TcpConnection、登记、把上线动作投递给某个 subLoop；
 *          连接断了再把回收动作要回 mainLoop 处理。
 * 【Reactor 角色】门面 + 组装者（不参与 epoll，但掌握全库唯一的“连接所有权表”）
 * 【依赖】TcpServer.h（自己的声明）；Logger.h（LOG_FATAL/INFO/ERROR）；TcpConnection.h
 *        （这里必须 include：newConnection 要 new TcpConnection(...)，构造与删除器都要完整类型）；
 *        <functional>（std::bind）、<string.h>（memset，给 getsockname 的 sockaddr_in 清零）。
 * 【谁用它】用户（构造/start/析构）；Acceptor 通过 NewConnectionCallback 反向调它；
 *        TcpConnection 通过 closeCallback_ 反向调它的 removeConnection。
 * 【先修知识】① ①→⑫ 全链路（尤其 ⑥ 与 ⑪ 的三次线程跳转）；
 *        ② runInLoop / queueInLoop 的差别；③ std::bind 按值捕获实参；
 *        ④ shared_ptr 计数与“谁最后放手谁析构”。
 * 【难度】★★★★☆   【建议耗时】40 分钟
 * 【自检】1) 一条新连接从 accept 到用户回调，经历哪三次线程跳转，各由本文件哪一行发起？
 *        2) ~TcpServer 为什么要把 item.second 拷进局部 conn 再 reset？
 *        3) removeConnectionInLoop 为什么先 erase、后 queueInLoop（而不是 runInLoop）？
 * ============================================================================ */
#include <functional>
#include <string.h>

// 【语法】TcpConnection.h 在这里 include 是**必须**的，不是风格问题：
//   newConnection 里那句 new TcpConnection(...) 需要完整类型（要算大小、要调构造函数），
//   shared_ptr<TcpConnection>(TcpConnection*) 的删除器也要完整类型（要调 ~TcpConnection）。
//   【对比】Callbacks.h 只前向声明就能定义别名，本文件却要 include 完整定义 ——
//   同一个类型在不同翻译单元里对“完整度”的要求不同，这是理解前向声明的关键对照。
#include "TcpServer.h"
#include "Logger.h"
#include "TcpConnection.h"

// 【语法】static → 内部链接（与 src/TcpConnection.cc 里的同名函数各归各家，否则链接期重定义）。
//   【设计】唯一作用：把 nullptr 检查塞进初始化列表里（loop_(CheckLoopNotNull(loop))），
//   因为初始化列表里只能写表达式、不能写 if 语句。
//   【⚠问题】LOG_FATAL 展开里有 exit(-1)（见 include/Logger.h 的宏），所以“loop 为空”
//   的结局是进程直接结束：不走 RAII 析构、不刷新输出缓冲，日志可能丢。本函数的
//   return loop 只在非空时可达，因此绝不能省 —— 省了就是“无返回值函数落到底”的 UB。
// 【流程】② 的前置检查（TcpServer 构造第一步）。
// 【线程】随调用者，实际是 mainLoop（用户在主线程构造 TcpServer）。
static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null!\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 构造函数：一台服务器在内存里被组装出来的那一刻（流程 ②）                      │
// └────────────────────────────────────────────────────────────────────────────┘
// 【流程】②：socket + bind + setReuseAddr/Port 都在这条链里发生（具体在 Acceptor 构造函数内）。
// 【线程】mainLoop（用户在主线程构造 EchoServer，于是 loop_ 就是 main 栈上那个 EventLoop）。
// 【设计】成员初始化顺序按**声明顺序**（loop_ → ipPort_ → name_ → acceptor_ → threadPool_ →
//   三个回调 → threadInitCallback_ → numThreads_ → started_ → nextConnId_ → connections_），
//   而下面这段初始化列表的书写顺序与之**不一致**：nextConnId_ 写在 started_ 前面。
//   【⚠问题】实测 g++ -Wall 会在这里报：
//     warning: ‘TcpServer::nextConnId_’ will be initialized after [-Wreorder]
//   两个都是常量初始化，所以**没有实际危害**，但它是“书写顺序骗人”的活教材：
//   真正决定谁先构造的是 include/TcpServer.h 里的声明顺序。若哪天某个成员要依赖另一个成员，
//   按列表顺序想当然就会写出真 bug（依赖声明顺序的初始化在 Acceptor 里就有，那里顺序是对的）。
//   规矩：初始化列表一律按声明顺序写，让 -Wreorder 永远闭嘴。
TcpServer::TcpServer(EventLoop *loop,
                     const InetAddress &listenAddr,
                     const std::string &nameArg,
                     Option option)
// 【设计】loop_ 存 mainLoop；它是后面所有“回主线程”动作的落脚点
//   （removeConnectionInLoop 靠它、start 里的 listen 投递也靠它）。
    : loop_(CheckLoopNotNull(loop))
// 【语法】listenAddr.toIpPort() 在构造期就把地址转成字符串存下来（形如 0.0.0.0:8080），
//   之后拼连接名只用这个快照，不再回头查 InetAddress 对象。
//   【易错】它是一次性快照：如果 bind 的是 0.0.0.0，连接名里显示的就是 0.0.0.0:8080，
//   不是本机真实网卡地址（真实本端地址由 newConnection 里的 getsockname 另查）。
    , ipPort_(listenAddr.toIpPort())
    , name_(nameArg)
// 【流程】② 的主体：new Acceptor 里完成 socket(SOCK_NONBLOCK|SOCK_CLOEXEC) + bind +
//   setReuseAddr/setReusePort + 建 acceptChannel_。
//   【语法】option == kReusePort 这个表达式就是 Option 枚举唯一被“读”的地方 ——
//   但【⚠问题】传进去的 bool 在 Acceptor 构造函数里没被使用（那边无条件 setReusePort(true)），
//   所以 kNoReusePort/kReusePort 目前行为相同。
//   【设计】unique_ptr：所有权唯一（一个 server 一个 acceptor），析构顺序由声明顺序决定。
    , acceptor_(new Acceptor(loop, listenAddr, option == kReusePort))
// 【设计】线程池此刻**还没有任何线程**：EventLoopThreadPool 构造只是记下 baseLoop 和名字。
//   真正的 N 个 subLoop 线程要等 start() 里 threadPool_->start() 才拉起（流程 ③）。
//   name_ 传给线程池是为了给每个 EventLoopThread/线程起名（日志里能看到 muduo #1 #2 #3）。
//   【易错】threadPool_(new EventLoopThreadPool(loop, name_)) 里的 name_ 能安全使用，
//   是因为 name_ 声明在 threadPool_ **之前**（同一条声明顺序规矩的正面例子）。
    , threadPool_(new EventLoopThreadPool(loop, name_))
// 【语法】显式写“()”= 调用 std::function 的默认构造函数，得到**空**函数对象。
//   这里不写也是同样结果，写出来是给自己看的声明：“门面不预置任何默认回调”。
//   【⚠问题】正因为没有默认实现，用户忘记 setConnectionCallback / setMessageCallback 时，
//   TcpConnection 的 connectEstablished / handleRead 里裸调空 std::function 会抛
//   std::bad_function_call，异常在 subLoop 里无人 catch → std::terminate，进程崩。
//   【对比】原版 muduo 在这两个成员上挂了默认的 defaultConnectionCallback 之类兜底实现
//   （是否如此以原版源码为准）；本仓库可核实的是这里没有兜底。
//   【流程】writeCompleteCallback_ 没在这里显式初始化，同样是空 function，
//   但它的调用点全部判空（sendInLoop / handleWrite），所以不注册只是“没有通知”，不会崩。
    , connectionCallback_()
    , messageCallback_()
// 【易错】nextConnId_ 从 1 开始（不是 0）：连接名里 #0 容易被误读成“第 0 条=没建好”，
//   从 1 起更像人读的编号。它与 started_ 的书写顺序颠倒会触发上面那段 -Wreorder 警告。
//   【设计】它不加 atomic：唯一的 ++ 发生在 newConnection 里，而 newConnection 只在 mainLoop 跑。
    , nextConnId_(1)
    , started_(0)
{
// 【流程】② 的收尾：把自己的 newConnection 交给 Acceptor。
//   【设计】这一句是全库“反向调用”的起点：Acceptor 完全不认识 TcpServer（参数只有
//   int + InetAddress），却能在有新连接时把 TcpServer::newConnection 叫起来 ——
//   靠 std::function 的类型擦除，而不是靠继承或前向声明。
    // 当有新用户连接时，Acceptor类中绑定的acceptChannel_会有读事件发生，执行handleRead()调用TcpServer::newConnection回调
// 【语法】std::bind(&TcpServer::newConnection, this, _1, _2)：
//   newConnection 是成员函数，隐含第 0 个参数是对象，所以先补 this，再用两个占位符
//   接住 Acceptor 传来的 (connfd, peerAddr)。
//   缺任何一个 _1/_2 都会导致 bind 结果签名与 NewConnectionCallback 不匹配 → 编译错误。
//   【易错】绑的是裸 this：TcpServer 必须活得比 Acceptor（以及任何还在跑的 handleRead）久。
//   在本项目里成立 —— acceptor_ 是 TcpServer 的成员，随它一起死，且 Channel 的回调
//   只在 loop 线程跑；但如果有人把 TcpServer 放到子线程里销毁而 mainLoop 还在跑，就会踩空。
//   【对比】TcpConnection 绑四个 handler 时也是裸 this，但它额外有 Channel::tie 兜底；
//   Acceptor/Channel 这一对没有 tie，靠“值成员/所有权同生死”这条更简单的规矩保证安全。
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
}

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 析构：进程退出路径上的连接回收（规范点名的 item.second.reset() 技巧）        │
// └────────────────────────────────────────────────────────────────────────────┘
// 【线程】调用者线程（用户 EchoServer 出作用域 → 通常是主线程，而主线程正是 mainLoop）。
// 【设计】要解决的问题：connections_ 里的每个 TcpConnection 都归属于某个 subLoop，
//   它的销毁必须在**它自己的 loop 线程**里完成（因为 ~Channel/~Socket 之前一定要先
//   在 subLoop 里做完 disableAll + epoll DEL；而 epoll 那套容器没有锁）。
//   直接在 mainLoop 里把 shared_ptr 减一，就可能“在错误的线程里析构对象”。
// 【语法】技巧三件套（缺一不可）：
//   ① TcpConnectionPtr conn(item.second); —— 先拷一份到**栈上局部量**，拿到第 2 个强引用；
//   ② item.second.reset(); —— 把 map 里那份强引用释放掉，此刻对象只由局部 conn 持有；
//   ③ conn->getLoop()->runInLoop(std::bind(&TcpConnection::connectDestroyed, conn)); ——
//      bind **按值拷贝 conn**（引用计数 +1，变 2），把这份强引用装进 functor。
//   于是函数返回/循环下一轮时局部 conn 析构（-1），最后一个引用留在 functor 里；
//   functor 在 subLoop 的 doPendingFunctors 里执行完才析构（-1 → 0）→
//   ~TcpConnection 正好在 subLoop 线程发生。这就是“对象在自己的 loop 线程里被
//   connectDestroyed 并最终析构”的完整机制。
// 【易错】如果少了 ①② 只做 runInLoop(connectDestroyed, item.second)：
//   map 里的强引用还留着，connections_ 的 value 与 functor 各一份，
//   而 ~TcpServer 结束后 map 也随之销毁，顺序上仍可能析构在 mainLoop —— 不确定。
//   写成“局部 + reset”就把它变成**确定**的：map 已经不再持有。
// 【易错】runInLoop 在“当前就在该 loop 线程”时是**就地执行**：
//   单线程模式（setThreadNum(0)）下 ioLoop == mainLoop == 调用者线程，
//   connectDestroyed 当场跑完，然后局部 conn 出作用域 → 对象在 mainLoop 析构，
//   而 mainLoop 就是它的 loop 线程，依然正确。多线程模式下走 queueInLoop，
//   投递到 subLoop 执行并在那里析构。两种情况都满足“在自己的 loop 线程里死”。
// 【⚠问题】遍历 connections_ 时没有清空 map 本身（reset 只把 value 变空，key 还在），
//   随后 map 整体析构；功能无害，只是“表里残留一堆空 shared_ptr”。
// 【⚠问题】本函数没有停 accept：acceptor_ 是成员，销毁顺序上它比 connections_ **晚**死，
//   所以理论上“正在拆连接的窗口里仍可能 accept 到新连接”（原版 muduo 在 ~TcpServer 里
//   显式 acceptor_.reset() 以避免这一点；本项目是否触发取决于退出时机，属于隐患）。
// 【流程】⑫ 退出路径 + ⑪ 的连接回收（本项目里唯一在析构函数中跨线程投递任务的类）。
TcpServer::~TcpServer()
{
    for(auto &item : connections_)
    {
// 【语法】TcpConnectionPtr conn(item.second)：显式拷贝构造，引用计数 +1。
//   写成语义更清楚的 auto& 或直接 move 都会破坏后面那套“最后一个引用在 functor 里”的设计。
        TcpConnectionPtr conn(item.second);
// 【设计】行尾原注释说的就是这个意思：让 map 不再持有对象，把“唯一的所有者”换成栈上的 conn。
        item.second.reset();    // 把原始的智能指针复位 让栈空间的TcpConnectionPtr conn指向该对象 当conn出了其作用域 即可释放智能指针指向的对象
        // 销毁连接
// 【线程】这里用 **conn->getLoop()** 而不是 loop_：
//   每条连接要回的是**它自己**的那个 subLoop，不是 mainLoop。写成 loop_ 就是经典错误
//   （在 mainLoop 里对属于 subLoop 的连接调 epoll / 在 mainLoop 里析构它）。
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn));
// 【语法】bind 里的 conn 是**按值拷贝的 shared_ptr**：functor 生命期内对象一定活着。
//   这是全库三处“bind 捕获 shared_ptr 延长寿命”手法之一（另两处见
//   TcpServer::removeConnectionInLoop 与 TcpConnection::sendFile）。
    }
}

// 设置底层subloop的个数
// 【流程】③ 的前置。【线程】mainLoop（用户构造期调用）。
void TcpServer::setThreadNum(int numThreads)
{
// 【易错】规范点名的变量遮蔽（shadowing）：这里声明了一个**局部** int numThreads_，
//   与成员 numThreads_ 同名，之后 threadPool_->setThreadNum(numThreads_) 用的是局部那份。
//   后果：① 功能仍然正确（真正决定线程数的 threadPool_ 收到了正确数字）；
//   ② 成员 TcpServer::numThreads_ 永远是**未初始化**的垃圾值，且没有任何地方读它，
//      所以看不出问题；③ 读代码的人极易以为“TcpServer 记录了线程数”，其实没有。
//   【语法】这种遮蔽默认不报警（-Wshadow 不在 -Wall/-Wextra 里，要显式开）。
//   【面试】“为什么这行没写 const/下划线开头会出事”→ 命名规范（成员一律 xxx_，
//   局部不要用 xxx_ 结尾）就是为躲这一类“看起来对、其实改了另一个变量”的错。
//   正确写法：直接 threadPool_->setThreadNum(numThreads);（形参就叫 numThreads）。
    int numThreads_=numThreads;
    threadPool_->setThreadNum(numThreads_);
}

// 开启服务器监听
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ start：③ 的起点，也是“重复 start 防御”的样板                               │
// └────────────────────────────────────────────────────────────────────────────┘
// 【流程】③：起 N 个 subLoop 线程 → 把 Acceptor::listen 投递到 mainLoop 执行
//   （listen(2) + enableReading，此刻 listenfd 才进 epoll）。
// 【线程】调用者线程（通常 mainLoop）；listen 的实际执行一定在 mainLoop。
// 【设计】顺序很重要：必须先 threadPool_->start()，才能保证 newConnection 里
//   getNextLoop() 拿到的 subLoop 已经存在（EventLoopThreadPool::start 会建好 loops_）。
//   反过来先 listen，就可能在池子还没起来时 accept 到新连接并取到空 loop。
void TcpServer::start()
{
// 【语法】started_.fetch_add(1) 返回**自增之前**的值：首次返回 0 → 条件成立；
//   第二次起返回 1、2、3… → 条件不成立。整个判断 + 置位由一次原子操作完成，
//   不存在“两个线程都读到 0”的窗口。
//   【对比】普通写法 if (started_ == 0) { started_ = 1; ... } 是 check-then-act，
//   两个线程可能同时通过检查 → 起两套线程池、listen 两次（虽然 listen 幂等，但线程池重复起就炸）。
//   【易错】fetch_add 让第二个调用者**直接返回**，但第一次的 listen 可能还在
//   pendingFunctors_ 里没执行 —— 所以 start() 返回不等于“已经能接客”。
//   【⚠问题】用 atomic_int 当 bool 只是省了一个 exchange；语义上更清晰的写法是
//   started_.exchange(true) == false（原版 muduo 的思路；具体以原版源码为准）。
//   【设计】started_ 是 atomic_int，值会随调用次数一直涨（0,1,2…），不是布尔。
    if (started_.fetch_add(1) == 0)    // 防止一个TcpServer对象被start多次
    {
// 【流程】③ 第一步：拉起 N 个 IO 线程（每个线程内部：EventLoopThread::startLoop →
//   新线程里建 EventLoop（栈对象）→ 跑 loop() → 条件变量把 loop 指针交回主线程）。
//   【线程跳转】mainLoop → 新建的 subLoop#1..#N。
//   【设计】threadInitCallback_ 此刻才第一次被使用，且是在**每个新线程里**被调用；
//   传空 function 也没关系（EventLoopThreadPool::start 里会判空 —— 本仓库可核实）。
        threadPool_->start(threadInitCallback_);    // 启动底层的loop线程池
// 【流程】③ 第二步。【语法】acceptor_.get() 从 unique_ptr 里取裸指针给 bind ——
//   Acceptor 不可拷贝、也不需要延长寿命（它是 server 的成员），用裸指针最贴切。
//   若这里写 *acceptor_ 会试图拷贝 Acceptor → 编译失败（noncopyable）。
// 【线程】runInLoop：当前若就在 mainLoop（正常情况）则**立即执行** listen()；
//   若用户从别的线程调 start()，则排队 + wakeup，由 mainLoop 执行。
//   【易错】这就是“start() 不是同步完成”的根源：跨线程调用时返回得很快，listen 还没发生。
        loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));
    }
}

// 有一个新用户连接，acceptor会执行这个回调操作，负责将mainLoop接收到的请求连接(acceptChannel_会有读事件发生)通过回调轮询分发给subLoop去处理
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ newConnection：全库最忙的一个函数（流程 ⑥，含【线程跳转 1】）               │
// └────────────────────────────────────────────────────────────────────────────┘
// 【流程】⑥：轮询选 ioLoop → 造连接名 → getsockname → new TcpConnection →
//   进 connections_ → 三级传递回调 → closeCallback 绑 removeConnection →
//   ioLoop->runInLoop(connectEstablished)（这一投就是【线程跳转 1】）。
// 【线程】整个函数在 mainLoop（由 Acceptor::handleRead 同步调用），
//   除了最后那行投递，其余都不跨线程。
// 【设计】为什么“建对象”这种重活敢放在 mainLoop：它只做纯内存操作 + 一次 getsockname，
//   没有任何 IO、没有锁、没有阻塞；真正的 epoll 注册交给 subLoop 的 connectEstablished。
void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr)
{
   // 轮询算法 选择一个subLoop 来管理connfd对应的channel
// 【设计】轮询（round-robin）分配：EventLoopThreadPool::getNextLoop 内部
//   loops_[next_++ % size]，numThreads_==0 时永远返回 baseLoop_。
//   【易错】轮询只看“新连接到来”的顺序，不看各连接的负载；长连接场景可能不均，
//   生产上常按“最小连接数/最小负载”选 loop。
//   【面试】“muduo 怎么做负载均衡？”→ 默认轮询，要更聪明的算法就改 getNextLoop。
    EventLoop *ioLoop = threadPool_->getNextLoop();
// 【语法】buf[64] 装 “-ip:port#id” 这一段。snprintf 带 sizeof 限制，不会越界写
//   （最坏情况是**截断**，IPv4 的 ip:port 最长 21 字符，加 “-#%d” 远小于 64，实际不会截）。
//   【易错】连接名唯一的来源是 nextConnId_，不是 fd：fd 会被内核复用，
//   用它当 key 有踩到旧条目的风险（这也是 connections_ 用 string 做 key 的理由）。
    char buf[64] = {0};
// 【系统调用】snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_)：
//   第二个参数是**目标缓冲区容量**，函数保证不写越界，返回“若不限长本应写出的字符数”。
//   所以判断截断要比较返回值 >= sizeof buf（本项目没判，靠容量足够大）。
    snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
// 【设计】++nextConnId_ 不加锁不用 atomic：这条语句只在 mainLoop 执行（见函数头【线程】），
//   别的线程既不会调 newConnection 也不会读它 —— “单线程独占”比“原子”更便宜也更清楚。
//   先自增再用当前值拼 buf？不：snprintf 已经用过 nextConnId_，这里是**用完才加**，
//   所以第一条连接的编号是 1（构造函数里初始化为 1）。
    ++nextConnId_;  // 这里没有设置为原子类是因为其只在mainloop中执行 不涉及线程安全问题
// 【设计】connName = 服务器名 + “-ip:port#编号”，形如 EchoServer-0.0.0.0:8080#1。
//   它同时是日志里的连接代号和 connections_ 的 key（两职一身，所以必须唯一且稳定）。
    std::string connName = name_ + buf;

    LOG_INFO("TcpServer::newConnection [%s] - new connection [%s] from %s\n",
             name_.c_str(), connName.c_str(), peerAddr.toIpPort().c_str());
    
    // 通过sockfd获取其绑定的本机的ip地址和端口信息
// 【系统调用】getsockname(sockfd, (sockaddr*)&local, &addrlen)：查“这个 socket 的**本端**地址”。
//   为什么需要它：listenAddr 可能是 0.0.0.0（任意网卡），accept 出来的 connfd 到底绑在
//   哪个具体 IP 上，只有内核知道（多网卡/多 IP 机器上差别很大）。
//   前置：addrlen 必须先赋 sizeof（in/out 参数，进内核时要知道缓冲区大小）。
//   失败返回 -1（EBADF/ENOTSOCK 等），本项目只 LOG_ERROR，不终止 —— local 可能保持全 0，
//   于是 localAddr_ 变成 0.0.0.0:0，只影响日志显示，不影响功能。
//   【对比】对端地址不用查：accept4 的 addr 参数已经填好（见 src/Acceptor.cc 的 handleRead）。
//   【易错】memset 清零在这里只是“防御 + 便于调试”，getsockname 会自己覆盖整个结构体。
    sockaddr_in local;
    ::memset(&local, 0, sizeof(local));
    socklen_t addrlen = sizeof(local);
    if(::getsockname(sockfd, (sockaddr *)&local, &addrlen) < 0)
    {
        LOG_ERROR("sockets::getLocalAddr");
    }

// 【语法】InetAddress localAddr(local)：用 sockaddr_in 构造一个地址对象（本端），
//   和上面 peerAddr（对端）成对，一起塞进 TcpConnection 的 const 成员。
    InetAddress localAddr(local);
// 【流程】⑥ 的核心动作：把裸 connfd 变成一个受 shared_ptr 管理的连接对象。
//   【设计】第 1 个参数是 **ioLoop 而不是 loop_** —— 决定了这条连接以后所有的读写、
//   事件注册、析构都在这个 subLoop 线程（这就是“one loop per thread”的落地时刻）。
//   【语法】这一行是 TcpConnection 在全库唯一的 new 点，也是 shared_ptr 控制块诞生处；
//   它要求 TcpConnection 是完整类型（本文件 include TcpConnection.h 的真正理由）。
//   【易错】构造里绝不能调 shared_from_this()（bad_weak_ptr），所以 connectEstablished
//   必须留到对象进了 shared_ptr 之后再做 —— 这就是下面两个动作存在的原因。
    TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                            connName,
                                            sockfd,
                                            localAddr,
                                            peerAddr));
// 【设计】先登记再投递，两个理由：
//   ① 让 connections_ 成为这份对象的第一个强引用（shared_from_this 从此合法可用）；
//   ② 就算 runInLoop 之后立刻有事件（同线程不会打断，但语义上保险），
//      这条连接也已经在“服务器拥有的连接集合”里，removeConnection 能找到它。
//   【易错】key 用 connName 而不是 fd（同上）。这里没有查重：如果名字撞了（例如 nextConnId_
//   被外部重置），map 会**静默覆盖**旧 value，旧连接的强引用被丢掉 → 提前析构。
//   本项目靠自增保证不撞，所以没做查重。
    connections_[connName] = conn;
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 回调的三级传递：用户 → TcpServer → TcpConnection（→ Channel 用另一套四件套） │
// └────────────────────────────────────────────────────────────────────────────┘
// 【设计】链路：用户 EchoServer 构造里 server_.setConnectionCallback(...) 存进
//   TcpServer::connectionCallback_（第一级）→ 本函数把成员**拷贝**给这条连接
//   conn->setConnectionCallback(connectionCallback_)（第二级）→ 连接在自己的
//   connectEstablished / handleRead / handleClose 里调用它们（第三级）。
//   Channel 侧那四根线（handleRead/handleWrite/handleClose/handleError）不是这条链的一部分：
//   它们是“事件 → TcpConnection 成员函数”的低层连线，在 TcpConnection 构造函数里绑。
// 【易错】传的是**拷贝/快照**：start() 之后再改 TcpServer 的成员回调，
//   只影响以后新建立的连接，已经建立的连接手里还是老副本。
// 【易错】用户没注册就是空 std::function（本文件构造函数里显式初始化成空），
//   而 connectEstablished / handleRead 的调用点不判空 → std::bad_function_call。
    // 下面的回调都是用户设置给TcpServer => TcpConnection的，至于Channel绑定的则是TcpConnection设置的四个，handleRead,handleWrite... 这下面的回调用于handlexxx函数中
// 【流程】三个用户级回调的注入（上线通知 / 可读数据 / 发送完成）。
//   【易错】writeCompleteCallback_ 是从门面**统一**下发的（用户 setWriteCompleteCallback），
//   而高水位回调没有对应 setter（TcpServer 没暴露），只能在 onConnection 里对单条连接
//   conn->setHighWaterMarkCallback(...) 手工设 —— 想找“怎么设 64MB 阈值”，答案在这。
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    // 设置了如何关闭连接的回调
// 【流程】⑪【线程跳转 2】的接线点：这条连接的 closeCallback_ = TcpServer::removeConnection。
// 【设计】为什么绑 TcpServer 而不是某个连接内建逻辑：连接的“所有权”在门面手里
//   （connections_ 是它的成员），回收必须由所有者做；TcpConnection 只负责喊一声“我死了”。
// 【易错】用 _1 占位接住 handleClose 传来的 const TcpConnectionPtr&；
//   写成 std::bind(&TcpServer::removeConnection, this) 会因签名不匹配编译失败。
// 【易错】绑的是裸 this：TcpServer 必须比所有连接活得久（正常退出流程里成立，
//   因为 ~TcpServer 会主动把所有连接送出；但如果用户先销毁 TcpServer 再让 loop 继续跑，
//   就是悬垂 —— 这也是本类析构里那三件套存在的原因之一）。
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

// 【流程】⑥ 的最后一步 = 【线程跳转 1】：mainLoop → subLoop。
// 【设计】机制：bind **按值捕获 conn**（引用计数 +1）→ ioLoop->runInLoop：
//   当前在 mainLoop 而 ioLoop 是 subLoop，isInLoopThread() 为假 → queueInLoop：
//   加锁 push 进 subLoop 的 pendingFunctors_ → 写 wakeupFd_(eventfd) 唤醒卡在
//   epoll_wait 的 subLoop → 它醒来跑 doPendingFunctors → 执行 connectEstablished 这一步
//   才真正 setState(kConnected) + tie + enableReading + 通知用户上线（流程 ⑦）。
//   【易错】为什么必须“在 subLoop 里”做 enableReading：epoll_ctl 和 Poller 的 Channel
//   映射表都没有锁，只有那个 loop 自己的线程能动它。跨线程直接 enableReading
//   是最典型的一类 muduo 误用（会破坏 Poller 的数据结构，症状是随机崩溃或事件丢失）。
//   【易错】这里用 runInLoop 而不是 queueInLoop：语义上等价（runInLoop 会自己判断在不在
//   目标线程），但保留 runInLoop 的写法等于在说“如果哪天单线程模式（ioLoop==mainLoop），
//   这条路径就直接就地执行”，少一次排队。
    ioLoop->runInLoop(
        std::bind(&TcpConnection::connectEstablished, conn));
}

// 【流程】⑪ 的“跨线程回主”跳板，被 TcpConnection::handleClose 的 closeCallback_ 调起。
// 【线程】**在 subLoop#N** 执行（不要在 mainLoop 改 connections_！），函数体只有一句话：
//   把 removeConnectionInLoop 投给 loop_（mainLoop）。
// 【设计】为什么需要这层跳板：connections_ 归 mainLoop 独占（newConnection 也在 mainLoop 写它），
//   所以删除也必须在 mainLoop，两个写者不会同时动 map，全程无锁。
//   【线程跳转 2】的依据就在这一行：loop_->runInLoop + bind 捕获 conn（按值 shared_ptr）。
// 【易错】runInLoop 在“不在目标线程”时等价于 queueInLoop，一定会唤醒 mainLoop；
//   如果写成 mainLoop 的 loop() 正卡在 epoll_wait(10s) 而没有 wakeup，回收会延迟 ——
//   queueInLoop 里有 wakeup()，所以不会。
void TcpServer::removeConnection(const TcpConnectionPtr &conn)
{
    loop_->runInLoop(
        std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

// 【流程】⑪ 的 mainLoop 侧收口：从表里摘掉 + 把销毁动作再投给 subLoop（【线程跳转 3】）。
// 【线程】mainLoop（EventLoop::loop 的 doPendingFunctors 里）。
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
{
    LOG_INFO("TcpServer::removeConnectionInLoop [%s] - connection %s\n",
             name_.c_str(), conn->name().c_str());

// 【设计】先 erase 再投递，是这段代码的顺序精髓：
//   erase 会减一次引用计数（map 那份强引用没了）。此刻对象至少还剩两份引用：
//   ① 本函数的形参 conn（const 引用，指向调用者 functor 里那份）；
//   ② 下面 bind 会拷进队列的那份。
//   所以 erase 不会当场析构对象 —— 这是“为什么敢先删再投”的理由。
//   【易错】如果顺序反过来（先 queueInLoop 再 erase），functor 里已经有引用，
//   行为其实一样；但如果 erase 之后**不再**投递（或投递时不捕获 shared_ptr 而捕获裸指针），
//   对象就可能在下一个 epoll 回调里被使用 —— 那就是 use-after-free。
//   规范点名的“queueInLoop 里捕获的 conn 保证对象活到回调执行”，说的就是 bind 实参
//   按值拷贝 shared_ptr 这一条 C++ 语义（不是 muduo 的魔法）。
    connections_.erase(conn->name());
    EventLoop *ioLoop = conn->getLoop();
// 【语法】为什么这里是 queueInLoop 而不是 runInLoop：
//   ioLoop 是**别的线程**（多线程模式下），两者行为相同（runInLoop 内部就是分流到 queueInLoop）；
//   但单线程模式下 ioLoop == loop_ == 当前线程，此时 runInLoop 会**就地执行**
//   connectDestroyed → 在本函数的调用栈上（还在遍历/刚 erase 完 map）就把对象拆掉，
//   形成“在持有它的回调栈里自我销毁”的局面。用 queueInLoop 就把销毁推到
//   下一轮 doPendingFunctors，栈上再没有本函数与本对象的痕迹，绝对安全。
//   【易错】queueInLoop 内部还会因 callingPendingFunctors_ 为真而额外 wakeup 一次，
//   保证“这一轮结束时立刻能执行”而不用等下一次 epoll 超时（kPollTimeMs 是 10 秒）。
//   【对比原版】原版 muduo 在这个位置写着注释“Don't use runInLoop，因为 conn 可能是
//   最后一个引用，析构要发生在 loop 线程”（原文以源码为准，本环境未能核对）。
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn));
}