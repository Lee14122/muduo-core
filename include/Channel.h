/* ============================================================================
 * 【文件】include/Channel.h
 * 【一句话】把「一个 fd + 它感兴趣的读写事件 + 四个事件回调」捆成一个对象，让 Poller 报回来的
 *          事件位能够被翻译成对业务成员函数的调用；它是 epoll 世界与 C++ 对象世界之间的适配器。
 * 【Reactor 角色】Handler 登记表 + Demultiplexer 出口。注意 Channel 自己不发起任何 IO：
 *          它从不 read()/write()/accept()，只做「事件发生了 → 该调哪个函数」这一件事。
 * 【依赖】<functional>（std::function 回调容器）、<memory>（tie 用的 shared_ptr/weak_ptr）、
 *        "noncopyable.h"（禁止拷贝）、"Timestamp.h"（ReadEventCallback 的参数类型）。
 *        对 EventLoop 只用「前向声明」，不 include —— 原因见下面 class EventLoop; 处的【设计】。
 * 【谁用它】EventLoop::loop() 每轮遍历 activeChannels_ 调 handleEvent()；
 *        TcpConnection 构造时调 4 个 set*Callback()、connectEstablished() 里调 tie()+enableReading()、
 *        handleClose()/connectDestroyed() 里调 disableAll()+remove()；
 *        Acceptor 给 listenfd 的 channel 调 setReadCallback()+enableReading()；
 *        EventLoop 给 wakeupFd_ 也建了一个 channel；EPollPoller 用 set_revents()/index()/set_index()。
 * 【先修知识】1) epoll 的事件位（EPOLLIN/EPOLLPRI/EPOLLOUT/EPOLLERR/EPOLLHUP）
 *          2) 位运算 |=（开位）、&= ~（关位）、&（测位） 3) std::function 的类型擦除
 *          4) weak_ptr::lock() 把弱引用提升为强引用
 * 【难度】★★★★☆   【建议耗时】35 分钟
 * 【自检】① events_ 与 revents_ 的区别？谁写、谁读、在哪个线程？
 *        ② Channel 为什么不 include EventLoop.h，却又能存一个 EventLoop* ？
 *        ③ tie() 防的是哪一种具体的崩溃？不 tie 会在第几步（①→⑫）出事？
 * 【口径】本文件凡【对比原版】条目里涉及原版 muduo 具体写法的话，本环境无网络、无原版源码，
 *        一律「以原版仓库为准」，不当作已核实事实；凡写「可核实：…」的都是能在本仓库
 *        直接 grep / 读码验证的事实。本文件的隐患与要点已全部就地打了标签，用 grep 【⚠问题】/【易错】/【对比原版】 即可索引。
 * =========================================================================== */

#pragma once

#include <functional>
#include <memory>

#include "noncopyable.h"
#include "Timestamp.h"

class EventLoop;

// 【设计】前向声明（forward declaration）而不是 #include "EventLoop.h"，是为了打破循环包含：
//        EventLoop.h 里必须出现 Channel（成员 std::unique_ptr<Channel> wakeupChannel_、
//        std::vector<Channel*> activeChannels_），所以它要 include "Channel.h"；
//        若 Channel.h 反过来再 include "EventLoop.h"，两边互相要求对方先展开完，
//        即便有 #pragma once 也只会得到其中一方看到「未定义的类型」而编译失败。
// 【语法】这里能只用声明，是因为 Channel 里 EventLoop 只以「指针」形式出现：
//        指针的大小固定（本平台 8 字节），编译器不需要知道 EventLoop 有哪些成员；
//        真正要「解引用 loop_->updateChannel(this)」的代码全在 src/Channel.cc 里，
//        那个 .cc 同时 include 了 Channel.h 和 EventLoop.h，两边都在它这里变成完整类型。
//        一句话：头文件之间用「声明」解耦，源文件里用「include」闭合。
// 【对比原版】可核实：本仓库 include/EventLoop.h 同样只写 class Channel; 前向声明，两边对称，
//        「头文件互相只给名字」这条规则才成立。原版 muduo 是否与此逐字一致，本环境无原版
//        源码可查，以原版仓库为准。

/**
 * 理清楚 EventLoop、Channel、Poller之间的关系  Reactor模型上对应多路事件分发器
 * Channel理解为通道 封装了sockfd和其感兴趣的event 如EPOLLIN、EPOLLOUT事件 还绑定了poller返回的具体事件
 **/
class Channel : noncopyable
// 【语法】class 默认是 private 继承，所以这里是「私有继承 noncopyable」：
//        效果 = 拷贝构造/赋值对本类及其调用者都不可用，但本类仍能调用基类 protected 的构造/析构。
//        Channel 有 fd、有事件状态、有回调，被拷贝会造成同一个 fd 有两个 channel 注册到 epoll，
//        属于必须禁止的语义，因此这里禁拷贝是「正确性」要求，不是风格要求。
// 【设计】Channel 的生命周期由「谁创建它」用 unique_ptr/成员对象负责：
//        TcpConnection 里是 std::unique_ptr<Channel> channel_；Acceptor 里是直接成员 Channel acceptChannel_；
//        EventLoop 里是 std::unique_ptr<Channel> wakeupChannel_。Poller 只借指针、不拥有。
{
public:
    using EventCallback = std::function<void()>; // muduo仍使用typedef
    // 【语法】using 别名与 typedef 等价（using 更好读、且能用于模板）。
    // 【设计】为什么是 std::function 而不是「void(*)(int) 这种函数指针」：
    //        实际要绑的是 TcpConnection 的成员函数 + this（std::bind(&TcpConnection::handleRead, this, _1)），
    //        成员函数指针必须搭配对象才能调用，普通函数指针装不下「对象 + 方法」这一对，
    //        std::function 就是能擦除这个差异的统一容器。代价：可能有堆分配与一次间接调用。
    // 【易错】std::function 默认构造是「空的」，直接调用空 std::function 抛 std::bad_function_call；
    //        所以 handleEventWithGuard 里每个分支都先 if (callback) 判空再调。
    using ReadEventCallback = std::function<void(Timestamp)>;
    // 【设计】只有读回调带 Timestamp，因为只有「读」需要告诉用户数据是什么时候到达的
    //        （写完成/连接关闭的时刻由内核即时决定，用户不关心）。
    // 【流程】这个 receiveTime 一路来自 ④ 里 EPollPoller::poll() 返回的 Timestamp::now()，
    //        经 EventLoop::loop() 的 pollRetureTime_ → Channel::handleEvent → readCallback_(receiveTime)
    //        → TcpConnection::handleRead → 用户 onMessage(conn, buf, receiveTime)。
    //        也就是说它是「epoll_wait 返回那一刻」，不是「网卡收到那一刻」，见 EPollPoller.cc 的【易错】。

    Channel(EventLoop *loop, int fd);
    // 【流程】②（Acceptor 构造给 listenfd 建 channel）、①（EventLoop 构造给 wakeupFd_ 建 channel）、
    //        ⑥（TcpServer::newConnection 里 new TcpConnection 时给 connfd 建 channel）三处都会走到。
    // 【线程】在谁手里构造就在谁的线程：acceptChannel_/wakeupChannel_ 在 mainLoop（①②），
    //        connfd 的 channel 对象本身是在 mainLoop 里 new 出来的（⑥），但它「注册进 epoll」被推迟到
    //        subLoop#N 里执行 connectEstablished 时才发生（⑦）——这是【设计】上的关键点：
    //        构造 ≠ 注册，注册必须在 ownerLoop 所在线程做，否则 epoll_ctl 的线程归属就乱了。
    // 【设计】参数是裸指针 int fd：fd 的所有权仍在 Socket（TcpConnection 持有 unique_ptr<Socket>），
    //        Channel 只是「借用者」，它需要 fd 只是为了拿它去调 epoll_ctl。
    // 【易错】构造函数不会把 fd 注册到 epoll，此时 events_==0、index_==-1(kNew)，
    //        对端连上也不会惊动这个 channel——必须有人调 enableReading() 才真正进 epoll。
    ~Channel();
    // 【设计】Channel 析构不做任何 IO 相关的事，见 src/Channel.cc 的注释。
    // 【⚠问题】Channel 析构里不 close(fd_)：Close 的责任在 Socket::~Socket()（A2 的文件）。
    //        一个 fd 同时被 Socket 与 Channel 引用，如果两边都 close 就是 double close，
    //        会把别的线程刚 open 到的同号 fd 误关（经典 fd 复用竞态）。所以规矩是：
    //        Channel 只写「怎么监听」，Socket 只写「怎么关闭」，互不越界。

    // fd得到Poller通知以后 处理事件 handleEvent在EventLoop::loop()中调用
    // 【流程】⑤⑦⑧⑩⑪ 里所有「事件驱动」的动作，最终都从这一个函数进去：
    //        ④ epoll_wait 返回 → EventLoop::loop() 遍历 activeChannels_ → channel->handleEvent(pollRetureTime_)
    //        → handleEventWithGuard() → 按 revents_ 的位分发到 readCallback_/writeCallback_/...
    // 【线程】只在 channel 的 ownerLoop 所在线程被调用（mainLoop 或某个 subLoop#N），
    //        这就是「一个 channel 的回调永远不会并发」的由来，也是 TcpConnection 内部不加锁的前提。
    void handleEvent(Timestamp receiveTime);
    // 【设计】public 的 handleEvent 只做「tie 保护」这一件事，真正分发在 private 的 handleEventWithGuard。
    //        拆成两层是为了让 guard 的生命周期精确覆盖回调调用期间，见 src/Channel.cc::handleEvent。

    // 设置回调函数对象
    // 【设计】四个 setter 都 inline 在头文件里，只是 std::move 一次赋值，不碰 epoll、不加锁。
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    // 【流程】② Acceptor 绑 handleRead、⑥ TcpConnection 绑 handleRead（用户数据到达时走 ⑧）。
    // 【线程】谁 new 出 channel 谁的线程（mainLoop 里构造 TcpConnection/Acceptor）。
    // 【易错】必须在 enableReading() 之前设好回调：一旦注册进 epoll，subLoop 线程随时可能拿着
    //        这个 channel 跑回调，此时再改 readCallback_ 就是跨线程写 std::function（数据竞争）。
    // 【语法】形参按值 + std::move：调用方传左值会拷一次、传右值/bind 结果直接移动，
    //        比「const 引用 + 拷贝」在绑 std::bind 结果时少一次堆分配。
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    // 【流程】⑥ 绑 TcpConnection::handleWrite；⑩ EPOLLOUT 到达时被调用（把 outputBuffer_ 继续写干）。
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    // 【流程】⑥ 绑 TcpConnection::handleClose；⑪（read 返回 0 或 EPOLLHUP）时被调用，
    //        handleClose 再经 closeCallback_（=TcpServer::removeConnection）触发线程跳转 2/3。
    // 【易错】本项目里 closeCallback_ 只在「EPOLLHUP 且非 EPOLLIN」时走；
    //        对端正常 FIN 关闭时更常见的路径是 EPOLLIN + read()==0，由 TcpConnection::handleRead 自己调 handleClose。
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }
    // 【流程】⑥ 绑 TcpConnection::handleError；revents_ 含 EPOLLERR 时（⑪ 的异常路径）被调用。

    // 防止当channel被手动remove掉 channel还在执行回调操作
    // 【面试】「Channel::tie 是干什么的？为什么参数是 shared_ptr<void>？」
    //        一句话答案：把宿主对象（TcpConnection）以 weak_ptr<void> 的形式挂在 Channel 上，
    //        handleEvent 里用 lock() 提升出局部 shared_ptr 当「保命绳」——宿主已死就不调回调，
    //        宿主还活着就把它引用计数抬到至少 1，保证回调期间不被别的线程析构；
    //        用 void 是为了类型擦除（Channel 不该认识 TcpConnection），且不会造成循环引用。
    // 【设计】这行原注释只说了一半。tie 真正防的是：「TcpConnection 已经析构，
    //        但它的 Channel 还被 activeChannels_ 里的裸指针拿着，正在执行/准备执行回调」。
    //        详见 src/Channel.cc::tie / ::handleEvent 的逐行讲解（本项目最经典面试点）。
    void tie(const std::shared_ptr<void> &);
    // 【流程】⑦ TcpConnection::connectEstablished() 里调 channel_->tie(shared_from_this())，
    //        这是全项目唯一一次 tie 调用。
    // 【设计】为什么形参是 shared_ptr<void> 而不是 TcpConnectionPtr：
    //        Channel 不该知道上层是什么类型（TcpConnection、还是别的什么），用 void 擦掉类型，
    //        weak_ptr<void> 只保留「控制块 + 指针」，lock() 成功即代表宿主还活着。
    //        注意 shared_ptr<void> 的删除器来自构造它的 shared_ptr<T>，类型擦除不会漏析构。
    // 【语法】声明里故意不写形参名，表示「接口只用类型」，定义在 .cc 里才起名 obj。

    int fd() const { return fd_; }
    // 【设计】给 Poller 用的（EPollPoller::updateChannel / removeChannel 里取 fd 作 channels_ 的 key，
    //        EPollPoller::update 里传给 epoll_ctl）。【线程】调用者是 ownerLoop 线程。
    int events() const { return events_; }
    // 【设计】只被 EPollPoller::update 读取，用来填 epoll_event.events 交给 epoll_ctl。
    //        即「我感兴趣的事件」这条唯一出口，是 Channel 与 epoll 位值直接同构的证明。
    void set_revents(int revt) { revents_ = revt; }
    // 【流程】④ poll → EPollPoller::fillActiveChannels 里写：channel->set_revents(events_[i].events)。
    // 【线程】ownerLoop 线程（就是执行 epoll_wait 的那个线程），因此和 handleEvent 同线程，不会竞争。
    // 【易错】revents_ 是「内核报的已发生事件」，不是「我感兴趣的事件」；两者位宽不同：
    //        内核总会额外报 EPOLLERR/EPOLLHUP（即使你从没注册过它们），所以 revents_ 里可能出现
    //        events_ 里根本没有的位。这也是 handleEventWithGuard 要判 EPOLLHUP 的原因。

    // 设置fd相应的事件状态 相当于epoll_ctl add delete
    // 【设计】下面 5 个开关是 Channel 对外的「事件意图」接口，每个都遵循同一套三步：
    //        ① 改 events_ 位图 → ② 调 update() → ③ update() 转发到 Poller 变成一次 epoll_ctl。
    //        所以「开一次读」不是免费的：它是 1 次 epoll_ctl 系统调用（约 1~2us）。
    //        本项目没有任何合并 / 延迟提交优化（可核实：5 个开关函数各自独立调 update()，
    //        连着写 enableReading()+enableWriting() 就是两次 epoll_ctl 系统调用）。
    void enableReading() { events_ |= kReadEvent; update(); }
    // 【语法】events_ |= 0x0003：把 bit0(EPOLLIN) 与 bit1(EPOLLPRI) 置 1，其他位不动（开位用 |=）。
    // 【流程】③ Acceptor::listen()（mainLoop）第一次调它 → 触发 kNew→kAdded 的 EPOLL_CTL_ADD；
    //        ⑦ TcpConnection::connectEstablished()（subLoop#N）第一次调它 → connfd 进 epoll。
    // 【易错】对已经是 kAdded 的 channel 再调 enableReading() 会走 EPOLL_CTL_MOD，不是重新 ADD。
    void disableReading() { events_ &= ~kReadEvent; update(); }
    // 【语法】~kReadEvent = ~0x0003，高 30 位全 1、低 2 位为 0，&= 就是精确关掉这两位（关位用 &= ~）。
    // 【⚠问题】disableReading() 在本仓库没有任何调用点（可核实：全库 grep 只命中这一行定义）：
    //        TcpConnection 要停读一律走 disableAll() 一步清空，属于「接口备而未用」。
    // 【⚠问题】注意这里 int 取反后仍是 int，events_ 是有符号数；当前用的位都在低 16 位，
    //        没踩到符号位，所以行为正确。若将来要加 EPOLLET(0x80000000) 就必须改成 unsigned int，
    //        否则 1<<31 是 UB 且符号位会污染比较。
    void enableWriting() { events_ |= kWriteEvent; update(); }
    // 【线程】ownerLoop 线程（⑨ sendInLoop 在 subLoop#N 上调用它）。
    // 【流程】⑨ TcpConnection::sendInLoop() 一次没写完、数据进 outputBuffer_ 后调它（subLoop#N）；
    //        从此内核发送缓冲区一有空就报 EPOLLOUT，驱动 ⑩ handleWrite 继续搬。
    // 【易错】不能一直开着 EPOLLOUT：水平触发下「可写」是常态， epoll 会每轮都报，CPU 空转打满。
    //        这就是 ⑩ 发完必须 disableWriting() 的原因（见 handleWrite 的注释）。
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    // 【线程】ownerLoop 线程（⑩ handleWrite 发空 outputBuffer_ 后在 subLoop#N 上调用）。
    // 【流程】⑩ TcpConnection::handleWrite() 里 outputBuffer_ 写空后调用（subLoop#N）。
    void disableAll() { events_ = kNoneEvent; update(); }
    // 【线程】ownerLoop 线程：⑪ handleClose / connectDestroyed 在 subLoop#N 上调用；
    //        EventLoop 析构时对 wakeupChannel_ 的调用也在其 ownerLoop 线程（mainLoop 或 subLoop#N）。
    // 【设计】直接赋 0（而不是 &= ~）——一次性清空所有感兴趣的位。
    // 【流程】⑪ TcpConnection::handleClose()、以及 ⑫/连接销毁路径 connectDestroyed() 里调；
    //        此时 Poller 见 isNoneEvent() 为真，会打 EPOLL_CTL_DEL 把 fd 从 interest list 摘掉。
    // 【易错】disableAll() 只是「不再监听」，不等于「从 Poller 里除名」：index_ 会变成 kDeleted，
    //        但 channels_ 这个 map 里还留着这个 fd → 想彻底除名要另外调 remove()。

    // 返回fd当前的事件状态
    // 【设计】下面三个谓词读的是 events_（「我想要什么」），不是 revents_（「发生了什么」）。
    bool isNoneEvent() const { return events_ == kNoneEvent; }
    // 【流程】⑪ disableAll() 之后、⑨/⑦ 注册路径上都会被 EPollPoller::updateChannel 调用来决定 MOD 还是 DEL。
    // 【易错】这里必须用 == 全等判断，不能用 !=0 之类；它是「有没有任何感兴趣的事件」的唯一判据。
    bool isWriting() const { return events_ & kWriteEvent; }
    // 【语法】返回类型是 bool，int 位与的结果（0x4）被隐式转成 true —— 合法但要注意非零即真。
    // 【流程】⑨/⑩ 里 TcpConnection 用它判断「现在是否正在等待 EPOLLOUT」，避免重复 enableWriting；
    //        shutdownInLoop 用它决定能不能安全地 shutdown 写端。
    bool isReading() const { return events_ & kReadEvent; }
    // 【设计】位图的封闭性（可核实）：本项目 events_ 只可能是 0 / 3 / 4 / 7 四种值 —— 只有
    //        kReadEvent(3) 与 kWriteEvent(4) 会被置上；EPOLLERR(0x8)/EPOLLHUP(0x10)/EPOLLRDHUP/
    //        EPOLLET 在全库代码里一个都没出现（只出现在注释里）。所以 revents_ 里带有 events_
    //        里没有的位是【正常现象】：那两个位由内核强制上报。（原版是否有更细的位管理，
    //        以原版仓库为准。）

    int index() { return index_; }
    // 【设计】index_ 是「这个 channel 在 Poller 里的登记状态」，三值 kNew(-1)/kAdded(1)/kDeleted(2)，
    //        常量定义在 src/EPollPoller.cc（不在本头文件！）——因为状态机是 epoll 实现的私事，
    //        换一种 IO 复用（poll/devpoll）状态含义可能完全不同，不该污染 Channel 的接口。
    // 【易错】index_ 不是 bool 也不是位图，不能用 ==true 判断；它必须严格跟着 epoll 的真实状态走，
    //        一旦走偏（比如手工 ADD 过又忘了改），EPOLL_CTL_ADD 会返回 EEXIST 并触发 LOG_FATAL。
    // 【线程】只在 ownerLoop 线程读写（update/remove 都收敛到该线程），所以无需原子变量。
    void set_index(int idx) { index_ = idx; }
    // 【流程】④/⑪ 的注册路径里由 EPollPoller::updateChannel / removeChannel 调用。
    // 【易错】public 的 setter：任何代码都能改它。规矩是「只有 Poller 实现可以改 index_」，
    //        业务层（TcpConnection/Acceptor）绝不要碰。

    // one loop per thread
    // 【设计】整个 muduo 的线程模型口号：一个线程最多一个 EventLoop，一个 Channel 终生属于一个 EventLoop。
    //        ownerLoop() 就是这个「归属关系」的只读出口。
    EventLoop *ownerLoop() { return loop_; }
    //        据流传说法，原版会在 EventLoop::updateChannel / removeChannel 里 assertInLoop()
    //        断言「当前线程 == channel 的 ownerLoop 线程」（本环境无法核实原版源码，以原版仓库为准）。
    // 【⚠问题】本项目虽然提供了 ownerLoop()，但 EventLoop::updateChannel / removeChannel 只是两行
    //        裸转发，一条断言都没有。可核实：整个 include/ 与 src/ 里搜不到任何 assert( 调用 ——
    //        全库【完全没有】用断言，「线程归属」这条最关键的不变式没有任何机器检查，
    //        跨线程误用只能靠读代码自律（这是本项目与「严谨原版」之间的实质差距）。
    //        所以「在别的线程调 enableReading」这种错误不会被抓到，只会静默变成跨线程 epoll_ctl 竞争。
    //        读代码时要把「必须在 ownerLoop 线程操作 channel」当成隐式契约。
    void remove();
    // 【线程】ownerLoop 线程：⑪ connectDestroyed 在 subLoop#N 上调用它。
    // 【流程】⑪ TcpConnection::connectDestroyed() 里调用（在 conn 的 subLoop#N 上），
    //        把 channel 从 Poller 的登记表与 epoll interest list 里彻底摘掉。
    // 【⚠问题】remove() 没有任何前置检查：既不要求 isNoneEvent()，也不看 index_ 是什么。
    //        通行描述：原版这里是 assert(isNoneEvent())（本环境无法核实，以原版仓库为准），
    //        用意是强制「先 disableAll() 再 remove()」这个调用顺序，
    //        本项目去掉断言后仍能工作，但「忘记 disableAll 就 remove」这种用法错误不再报警。
private:

    void update();
    // 【线程】ownerLoop 线程（它只把调用透传给 loop_，自身不做任何线程检查）。
    // 【流程】上面 5 个 enable/disable 的公共尾巴；三层转发链的第一层（见 src/Channel.cc::update）。
    void handleEventWithGuard(Timestamp receiveTime);
    // 【线程】ownerLoop 线程。
    // 【流程】④→⑤/⑦/⑧/⑩/⑪ 的实际分发者；只在 ownerLoop 线程被 handleEvent 调用。

    // ==================== 三个事件常量：为什么「声明在 .h，定义在 .cc」 ====================
    // 【设计】它们的值来自 <sys/epoll.h> 的宏（EPOLLIN/EPOLLPRI/EPOLLOUT）。Channel.h 故意不 include
    //        epoll.h，否则：① 每个包含 Channel.h 的编译单元都被迫吃进 epoll 宏和内核头文件，
    //        编译变慢且污染宏命名空间；② Channel 就成了「epoll 专用」的类，换 poll/devpoll 实现时
    //        全库都要改。所以只在这里声明，具体定义放到唯一真正懂 epoll 位值的地方：src/Channel.cc。
    // 【语法】类内的 static const int 如果不给初值，就只是声明，必须在某个 .cc 里定义一次；
    //        少了那一行，链接期报 undefined reference to Channel::kReadEvent（编译能过，链接才炸）。
    //        这也是「static const 成员变量」和「static constexpr 成员变量」的经典差别：后者可以类内给值。
    static const int kNoneEvent;
    // 【系统调用】epoll 事件位全表（取自本机的 /usr/include/x86_64-linux-gnu/sys/epoll.h，实测值）：
    //   名称          值(16进制)   位     语义 / 谁置它
    //   ---------     --------   ----   -------------------------------------------
    //   EPOLLIN       0x0001     bit0   普通数据可读；对端 FIN 后仍会置位（让你 read 到 0）
    //   EPOLLPRI      0x0002     bit1   带外/高优先级数据（TCP urgent）
    //   EPOLLOUT      0x0004     bit2   可写（发送缓冲有空）
    //   EPOLLERR      0x0008     bit3   出错 —— 内核总会报，注册时写不写都一样
    //   EPOLLHUP      0x0010     bit4   挂断 —— 同上，总是被监听
    //   EPOLLRDHUP    0x2000    bit13   对端关闭或半关闭（本项目没用，见 handleEventWithGuard）
    //   EPOLLET  0x80000000    bit31   边沿触发（本项目全用水平触发 LT，从不置这一位）
    //
    //   kNoneEvent  = 0
    //   kReadEvent  = EPOLLIN | EPOLLPRI = 0x0001 | 0x0002 = 0x0003  → 十进制 3
    //   kWriteEvent = EPOLLOUT                                     = 0x0004  → 十进制 4
    //   读写同时开：events_ = 0x3 | 0x4 = 0x7 → 7；内核报「可读且挂断」：revents_ = 0x3 | 0x10 = 0x13 → 19
    //
    // 【设计】Channel 的位值与 epoll 的位值完全相同，所以 Channel 里存的东西可以直接塞进
    //        epoll_event.events，中间零转换 —— 用「位值兼容」换掉了「转换函数」的开销与出错机会。
    // 【易错】kReadEvent 为什么带上 EPOLLPRI：因为带外数据也算「有东西要读」，如果只注册 EPOLLIN，
    //        收到 urgent 数据时 epoll 不会唤醒，那个字节会一直压在带外指针处读不走。
    static const int kReadEvent;
    static const int kWriteEvent;

    // 【设计】Channel 的数据成员排布。实测 sizeof(Channel)==176 字节（本机 x86-64 + libstdc++，
    //        用同布局的镜像结构体 offsetof 量得；换编译器/平台数字可能变，但相对顺序与对齐规律不变）：
    //   偏移   成员            大小   谁能改                含义
    //   ----   -------------   ----   --------------------   ----------------------------
    //   0      loop_           8      构造后不变             归属的 EventLoop（借用，不拥有）
    //   8      fd_             4      构造后不变(const)      被监听的描述符（所有权在 Socket）
    //   12     events_         4      ownerLoop 线程的 enable/disable   我想听什么（0/3/4/7）
    //   16     revents_        4      EPollPoller::fillActiveChannels   实际发生了什么
    //   20     index_          4      EPollPoller 的 set_index          kNew/kAdded/kDeleted
    //   24     tie_            16     构造/析构期由 shared_ptr 内部维护  weak_ptr<void>（对象 ptr + 控制块 ptr）
    //   40     tied_           1      tie() 置 true，永不清零           是否绑过宿主
    //   41     (padding)       7      -                                把 readCallback_ 顶到 8 字节对齐的 48
    //   48     readCallback_   32     setter                           std::function<void(Timestamp)>
    //   80     writeCallback_  32     setter                           std::function<void()>
    //   112    closeCallback_  32     setter                           std::function<void()>
    //   144    errorCallback_  32     setter                           std::function<void()>
    //   （libstdc++ 的 std::function 是 32 字节、内含指向堆上 _Function_handler 的指针；
    //     绑 std::bind 结果时通常还会在堆上再分配一小块 —— 所以每条连接的 4 个回调是有堆成本的。）
    // 【易错】虚表指针不在这张表里：Channel 没有 virtual 函数（原版 muduo 也没有），
    //        所以 handleEvent 是静态绑定的普通成员函数调用，少一层间接 —— 这也是热路径敢每事件调它的原因。
    EventLoop *loop_; // 事件循环
    // 【设计】裸指针、不拥有：EventLoop 的生命周期由创建者管（mainLoop 是 testserver 栈上对象，
    //        subLoop#N 是 EventLoopThread 栈上对象），它们都比自己的 channel 活得久。
    //        channel 反过来若持 shared_ptr<EventLoop> 就形成环，且语义荒谬。
    //        初始化后终生不变（one loop per thread），这就是它「不需要原子/锁」的根据。
    const int fd_;    // fd，Poller监听的对象
    // 【语法】const 成员 ⇒ 只能在初始化列表里赋值一次 ⇒ Channel 一生绑一个 fd，
    //        也让整个类无法被赋值（这也是禁拷贝之外少有人注意的一点）。
    // 【⚠问题】fd_ 不是拥有关系！Channel 不会关它（见 ~Channel 处的【⚠问题】）。
    //        真正的 owner 是同类的 Socket（TcpConnection 里 socket_ 与 channel_ 同生共死，
    //        ~TcpConnection → ~Socket::close(sockfd_) 才是唯一关闭点）。
    int events_;      // 注册fd感兴趣的事件
    // 【面试】「events_ 和 revents_ 有什么区别？」一句话：events_ 是我向 epoll 登记的「兴趣」
    //        （写进 epoll_event.events，取值只有 0/3/4/7），revents_ 是内核报回来的「事实」
    //        （可能额外带 EPOLLERR/EPOLLHUP），前者由业务改、后者由 Poller 填，两者都在 ownerLoop 线程。
    // 【设计】「我想听什么」。写者：ownerLoop 线程上的 enable/disable；读者：EPollPoller::update。
    int revents_;     // Poller返回的具体发生的事件
    // 【设计】「实际发生了什么」。写者：EPollPoller::fillActiveChannels（④，ownerLoop 线程）；
    //        读者：handleEventWithGuard。它像一个隐式参数：fillActiveChannels 先塞，
    //        handleEventWithGuard 后读，中间靠「同一个线程、同一次循环」串起来 —— 没有函数传参，
    //        是【易错】点：读代码时容易以为 revents_ 是凭空来的。
    // 【设计】这是一次「跨函数隐式传参」：revents_ 就是 Poller 写给 Channel 的参数。
    //        另一种设计是把事件位随 ChannelList 一起传出、不塞进对象（换 IO 复用实现时改动更小）。
    //        本项目选哪种是既定事实（可核实：fillActiveChannels 写、handleEventWithGuard 读）；
    //        原版采用哪种，以原版仓库为准。
    int index_;
    // 【设计】登记状态机，取值 kNew(-1)/kAdded(1)/kDeleted(2)，由 Poller 维护，见 index() 处说明。
    //        它是「用 O(1) 的时间知道该 ADD 还是 MOD」的关键：不存状态就只能每次先 ADD 再 fallback MOD。

    std::weak_ptr<void> tie_;
    // 【设计】弱引用宿主对象：不会延长 TcpConnection 的生命周期，但能问一句「宿主还活着吗」。
    //        类型擦成 void，所以 Channel 不需要知道宿主类型（也不需要 TcpConnection 的头文件）。
    //        若这里用 shared_ptr<void>：channel_ 是 TcpConnection 的成员，TcpConnection 持 channel，
    //        channel 又持 TcpConnection → 循环引用，引用计数永不归零，连接对象与 fd 全部泄漏。
    bool tied_;
    // 【设计】为什么多一个 bool 而不直接判断 tie_ 是否为空：weak_ptr 没有 operator bool，
    //        要判空只能 tie_.owner_before(weak_ptr()) 之类，写起来又慢又难懂；
    //        一个 bool 让 handleEvent 的热路径只读一个字节。
    // 【易错】tied_ 一旦置 true 永不清零（没有 untie()）：这没问题，因为宿主死后 lock() 会返回空。

    // 因为channel通道里可获知fd最终发生的具体的事件events，所以它负责调用具体事件的回调操作
    // 【设计】四个 std::function 是 Channel 与业务层唯一的接缝；它们都是「已绑好 this 的成员函数」，
    //        所以 Channel 完全不认识 TcpConnection/Acceptor，也不需要认识 —— 这就是回调式解耦。
    // 【易错】writeCallback_/closeCallback_/errorCallback_ 是无参的，读回调带 receiveTime；
    //        签名不同所以别名不同（ReadEventCallback vs EventCallback），写错了编译就挂，是好事。
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};