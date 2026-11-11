/* ============================================================================
 * 【文件】include/EventLoop.h
 * 【一句话】声明"一个线程的事件循环"：本线程的 Poller + 活跃 Channel 列表 + 唤醒用
 *          eventfd + 跨线程任务队列，对外只暴露 loop / quit / runInLoop / queueInLoop。
 * 【Reactor 角色】Loop（调度中枢）。注意它不是 Demultiplexer（那是 Poller/epoll），
 *          也不是 Handler（那是 Channel 上挂的回调）；它是把两者粘在一起、
 *          并且规定"本线程的 fd 只允许本线程碰"的那层胶水。
 * 【依赖】noncopyable.h  循环对象拷贝出来没有意义（拷贝了就出现两个线程抢一个 epoll）
 *        Timestamp.h     pollRetureTime_ 的类型（一轮 poll 的共同时间戳）
 *        CurrentThread.h isInLoopThread() 要拿当前线程 tid 比对
 *        <atomic>        looping_/quit_/callingPendingFunctors_ 要跨线程可见
 *        <mutex>         保护 pendingFunctors_
 *        <vector>        activeChannels_ 与 pendingFunctors_
 *        <functional>    Functor = std::function<void()>
 *        <memory>        poller_/wakeupChannel_ 用 unique_ptr 独占
 *        前向声明 Channel/Poller（只用它们的指针，见下面【设计】）
 * 【谁用它】1) main() 在主线程栈上构造 baseLoop（全链路 ①）
 *          2) EventLoopThread::threadFunc 在子线程栈上构造 subLoop（③）
 *          3) Channel 通过它持有的 EventLoop* 反向调 updateChannel/removeChannel
 *          4) TcpServer/TcpConnection 通过 runInLoop/queueInLoop 跨线程投递任务（③⑥⑦⑨⑪⑫）
 * 【先修知识】epoll_wait 的 timeout 单位是毫秒、LT 水平触发；TLS（__thread）线程局部存储；
 *          std::function 的类型擦除；RAII 与对象所有权；"数据竞争"与"可见性"的区别
 * 【难度】★★★★☆   【建议耗时】35 分钟（必须和 src/EventLoop.cc 对着读）
 * 【自检】a) threadId_ 为什么是 const？它在什么时候被赋值、代表什么？
 *        b) 为什么 activeChannels_ 是成员变量，而 doPendingFunctors 里的任务表是局部变量？
 *        c) 哪两个成员共同实现"跨线程唤醒"？为什么光有 fd 还不够、必须再包一个 Channel？
 * ============================================================================ */

#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"

// 【语法】前向声明（forward declaration）而不是 #include "Channel.h" / "Poller.h"：
//   本文件里这两个类型只以指针形式出现，编译器不需要知道它们的大小，声明就够了。
// 【设计】这里必须只前向声明：Channel.h 里 #include "EventLoop.h"（Channel 要调
//   loop_->updateChannel），EventLoop.h 若反过来 include Channel.h 就成环，
//   展开后有一边拿到的是不完整类型，直接编译失败。
// 【易错】unique_ptr<Poller>/unique_ptr<Channel> 的析构要求 Poller/Channel 是完整类型，
//   所以 ~EventLoop() 的定义只能放在 src/EventLoop.cc（那里 include 了两者）。
//   如果哪天有人把析构函数改成头文件里的 inline 空实现，就会报 "invalid use of incomplete type"。
class Channel;
class Poller;

// 【一句话】EventLoop = 本线程的 epoll 循环 + 本线程的任务收件箱。
// 【设计】one loop per thread 的三条落地手段全在这个类里：
//   (1) threadId_ 在构造时记下"建我的线程"，从此 loop 就认这个线程；
//   (2) src/EventLoop.cc 顶部的 __thread EventLoop* t_loopInThisThread 保证
//       同一线程最多登记一个 loop（第二个直接 LOG_FATAL）；
//   (3) 所有 fd 的增删改查只能经 updateChannel/removeChannel 转到"本 loop 的 poller_"，
//       于是"fd 注册进哪个 epoll"和"哪个线程在 epoll_wait"天然一致，不需要加锁。
// 【易错】EventLoop 不是线程！它不创建线程、不持有 std::thread。
//   想要"多个线程各跑一个 loop"，要的是 EventLoopThread / EventLoopThreadPool。
// 【对比原版】可核实的本地事实：在 git HEAD 里 grep 不到 TimerQueue / runAfter /
//   queueSize / assertInLoopThread / stopAt 这些标识符（一条命令自证：
//   git grep -l TimerQueue HEAD 无输出），也就是说教学版把"定时器队列 +
//   线程归属自检 + 队列积压监控"三类能力整片拿掉了，EventLoop 只剩纯粹的
//   事件循环骨架，成员也少了一半。
// 【口径说明】本环境无法联网核对陈硕原版仓库：凡"本仓库有/没有"的判断都可以用
//   git grep HEAD 当场验证；凡涉及"原版具体怎么写"的句子只作设计对比，
//   具体以原版源码为准。本子系统所有【对比原版】都按这个口径读。
// 事件循环类 主要包含了两个大模块 Channel Poller(epoll的抽象)
class EventLoop : noncopyable
{
public:
    // 【语法】成员 using：这个别名的全名是 EventLoop::Functor，写在类里是为了让它
    //   跟着类走（外部用 EventLoop::Functor 拿到同一个类型，和 Poller::ChannelList 同理）。
    // 【设计】跨线程任务统一成"无参数、无返回值的可调用对象"，所有需要的上下文
    //   在投递前就用 std::bind 绑好并按值拷进 std::function —— 这正是 muduo 线程安全的根基：
    //   任务里绝不引用调用方栈上的东西（否则调用方函数返回后就是悬垂引用）。
    // 【面试】为什么用 std::function 而不是模板参数？因为要装进同一个 vector，
    //   必须统一类型（类型擦除）；代价是可能的堆分配 + 一次间接调用，
    //   对"每个连接几个任务"的量级完全无所谓。
    using Functor = std::function<void()>;

    // 【流程】①（mainLoop：在主线程栈上）或 ③（subLoop：在 EventLoopThread 的子线程里）。
    // 【线程】"构造发生在哪个线程，loop 就归哪个线程"——见 threadId_。
    // 【易错】所以不能写成"在主线程 new EventLoop，然后把指针传给子线程去 loop()"：
    //   那样 threadId_ 记的是主线程，子线程里 isInLoopThread() 永远为假，
    //   整个 runInLoop 全退化成排队，而队列没人消费 → 连接永远不会建立。
    EventLoop();
    ~EventLoop();

    // 开启事件循环
    // 【线程】只能在 loop 所属线程调用：本仓库没有任何线程归属断言
    //   （git grep assertInLoopThread HEAD 无结果），跑错线程不会报错，只会静默失效。
    // 【流程】④：loop() 是 mainLoop 与 subLoop 共用的发动机 = poll → 派发 → 跑任务队列。
    // 【易错】loop() 内部是 while(!quit_)，它会阻塞、几乎不返回。
    //   所以 main() 里写 loop.loop(); 之后的语句，只有服务器退出后才会执行；
    //   想在"启动之后"做点事，得靠 runInLoop 或线程初始化回调，不能写在 loop() 后面。
    // 【设计】一个 EventLoop 对象可以"退出后再 loop()"吗？本项目允许（quit_ 会被重置回 false），
    //   但请把它当一锤子买卖：quit_ 复位后确实能再 loop 一次，只是没有任何代码路径依赖这个用法。
    void loop();
    // 退出事件循环
    // 【线程】任意线程都可调。别的线程调 → 必须 wakeup() 把对方从 epoll_wait 里踹醒；
    //   本线程调 → 只写标志位即可（回到 while 条件自然看得见）。
    // 【流程】⑫ 退出的唯一起点。
    // 【易错】它是"请求退出"，不是"已经退出"：本轮剩下的 handleEvent 和
    //   doPendingFunctors 还是会把所有活跃事件处理完，然后才在 while 判定处退出。
    void quit();

    // 【语法】在类内定义的成员函数默认就是 inline（隐式），可以放在头文件而不违反 ODR。
    // 【设计】把"poll 返回的那一刻"当作这一批 activeChannels 共同的到达时间，
    //   一个 fd 一次时间获取都省了；代价是精度 = 一轮派发的耗时（陈硕明确接受的取舍）。
    // 【易错】成员拼写是 pollRetureTime_（源码笔误 Reture），接口名却是 pollReturnTime，
    //   grep 的时候两个拼法都要试；const 修饰表示它不改动对象，可在 const 语境调用。
    Timestamp pollReturnTime() const { return pollRetureTime_; }

    // 在当前loop中执行
    // 【流程】③ loop_->runInLoop(Acceptor::listen)；⑥⑦ ioLoop->runInLoop(connectEstablished)；
    //   ⑪ TcpServer::removeConnectionInLoop 之后 ioLoop->runInLoop/queueInLoop(connectDestroyed)。
    // 【线程】在 loop 线程调 → 当场同步执行完才返回；在别的线程调 → 等价于 queueInLoop（只投递）。
    // 【设计】它是"把操作搬回数据的家"的通用手段：上层任何线程想动 TcpConnection，
    //   都包成 Functor 交给这个连接所属的 loop 去跑，于是 TcpConnection 内部一行锁都不用写。
    // 【易错】本线程调用是"立刻插队执行"，会打断当前事件的正常顺序，还会把你的调用栈压深一层；
    //   如果一个回调里对同一个 loop 再 runInLoop 自己，就是递归，注意终止条件。
    void runInLoop(Functor cb);
    // 把上层注册的回调函数cb放入队列中 唤醒loop所在的线程执行cb
    // 【设计】"投递，不执行"：加锁入队 + 视情况 wakeup，然后立刻返回，永不等待结果。
    //   它是 muduo 里唯一的跨线程动作原语；runInLoop 的非本线程分支、send() 的跨线程分支，
    //   最后都落到这里（见 src/EventLoop.cc 的 queueInLoop 精读）。
    // 【线程】任意线程；锁只保护 pendingFunctors_，且锁里只有一句 emplace_back。
    // 【面试】"用户线程能不能直接 write 一个 connfd?" —— 不应当：把写动作 queueInLoop
    //   到该连接所属的 loop 线程，才能和其他 IO 事件串行化，避免加锁保护 socket。
    void queueInLoop(Functor cb);

    // 通过eventfd唤醒loop所在的线程
    // 【系统调用】往自己的 eventfd 写 8 字节：内核把计数器加上 1，并让该 fd 变为可读，
    //   于是阻塞在 epoll_wait 的目标线程带着 EPOLLIN 返回。全程无锁、无 CAS、不会丢事件。
    // 【线程】任意线程，包括 loop 线程自己（自唤醒，见 queueInLoop 里 callingPendingFunctors_）。
    // 【易错】public 暴露出来是教学版的选择：上层可以"提前敲门"让 loop 空转一轮；
    //   正常业务只需要 runInLoop/queueInLoop，别把 wakeup 当业务通知用。
    void wakeup();

    // EventLoop的方法 => Poller的方法
    // 【设计】外观模式（Facade）：Channel 只认识 EventLoop，完全不认识 Poller/epoll。
    //   完整链路 Channel::update() -> EventLoop::updateChannel -> Poller::updateChannel -> epoll_ctl。
    //   收益：(1) Channel 的构造参数只要 (EventLoop*, fd)，不需要再传 Poller*；
    //        (2) 想加"必须在 loop 线程"之类的检查，只改这一层就全覆盖；
    //        (3) 换 epoll/poll/select 实现，业务代码一行不动。
    // 【线程】只允许在 loop 所属线程调用。EPollPoller 里的 fd->Channel map 没有锁，
    //   安全性完全靠"只有 loop 线程会走到这里"这条约定。
    // 【易错】没有加锁不是遗漏，是契约；破坏契约（比如在用户线程直接调 updateChannel）
    //   不会报错，只会在压测时随机崩，是最难查的一类 bug。
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
    // 【⚠问题】声明留在这里，但 src/EventLoop.cc 里对应定义整段被注释掉了。
    //   现在全库没人调它，所以链接得过；任何人写一句 loop->hasChannel(ch)，
    //   要到**链接期**才会炸出 undefined reference（编译期看不出问题）。
    //   同一个坑在 include/Poller.h 里也有一份（见对应文件的注释），两处要一起看。
    bool hasChannel(Channel *channel);

    // 判断EventLoop对象是否在自己的线程里
    // 【线程】全库判断"要不要跨线程"的唯一依据，被 runInLoop / queueInLoop / quit /
    //   TcpConnection::send / shutdown 高频调用。
    // 【设计】threadId_ = 构造时那个线程的 tid ⇒ "谁 new 出这个 EventLoop，它就归谁"。
    //   CurrentThread::tid() 读的是 __thread 缓存值，只在第一次才 syscall(SYS_gettid)，
    //   所以可以放心当 if 条件反复调用，这不是性能敏感点。
    // 【面试】为什么不拿 std::this_thread::get_id() 比较？因为 std::thread::id 是 pthread
    //   内部的不透明值，既不等于内核 tid（gettid 返回的 pid_t），也没法稳定打进日志；
    //   muduo 全程用 pid_t tid，连 Logger 打的也是它。
    // 【语法】只读查询所以标 const：不标也能编译，但标了之后 const EventLoop& 这种语境
    //   也能调它，同时向读者声明"它不改状态"——它在全库热路径上被反复调用，
    //   这也是敢把它当廉价 if 条件用的依据。
    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); } // threadId_为EventLoop创建时的线程id CurrentThread::tid()为当前线程id

private:
    // 【流程】④/⑦ 内部的一环：wakeupFd_ 上出现 EPOLLIN -> Channel::handleEvent
    //   -> 命中 readCallback_（构造时绑好的就是它）-> 这里。
    // 【线程】只在 loop 自己的线程执行，因为它是由本线程的 epoll 派发出来的。
    // 【设计】放 private：它是 eventfd 的实现细节，和上层的"读事件回调"是两回事，
    //   不能让业务代码误以为可以在这里读自己的 socket。
    void handleRead();        // 给eventfd返回的文件描述符wakeupFd_绑定的事件回调 当wakeup()时 即有事件发生时 调用handleRead()读wakeupFd_的8字节 同时唤醒阻塞的epoll_wait
    // 【设计】doPendingFunctors 是 loop 的私活：外部只能通过 runInLoop/queueInLoop
    //   间接影响它，永远不能自己跑任务队列 —— 否则就不止一个线程在动 TcpConnection 了。
    void doPendingFunctors(); // 执行上层回调

    // 【语法】又一次成员 using 起别名。Poller.h 里也有一个同名 ChannelList
    //   （也是 std::vector<Channel*>），两边靠"结构相同"对接，不是靠继承或 typedef 传递。
    //   【易错】正因如此，EPollPoller::poll(kPollTimeMs, &activeChannels_) 能编译，
    //   靠的是两个别名指向同一个类型；若哪天把 Poller 的容器换成 list，这里就悄悄编译不过。
    using ChannelList = std::vector<Channel *>;

    // 【设计】looping_ 表示"loop() 正在跑"，由 loop 线程自己写、别的线程可读（用于诊断）；
    //   两个都声明成 atomic_bool，是为了让跨线程读不构成数据竞争且能看到最新值。
    // 【易错】atomic 只保证"单次读/写无竞争 + 可见性"，不保证组合操作的原子性：
    //   loop() 里 looping_=true → ……循环…… → looping_=false 是一段非原子的状态迁移，
    //   期间并发调用 quit() 依然要靠 wakeup 才能及时生效。
    // 【语法】"底层通过 CAS 实现"这句原注释只对了一半：单纯 store/load 用不上 CAS，
    //   只有 read-modify-write（如 fetch_add、compare_exchange）才会用到 cmpxchg 指令；
    //   这里没有一处 CAS，atomic 的意义在内存序。
    // 【设计】选 atomic_bool 的动机是"这两位确实会被别的线程读到"（quit_ 由外部线程写，
    //   looping_ 可能被诊断代码读）。但注意 atomic 默认是 seq_cst，比实际需要的强；
    //   这里没有无锁队列、没有 CAS 循环，atomic 提供的是"不产生数据竞争 + 看得到最新值"，
    //   而不是"这些位的组合是原子的"——依赖组合语义的地方（loop 的进入/退出）依然不成立。
    std::atomic_bool looping_; // 原子操作 底层通过CAS实现
    // 【流程】⑫ 的唯一开关：由 quit() 置位，由 loop() 的 while 条件消费。
    // 【易错】loop() 进门第一件事就是 quit_ = false，所以在 loop() 跑起来之前调 quit()
    //   会被抹掉（见 src/EventLoop.cc 里 quit()/loop() 两处的详细推演）。
    std::atomic_bool quit_;    // 标识退出loop循环

    // 【语法】const 成员 ⇒ 只能在构造函数的初始化列表里赋值，一旦构造完成永不改变；
    //   副作用是 EventLoop 不可移动赋值（这也是我们想要的：loop 搬家等于线程归属错乱）。
    // 【设计】它就是 one loop per thread 的"身份证"：isInLoopThread() 与它比，
    //   日志与它比，EventLoopThread/Pool 收集 subLoop 时也靠它确认归属。
    // 【易错】单位是内核 tid（syscall(SYS_gettid) 的 pid_t），不是 pthread_t，
    //   也不是 std::thread::id —— 三者都不能互相比较，别混用。
    const pid_t threadId_; // 记录当前EventLoop是被哪个线程id创建的 即标识了当前EventLoop的所属线程id

    // 【设计】poller_ 用 unique_ptr 独占：Poller 内含 epollfd 和 fd->Channel 映射表，
    //   生命周期必须严格等于 EventLoop，且不能被第二个线程共享（一个 epoll 实例只服务一个线程）。
    // 【顺序】构造时是 Poller::newDefaultPoller(this) —— 把还没构造完的 this 交给别人存起来，
    //   属于"构造期泄漏 this"；这里安全的原因是 DefaultPoller/EPollPoller 只把指针存进成员，
    //   构造期间并不解引用它（真正用到是在后面的 updateChannel）。
    // 【易错】poller_ 必须排在 wakeupFd_/wakeupChannel_ 之前初始化（头文件声明顺序决定），
    //   否则构造 wakeupChannel_ 时调 enableReading() 会用到空的 poller_ → 段错误。
    // 【易错】pollRetureTime_ 是默认构造（内部微秒值 0）：第一次 poll 返回之前调
    //   pollReturnTime() 拿到的是 0，别读成"1970 年到的一批数据"，它只表示"还没 poll 过"。
    Timestamp pollRetureTime_; // Poller返回发生事件的Channels的时间点
    std::unique_ptr<Poller> poller_;

    // 【设计】wakeupFd_ 和 wakeupChannel_ 必须成对出现：eventfd 只是一个 fd，
    //   而 epoll 派发的单位是 Channel —— 所以给这个 fd 包一层 Channel 并挂上 handleRead，
    //   "内核里的计数器"才算接进了事件派发体系。
    // 【流程】⑥【线程跳转 1】、⑪【线程跳转 2/3】的最后一公里：queueInLoop → wakeup()
    //   → 目标 loop 的 epoll_wait 带着 wakeupFd_ 的 EPOLLIN 返回 → handleRead 清计数。
    // 【易错】wakeupFd_ 不参与业务 IO，不会出现在 activeChannels_ 之外的任何列表里；
    //   它是 EventLoop 自己的"门铃"，一个 loop 一个，永不共享、永不 delete。
    int wakeupFd_; // 作用：当mainLoop获取一个新用户的Channel 需通过轮询算法选择一个subLoop 通过该成员唤醒subLoop处理Channel
    std::unique_ptr<Channel> wakeupChannel_;

    // 【设计】成员而不是局部变量的两个理由：
    //   (1) EPollPoller::poll 是"往调用方给的容器里 push_back"的填装语义，
    //       传成员进来可以复用容量 —— 第 2 轮起零堆分配（一天百万次 epoll_wait 很划算）；
    //   (2) 它只被 loop 线程读写，天然单线程所有权，做成成员不需要任何锁。
    // 【易错】正因为 poll 是追加语义，loop() 每轮开头必须 clear()，
    //   漏掉就会重复派发上一轮的 Channel（同一个 fd 的回调被跑两次）。
    // 【易错】本仓库没有任何"当前正在派发哪个 Channel"的记录（git grep eventHandling_ HEAD、
    //   git grep currentActiveChannel_ HEAD 都无结果），所以"在回调里销毁正被遍历的 Channel"
    //   这条危险路径完全没设防：activeChannels_ 里会留着一个可能已悬垂的裸指针，
    //   直到下一轮 clear 才被覆盖。本库唯一的保护是 Channel::tie()（weak_ptr 提升失败
    //   就不调回调，见 src/Channel.cc）+ TcpConnection 的 shared_ptr 引用计数。
    //   更严谨的实现会加"派发期诊断成员 + 断言"（具体写法以原版源码为准）。
    ChannelList activeChannels_; // 返回Poller检测到当前有事件发生的所有Channel列表

    // 【设计】一个标准的生产者/消费者模型：所有线程都是生产者（queueInLoop 里 emplace_back），
    //   loop 线程是唯一的消费者（doPendingFunctors 一次 swap 走整批）。
    //   mutex_ 的保护范围只有 pendingFunctors_ 这一个容器 —— 全 EventLoop 只有这一把锁。
    // 【易错】别把它当成"整个 EventLoop 的锁"：looping_/quit_/callingPendingFunctors_
    //   靠 atomic，activeChannels_ 靠单线程独占，都不在这把锁里。
    // 【面试】"muduo 的 EventLoop 有几把锁？主循环路径上有锁吗？"
    //   一把，只盖任务队列；epoll_wait 与事件派发全程无锁 —— 因为唤醒用的是 eventfd
    //   而不是 condition_variable，这是 muduo 高性能的关键之一。
    // 【设计】这把锁还能做得更细（无锁队列，或给静态分析工具加"该成员受此锁保护"的注解；
    //   上游 muduo 属于后一种思路，细节以原版源码为准）。
    //   教学版选"一把 std::mutex 盖住一个 vector"，胜在临界区边界一眼可见。
    std::atomic_bool callingPendingFunctors_; // 标识当前loop是否有需要执行的回调操作
    std::vector<Functor> pendingFunctors_;    // 存储loop需要执行的所有回调操作
    std::mutex mutex_;                        // 互斥锁 用来保护上面vector容器的线程安全操作
};