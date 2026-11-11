/* ============================================================================
 * 【文件】src/EventLoop.cc
 * 【一句话】实现"一个线程的 epoll 循环"：建一个 eventfd 当门铃，每轮 poll 收活跃
 *          Channel、逐个派发回调，最后把别的线程（或自己）塞进来的任务整批执行掉。
 * 【Reactor 角色】Loop（调度中枢）：向上驱动 Channel/Poller，向下承接
 *          TcpServer/TcpConnection 的所有跨线程投递。它是整个库的心脏起搏器。
 * 【依赖】<sys/eventfd.h> 创建唤醒 fd；<unistd.h> read/write/close；
 *        <fcntl.h> 提供 O_* 一类标志（本文件其实没直接用到，留着无害）；
 *        <errno.h> 失败原因；<memory> unique_ptr 成员的析构需要完整类型；
 *        "EventLoop.h" 自己的声明；"Logger.h" LOG_* 宏；
 *        "Channel.h" 构造 wakeupChannel_ 与派发 handleEvent；
 *        "Poller.h" Poller::newDefaultPoller 与 poll/updateChannel/removeChannel。
 * 【谁用它】main() 造 baseLoop（①）；EventLoopThread::threadFunc 造 subLoop（③）；
 *          Channel::update/remove 回调到 updateChannel/removeChannel；
 *          TcpServer/TcpConnection 调 runInLoop/queueInLoop/quit（③⑥⑦⑨⑪⑫）。
 * 【先修知识】epoll_wait 的 timeout 单位（毫秒）与 LT 语义；eventfd 计数器语义；
 *          __thread 线程局部存储；std::mutex 不可重入；vector::swap 是 O(1) 换缓冲区；
 *          内存可见性（写线程的 store 何时对读线程可见）。
 * 【难度】★★★★★ 全库最难 210 行。建议按"TLS → eventfd → 构造顺序 → loop 四步 →
 *          跨线程三件套(runInLoop/queueInLoop/wakeup) → doPendingFunctors"六段读。
 *          【建议耗时】90 分钟，注释量比代码多，别跳。
 * 【自检】a) 一个线程能创建两个 EventLoop 吗？靠哪个变量、在哪个函数里强制？
 *        b) kPollTimeMs 取 10 秒会不会让 quit() 等 10 秒？分几种情况说清楚。
 *        c) queueInLoop 的判断条件里 callingPendingFunctors_ 少了会出什么事？
 *        d) doPendingFunctors 为什么把任务 swap 到局部 vector？说出两个理由。
 *        e) 为什么 handleRead 必须把那 8 字节读走？不读会怎样？
 * 【口径说明】涉及"原版 muduo 怎么写"的句子只作设计对比：本环境无法联网核对原版
 *   仓库，具体以原版源码为准；而"本仓库有/没有某标识符"这类判断都能用
 *   git grep -l 名字 HEAD 当场自证。读下面所有【对比原版】时按这个口径。
 *
 * ============================================================================ */

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <memory>

// 【语法】项目自己的头文件用引号形式、尖括号形式在前，符合"先稳定后本地"的惯例；
//   本仓库没有强制 include 顺序，但每个 .cc 的第一个 include 必须能独立编译（自足性），
//   这一组就满足：EventLoop.h 只前向声明了 Channel/Poller，所以这里必须把它们补全。
#include "EventLoop.h"
#include "Logger.h"
#include "Channel.h"
#include "Poller.h"

/* ---------------------------------------------------------------------------
 * 【TLS 深度讲解】__thread EventLoop* t_loopInThisThread —— one loop per thread 的落地手段
 *
 * 1) 语义：它不是"全局变量"，而是"每个线程各有一份独立副本"的线程局部存储
 *    （Thread-Local Storage）。变量名相同，实体不同：主线程看到的那份指针和
 *    subLoop#1、subLoop#2 看到的完全不是一回事。因此它天生就是
 *    "本线程有没有 EventLoop"的登记表，一张表一个线程，读写都不需要锁。
 *
 * 2) 底层机制：编译器把这种变量放进 .tdata/.tbss 段，运行期通过 %fs 段寄存器做
 *    fs 相对寻址（x86-64），取值就是一条 mov 指令，没有查表、没有 syscall、
 *    没有 pthread_getspecific 的函数调用。这就是 TLS 比
 *    "std::unordered_map<pid_t, EventLoop*>" 快的根本原因（后者还得加锁）。
 *
 * 3) __thread vs thread_local【语法】面试常问：
 *    - __thread 是 GCC 扩展，要求对象是平凡类型（POD、常量初始化、静态存储期），
 *      不会调用构造/析构函数；
 *    - thread_local 是 C++11 标准写法，支持任意类型（比如放个 std::string），
 *      代价是要 TLS 守卫函数做懒初始化和注册析构，访问比 __thread 慢一截。
 *    这里存的是裸指针，平凡类型，所以用 __thread 最省。
 *    本仓库 CurrentThread::t_cachedTid 用 __thread 是同一个理由。
 *
 * 4) 它如何实现"一个线程只有一个 EventLoop"：构造函数里先读自己线程那份副本，
 *    非空说明"我这个线程已经有一个 loop 了"，直接 LOG_FATAL 退出；
 *    空则把 this 登记进去。析构函数里再清空，线程可以再建一个新的。
 *    因为每个线程独立一份，这个"检查 + 赋值"不存在竞态，也不需要 atomic。
 *
 * 5) 【易错】它只保证"同一线程内唯一"，不保证"整个进程唯一"——8 个线程各建一个
 *    EventLoop 完全合法，这正是 EventLoopThreadPool 在做的事。
 *    也【易错】于把它当成"线程到 loop 的映射表"用：它是 TLS，别的线程读不到你的那份，
 *    所以 muduo 想知道"某个线程的 loop"只能靠对象自己记（EventLoop::threadId_）。
 *
 *6) 【语法】这里的初值必须是编译期常量（写 nullptr 正好）：__thread 这类 TLS 变量走
 *    常量初始化，改成 __thread EventLoop* p = someFunc(); 这种需要动态初始化的形式，
 *    要么被编译器直接拒绝，要么（thread_local 路径）多插一个 TLS 初始化守卫函数。
 *    所以"TLS 里存裸指针或整型"是既省又稳的用法，本文件与 CurrentThread::t_cachedTid
 *    都停在这一档。
 *
 *7) 【⚠问题】本仓库的强制手段是 LOG_FATAL，而 include/Logger.h 里 LOG_FATAL 的收尾是
 *    exit(-1)（可核实）：不走 RAII 析构、不保证 std::cout 缓冲被刷出去。
 *    用在这一处后果有限——进程一退，内核统一回收 epollfd/eventfd，谈不上泄漏；
 *    但"构造函数里 exit"意味着其他 loop、TcpServer、已有连接连清理的机会都没有，
 *    而且最后那条日志可能连同缓冲区一起丢掉（A1 讲 Logger 时会展开）。
 *    真正的保障始终是纪律：EventLoop 只在"将来要跑 loop() 的那个线程"内部构造。
 * --------------------------------------------------------------------------- */
// 防止一个线程创建多个EventLoop
__thread EventLoop *t_loopInThisThread = nullptr;

// 【设计】单位是毫秒，会被原样当作 epoll_wait 的 timeout 实参（EPollPoller::poll 里传下去）。
//   三档语义：>0 最多睡这么久；0 立即返回（忙轮询）；-1 一直睡到有事发生。这里取 10 秒。
//   它不是全局变量而是文件内 const（const 在 C++ 里默认内部链接），
//   所以别的 .cc 文件即使有个同名 kPollTimeMs 也不会撞车。
// 【⚠问题】10 秒的代价要说清楚（面试高频"muduo 的 poll 超时为什么是 10 秒"）：
//   - 跨线程 quit() 会 wakeup()，所以"退出"不会真的等 10 秒；
//   - 但任何"只改共享状态、不 wakeup"的通知，最坏要 10 秒后才被看到。
//     本库里所有跨线程动作都走 queueInLoop（它会 wakeup），所以闭环是完整的；
//     如果你自己加一个"直接改某个成员变量让 loop 线程感知"的功能，就会踩这 10 秒。
//   - 想做"100ms 检查一次超时连接"这种定时任务，靠 poll 超时是做不到精度的，
//     必须往同一个 epoll 里注册一个 timerfd 并 enableReading。
// 【设计/延伸】本仓库把定时器整个删掉了（git grep TimerQueue HEAD 无结果），
//   所以这里的 10 秒真的只是"完全没事时睡多久"。
//   将来要加定时任务，正确做法不是把 10 秒改小（那只会变成周期性空转），
//   而是往同一个 epoll 里注册一个 timerfd 并 enableReading，让内核到点替你"敲门"——
//   机制和 wakeupFd_ 一模一样，这也是本文件最值得反复琢磨的地方。
// 定义默认的Poller IO复用接口的超时时间
const int kPollTimeMs = 10000; // 10000毫秒 = 10秒钟

/* 创建线程之后主线程和子线程谁先运行是不确定的。
 * 通过一个eventfd在线程之间传递数据的好处是多个线程无需上锁就可以实现同步。
 * eventfd支持的最低内核版本为Linux 2.6.27,在2.6.26及之前的版本也可以使用eventfd，但是flags必须设置为0。
 * 函数原型：
 *     #include <sys/eventfd.h>
 *     int eventfd(unsigned int initval, int flags);
 * 参数说明：
 *      initval,初始化计数器的值。
 *      flags, EFD_NONBLOCK,设置socket为非阻塞。
 *             EFD_CLOEXEC，执行fork的时候，在父进程中的描述符会自动关闭，子进程中的描述符保留。
 * 场景：
 *     eventfd可以用于同一个进程之中的线程之间的通信。
 *     eventfd还可以用于同亲缘关系的进程之间的通信。
 *     eventfd用于不同亲缘关系的进程之间通信的话需要把eventfd放在几个进程共享的共享内存中（没有测试过）。
 */
// 创建wakeupfd 用来notify唤醒subReactor处理新来的channel
/* 【系统调用】eventfd 完整语义（面试可以按这个顺序讲满三分钟）
 * 内核里维护一个 8 字节无符号计数器（主机字节序），eventfd() 返回指向它的 fd：
 *   write(fd, 值, 8) ：计数器 += 值。如果加下去会溢出（上限 0xfffffffffffffffe），
 *                     阻塞等待，带 EFD_NONBLOCK 则返回 -1/EAGAIN。
 *   read(fd, &值, 8) ：计数器不为 0 时，读出当前值并把计数器清零
 *                     （带 EFD_SEMAPHORE 时读出 1、计数器减 1，退化成信号量语义）；
 *                     计数器为 0 时阻塞，带 EFD_NONBLOCK 则 -1/EAGAIN。
 *   epoll/poll       ：计数器 != 0 → 可读 EPOLLIN；几乎永远可写 EPOLLOUT，
 *                     所以绝对不要给 eventfd 注册写事件，否则会疯狂空转。
 * 最关键的一条性质：N 次 write 会被内核"合并"成一次可读 + 一次 read 取走累计值，
 *   ⇒ 唤醒事件不会丢，也不需要像 pipe 那样循环读空。
 * 【设计】initval 取 0：如果初始就非 0，EventLoop 一建好 epoll 立刻虚醒一次，
 *   白跑一轮 handleRead。从 0 开始才准确表达"当前没有待处理的唤醒"。
 * 【设计】flags 里的 EFD_NONBLOCK：保证 handleRead 里那次 read 永远不会阻塞。
 *   一个事件循环里出现任何可能阻塞的 syscall 都是设计缺陷（会把整个线程、
 *   也就是这个线程上所有连接一起挂死）。
 * 【设计】flags 里的 EFD_CLOEXEC：exec 家族调用时自动关闭该 fd，避免 exec 后的新程序
 *   继承到我们的门铃 fd。（注意：本文件上方原有注释把 EFD_CLOEXEC 说反了 ——
 *   【易错】它影响的是 exec 而不是 fork；fork 出的子进程本来就会带着 fd 的副本，
 *   真正危险的是 exec 后 fd 泄漏给毫不相干的程序。EFD_CLOEXEC = 等价于 FD_CLOEXEC +
 *   O_CLOEXEC，用 flags 一次搞定，省掉"先 eventfd 再 fcntl"的竞态窗口。）
 * 【面试】为什么不用 mutex + condition_variable 通知 loop 线程？
 *   答：loop 线程 99% 的时间阻塞在 epoll_wait(epollfd) 上，它只能被"某个 fd 可读"叫醒，
 *   不可能同时被 pthread_cond_wait 叫醒 —— 一个线程只有一个阻塞点。
 *   可选方案只有：(a) 换成"可被 epoll 监听的 fd"来传递通知（pipe / eventfd / signalfd / socketpair）；
 *   (b) cond_.wait 带短超时轮询（浪费 CPU 且有固定延迟）；(c) 把 epoll_wait 放到 timeout=-1
 *   然后用 write 到 self-pipe（本质还是 (a)）。eventfd 是 (a) 里最省的：
 *   只占 1 个 fd（pipe 要 2 个）、自带 8 字节计数器与合并语义、不会写满（pipe 64KB 满就阻塞）、
 *   read 一次就清空。所以 muduo 选它。
 * 【⚠问题】失败路径是 LOG_FATAL，而本项目的 LOG_FATAL 收尾是 exit(-1)（Logger.h 可核实）。
 *  eventfd 失败通常是 EMFILE（fd 用尽）或 ENOMEM，属于"这个进程快不行了"的信号，
 *  更合适的是 abort() 以便留下 core dump 验尸；上游怎么写以原版源码为准。
 */
// 【一句话】给本 EventLoop 造"门铃"，返回 fd；失败直接 LOG_FATAL 退出。
// 【设计】为什么是文件内的自由函数而不是成员：它只在构造函数初始化列表里被调用一次，
//   那一刻 this 还不可用（成员还没构造完），所以只能是自由函数。
// 【易错】它是非 static 函数 ⇒ 符号会出现在 libmuduo_core.so 的动态符号表里。
//   如果别的翻译单元或动态库也有同名 createEventfd，链接期可能发生符号抢占
//   （谁先绑定谁生效），表现是"我改了这个文件却没生效"。规范做法：加 static
//   或放进匿名 namespace。对照同文件里的 kPollTimeMs：const 整量在 C++ 里
//   默认内部链接，就没有这个问题。
int createEventfd()
{
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0)
    {
        LOG_FATAL("eventfd error:%d\n", errno);
    }
    return evtfd;
}

/* 【流程】①（mainLoop，主线程栈上）或 ③（subLoop，EventLoopThread 的子线程栈上）。
 * 【语法】C++ 里非静态数据成员的初始化顺序永远是"类内声明顺序"，与初始化列表里的
 *   书写顺序无关（写反了 -Wreorder 会给警告）。本项目的声明顺序是
 *   looping_ → quit_ → threadId_ → pollRetureTime_ → poller_ → wakeupFd_ →
 *   wakeupChannel_ → activeChannels_ → callingPendingFunctors_ → pendingFunctors_ → mutex_；
 *   而列表里把 callingPendingFunctors_ 写在第三个，它实际是第九个被构造的。
 *   这里没有后果（全是 false/0 的平凡初始化），但依赖顺序时就会出事。
 * 【顺序】为什么列表内部顺序"事实上"是对的：poller_ 在 wakeupFd_ 前，
 *   wakeupFd_ 在 wakeupChannel_ 前 —— Channel(this, wakeupFd_) 要读 wakeupFd_ 的值，
 *   紧接着的 enableReading() 要用 poller_。声明顺序只要错一位就是野指针/段错误。
 * 【设计】把"每个 EventLoop 都必然拥有的四件家当"（poller、门铃 fd、门铃 Channel、
 *   任务队列）全放进构造函数，是为了让"构造完成 = 可以安全 loop()"成为不变量，
 *   上层不需要 Init() 二段式初始化（EventLoopThread 才有 startLoop 那种二段式）。
 */
EventLoop::EventLoop()
    : looping_(false)
    , quit_(false)
    , callingPendingFunctors_(false)
    , threadId_(CurrentThread::tid())
    , poller_(Poller::newDefaultPoller(this))
    , wakeupFd_(createEventfd())
    , wakeupChannel_(new Channel(this, wakeupFd_))
{
    // 【设计】全库唯一一处把 loop 地址与它的所属线程放在一起的信息（排查多 loop 场景最有用）。
    //   两个 %p/%d 参数顺序要和格式串对齐（this 是 %p，threadId_ 是 %d，pid_t 就是 int）。
    // 【易错】但只要没定义 MUDEBUG，LOG_DEBUG 就是**空宏**（与 Debug/Release 无关，本项目三份
    //   CMakeLists 从来没定义它），所以这行默认等于不存在；实测连里面的实参副作用都会被整体丢弃。
    //   因此运行时真正看得到的只有 loop() 里那句 LOG_INFO("EventLoop %p start looping")：
    //   它只有 %p、没有 tid，本项目日志格式里没有任何线程字段（src/Logger.cc 可自证）。
    //   而紧随其后的 LOG_FATAL 不受级别控制，一定打印并 exit(-1)。
    LOG_DEBUG("EventLoop created %p in thread %d\n", this, threadId_);
    // 【流程】one loop per thread 的强制点（TLS 检查），机制讲解见文件顶部。
    // 【易错】注意它发生在 poller_ 已创建、wakeupFd_ 已创建之后：如果本线程真出现第二个
    //   loop，我们已经白白 epoll_create1 了一个 fd 才 exit。无害（exit 会关掉所有 fd），
    //   但说明"把校验放进构造函数"必然有这种"检查前已经花了资源"的尾巴；
    //     //   这类"检查前已经花了资源"的尾巴是所有"构造函数里做校验"写法的共性，取舍要写明。
    // 【设计】"检查 + 登记"写在一起，正是 TLS 能无锁保证唯一性的原因：
    //   读的是本线程那份副本，写的也是本线程那份，别的线程根本看不见彼此。
    //   反证：如果改用"全局 map<pid_t, EventLoop*>"，这两步就必须加锁，
    //   还要处理 map 自身的初始化与析构顺序——这就是 TLS 方案真正的价值。
    if (t_loopInThisThread)
    {
        LOG_FATAL("Another EventLoop %p exists in this thread %d\n", t_loopInThisThread, threadId_);
    }
    else
    {
        t_loopInThisThread = this;
    }
    
    // 【顺序】最后两步遵守"先配置好对象，再把它挂上任何可被观察的位置"这条纪律：
    //   1) 省掉 setReadCallback：EPOLLIN 到来时 Channel::handleEventWithGuard 里
    //      if (readCallback_) 为空 → 什么都不做 → eventfd 计数器没被读走 →
    //      电平触发下 epoll_wait 每次都立刻返回同一事件 → 主循环 100% CPU 空转。
    //   2) 省掉 enableReading：门铃压根没人监听，跨线程任务最坏压到 10 秒 poll 超时。
    //   3) 两句互换的风险是**理论性**的：它们都在构造函数里、loop() 之前完成，
    //      正常流程中这个窗口里不会有事件被派发；但只要发生"构造期把 this 交出去"
    //      （本类的 poller_ 就存了 this），这个窗口就可能被别人用到。
    //      顺序问题要按纪律判断，不要按"这次侥幸没事"判断。
    // 【语法】std::bind(&EventLoop::handleRead, this) 把成员函数指针 + this 绑成一个
    //   "无参可调用对象"，再隐式转换成 std::function<void()>（要求签名严格匹配，
    //   handleRead 正是 void()，所以能过；如果写成 bind(&EventLoop::quit, this) 也合法，
    //   但返回值 bool 会被丢掉——std::function<void()> 允许丢弃返回值）。
    // 【设计】按值捕获的是 this（裸指针）而不是 shared_from_this：EventLoop 一定比它
    //   自己的 wakeupChannel_ 活得久（两者同生共死，Channel 是 unique_ptr 成员），
    //   所以这里没有悬垂风险，也不需要 tie()。
    wakeupChannel_->setReadCallback(
        std::bind(&EventLoop::handleRead, this)); // 设置wakeupfd的事件类型以及发生事件后的回调操作
    
    // 【系统调用】完整链路：Channel::enableReading 把 events_ 置成 EPOLLIN|EPOLLPRI
    //   并 update() → EventLoop::updateChannel → EPollPoller::updateChannel（index_==kNew）
    //   → epoll_ctl(epollfd_, EPOLL_CTL_ADD, wakeupFd_, &event)。
    // 【设计】"EventLoop 从构造完成起就无条件监听自己的门铃"，这就是注释里写的
    //   每一个 EventLoop 都将监听 wakeupChannel_ 的读事件；它永远不会被 disable，
    //   因为一旦 disable，跨线程 queueInLoop 就再也叫不醒这个线程了。
    wakeupChannel_->enableReading(); // 每一个EventLoop都将监听wakeupChannel_的EPOLL读事件了
}
/* 【流程】⑫ 的最后一环：EventLoopThread::threadFunc 里的 EventLoop 是栈对象，
 *   loop.loop() 返回、离开作用域才走到这里 ⇒ 析构发生在 loop 自己的线程（必须如此）。
 *   mainLoop 那个则发生在 main 线程，进程退出前。
 * 【顺序】清理三步的顺序是有讲究的：先从 epoll 上摘除事件（disableAll → updateChannel
 *   走 EPOLL_CTL_MOD 把 events 清零）、再 EPOLL_CTL_DEL（remove）、最后才 close(fd)。
 *   反过来先 close 的话：fd 号会立刻被下一个 open 复用，而 epoll 里可能还挂着旧记录，
 *   后续任何一次误操作都可能打到别人的 fd 上。（Linux 在 close 时会清掉该 fd 在本
 *   epoll 实例里的注册，所以反序通常不炸，但"先摘后关"是工程纪律。）
 * 【⚠问题】wakeupFd_ 里可能残留没读走的计数：例如别的线程刚 wakeup() 完，
 *   本 loop 已经从 while 条件退出，那个 EPOLLIN 永远没人 handleRead。
 *   无害的原因：计数器是内核里跟 fd 走的对象，close(wakeupFd_) 把它连同 fd 一起销毁；
 *   将来新 EventLoop 拿到的是全新的 fd/全新的计数器，不会"继承"上一次的门铃声。
 * 【⚠问题】析构里没有任何自检（本教学版整体不用 assert，也没有 assertInLoopThread 一类
 *  接口），所以"loop 还在跑就销毁 EventLoop"是静默 UB：不报错、不死锁，
 *  只在压测时随机崩。便宜的改法是这里补一句 assert(!looping_)。
 * 【易错】最后一行 t_loopInThisThread = nullptr 清的是"当前执行析构的这个线程"的那份 TLS。
 *   正常情况下（谁建谁拆）正好是它自己；如果 A 线程 delete 了 B 线程的 EventLoop，
 *   会把 A 的登记表清空、B 的那份变成悬垂指针 —— 本项目靠 EventLoopThread 的
 *   quit + join 顺序保证不发生这种事。
 */
EventLoop::~EventLoop()
{
    wakeupChannel_->disableAll(); // 给Channel移除所有感兴趣的事件
    wakeupChannel_->remove();     // 把Channel从EventLoop上删除掉
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

// 开启事件循环
/* 【流程】④（mainLoop）/⑦（subLoop）的发动机；整个服务端 99.9% 的时间都在这 20 行里。
 * 【线程】只能在 loop 所属线程调用。
 * 【四步循环】背下来就是 EventLoop 的全部：
 *   (1) activeChannels_.clear()  清掉上一轮派发结果（poll 是追加语义，不清就重复处理）
 *   (2) poller_->poll(kPollTimeMs, &activeChannels_)  唯一的阻塞点 = epoll_wait
 *   (3) for (channel : activeChannels_) channel->handleEvent(pollRetureTime_)  事件派发
 *   (4) doPendingFunctors()  把别的线程（或自己）投递进来的任务整批执行掉
 *   然后回到 (1)，直到 quit_ 为真才跳出并复位 looping_。
 * 【设计】(3) 在 (4) 之前的意义：新连接的 enableReading 是在 (4) 里由
 *   connectEstablished 完成的，所以它一定晚于本轮 fd 事件派发 ——
 *   不会出现"连接还没注册进 epoll，就先被派发事件"的顺序错乱。
 * 【设计】为什么一轮只跑一批任务、而不是 while 跑空队列：见 queueInLoop 里
 *   callingPendingFunctors_ 那一节的"公平性"讨论（防止自投递回调饿死 IO）。
 * 【⚠问题】本项目 loop() 少了三道很常见的自检（上游 muduo 一类实现通常有对应断言，
 *   具体写法以原版源码为准）。下面三条都属于"错了不报错、只在压测时随机崩"的类型：
 *   a) 没有 assert(!looping_)：同一个 EventLoop 被两个线程同时 loop()，
 *      两者会一起 epoll_wait 同一个 epollfd，并对同一个 activeChannels_ 并发
 *      clear/push_back → 迭代中容器被清 → 随机崩溃或重复派发。
 *   b) 没有 assertInLoopThread()：在错误的线程 loop()，threadId_ 和真实线程不符，
 *      isInLoopThread() 恒为假，于是所有 runInLoop 都变成"往没人消费的队列里塞任务"。
 *  c) 没有"轮次计数/心跳"日志（git grep iteration_ HEAD 无结果），loop 卡死时
 *     无法从日志判断"它已经第几轮没动了"。唯一的每轮日志在 EPollPoller::poll 里，
 *     而且用的是 LOG_INFO（见对应文件的注释）。
 * 【面试】"muduo 的 EventLoop 一次循环做了哪些事？"—— 按上面四步答，
 *   再补一句"其中第 2 步是唯一可能阻塞的地方，所以所有回调都必须非阻塞"。
 */
void EventLoop::loop()
{
    looping_ = true;
    quit_ = false;

    LOG_INFO("EventLoop %p start looping\n", this);

    while (!quit_)
    {
        // 【设计】clear() 只把 size 归零、不释放 capacity ⇒ 从第 2 轮起零堆分配；
        //   这是把 activeChannels_ 做成成员变量的主要收益。
        // 【易错】EPollPoller::poll 内部只做 fillActiveChannels 的 push_back，不负责 clear。
        //   删掉这一行的现象：上一轮活跃过的 Channel 这一轮又被 handleEvent 一次，
        //   表现为"没数据也收到消息"、"同一段数据被处理两遍"，而且只在有流量的时候出现。
        activeChannels_.clear();
        // 【流程】④⑤⑦⑧⑩⑪ 全都经过这一行（它是所有事件的来源）。
        // 【系统调用】poller_->poll → epoll_wait(epollfd_, events, maxevents, timeoutMs)：
        //   返回 >0 是活跃 fd 个数；==0 是 10 秒超时；-1 且 errno==EINTR 被 EPollPoller
        //   当成"超时"吞掉（这是对的，信号打断不是错误）；其他 -1 打 LOG_ERROR。
        // 【设计】结果"填"进调用方给的容器（指针出参），而不是 return 一个 vector：
        //   省一次构造/移动，也让 EventLoop 能把容器做成有复用容量的成员。
        // 【易错】pollRetureTime_ 是"这一整批事件的共同到达时间"：一次返回 1000 个事件，
        //   最后一个被处理时拿到的时间戳可能已经比真实到达时间晚十几毫秒。
        //   需要精确时间就得自己 clock_gettime，muduo 明确接受这个误差。
        pollRetureTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        // 【流程】⑤⑧⑩⑪ 的"通知每个 Channel 处理自己的事件"都从这个 for 开始：
        //   EventLoop 不知道也不想知道每个 fd 该干什么，它只负责把发生事件的 Channel
        //   交出来（Demultiplex → Dispatch 的分界线就在这）。
        // 【语法】range-for 遍历的是 vector 的快照区间（begin/end 在循环开始时取一次）：
        //   如果回调里 emplace_back 往 activeChannels_ 塞东西，新元素不会被本轮看到；
        //   若塞到触发扩容，end 迭代器还会失效 → UB。这就是"不要在回调里改正在遍历的容器"。
        for (Channel *channel : activeChannels_)
        {
            // Poller监听哪些channel发生了事件 然后上报给EventLoop 通知channel处理相应的事件
            // 【线程】同步调用：用户回调（onConnection/onMessage/onWriteComplete…）
            //   就是在这一行、在 loop 线程里跑起来的。
            // 【易错】回调里做阻塞操作（同步查库、sleep、大文件 IO）= 这个线程上
            //   所有连接一起停摆，别的连接的数据在内核缓冲区里躺着。
            //   one loop per thread 的纪律就是："重活另开线程，做完 queueInLoop 回来"。
            // 【易错】本项目在派发期间**不留任何痕迹**：没有"当前正在处理哪个 Channel"的成员
            //   （git grep currentActiveChannel_ HEAD 与 eventHandling_ HEAD 都无结果），
            //   所以"在回调里销毁当前这个 Channel"完全没有诊断手段——下一轮 clear 之前，
            //   activeChannels_ 里一直躺着一个可能已经悬垂的裸指针。
            //   本库靠 Channel::tie()（weak_ptr 提升失败就不调回调，见 src/Channel.cc）+
            //   TcpConnection 的 shared_ptr 把这条路径挡住；这是本库最容易被说错的简化点。
            //   更严谨的实现会加派发期诊断成员与断言（写法以原版源码为准）。
            channel->handleEvent(pollRetureTime_);
        }
        /**
         * 执行当前EventLoop事件循环需要处理的回调操作 对于线程数 >=2 的情况 IO线程 mainloop(mainReactor) 主要工作：
         * accept接收连接 => 将accept返回的connfd打包为Channel => TcpServer::newConnection通过轮询将TcpConnection对象分配给subloop处理
         *
         * mainloop调用queueInLoop将回调加入subloop（该回调需要subloop执行 但subloop还在poller_->poll处阻塞） queueInLoop通过wakeup将subloop唤醒
         **/
        // 【流程】⑦ 的落地点（connectEstablished 在这里被执行）、⑥⑨⑪ 所有跨线程
        //   任务的消费点。每轮恰好一次，执行完立刻回到 poll。
        // 【设计】放在 while 的最后：保证 IO 事件优先，任务不会饿死主循环；
        //   也保证"本轮新 enableReading 的 Channel"要到下一轮 poll 才可能收到事件（正确性）。
        doPendingFunctors();
    }
    // 【流程】⑫ 的终点标志：走到这里说明 while(!quit_) 已经判假。
    // 【易错】loop() 返回不等于 EventLoop 被销毁，只是循环结束；紧接着
    //   EventLoopThread::threadFunc 会加锁把 loop_ 置回 nullptr（那一步是必须的，
    //   因为下面 loop 这个栈对象就要析构了，不能再让别的线程拿着它）。
    // 【设计】looping_ 在这里复位，和顶部的 looping_ = true 严格配对；
    //   中途 throw 异常会漏掉复位（本库假定回调不抛异常）。
    LOG_INFO("EventLoop %p stop looping.\n", this);
    looping_ = false;
}

/**
 * 退出事件循环
 * 1. 如果loop在自己的线程中调用quit成功了 说明当前线程已经执行完毕了loop()函数的poller_->poll并退出
 * 2. 如果不是当前EventLoop所属线程中调用quit退出EventLoop 需要唤醒EventLoop所属线程的epoll_wait
 *
 * 比如在一个subloop(worker)中调用mainloop(IO)的quit时 需要唤醒mainloop(IO)的poller_->poll 让其执行完loop()函数
 *
 * ！！！ 注意： 正常情况下 mainloop负责请求连接 将回调写入subloop中 通过生产者消费者模型即可实现线程安全的队列
 * ！！！       但是muduo通过wakeup()机制 使用eventfd创建的wakeupFd_ notify 使得mainloop和subloop之间能够进行通信
 **/
/* 【线程】任意线程可调 ⇒ quit_ 必须是 atomic（别的线程写的值要对本线程可见）。
 * 【流程】⑫ 起点：常见触发方式有两个 —— ① 信号处理/主逻辑里对 baseLoop 调 quit；
 *   ② ~EventLoopThread 里对 subLoop 调 quit（这是本仓库里唯一实际的调用点）。
 * 【设计】为什么本线程调用时不 wakeup：自己就站在 while 条件上，写标志位就够；
 *   给自己写 eventfd 反而会让"下一轮 epoll_wait 空醒一次"（虽然此时循环已经要结束了）。
 * 【⚠问题】和 loop() 顶部的 quit_ = false 打架：如果在 loop() 开始跑之前
 *   （例如 main 里 TcpServer::start() 之后立刻）从别的线程调 quit()，标志位先被置真，
 *   然后 loop() 第一件事把它清回 false ⇒ 退出请求被吞，程序照旧进 poll。
 *  这是"标志位 + 无锁握手"这类设计的固有缝隙：loop() 开头那句 quit_ = false 的本意是
 *  "允许一个已经退出过的 loop 再被 loop 起来"，代价就是吞掉启动之前的 quit 请求。
 *  规避办法很简单：先让目标线程进 loop()，再允许别人对它调 quit()。
 *  本仓库实际的 quit 调用点是 ~EventLoopThread，那时子线程早就在 loop 里了，不触发这条。
 * 【⚠问题】另一个副作用：本线程调用 quit() 时如果 loop 正阻塞在 poll（不可能——
 *   本线程就在 poll 里，跑不到 quit()），所以只有一种例外：
 *   信号处理函数里调 quit()。信号打断 epoll_wait 返回 EINTR，EPollPoller 当超时处理，
 *   回到 while 判定才看到 quit_ —— 这条路径能走通，但依赖"EINTR 被正确忽略"。
 */
void EventLoop::quit()
{
    quit_ = true;

    // 【易错】判断用的是 isInLoopThread()（比较 tid），不是"是否在 loop 中"。
    //   所以在 loop 线程里、但 loop() 还没跑起来时调 quit()，同样不会 wakeup ——
    //   这就是上面那条 quit_ 被吞的问题的成因。
    // 【设计】这里"不持锁、不用 cond.notify"，只是往 eventfd 写 8 字节：
    //   写方永远不阻塞，读方（poll）一定被打断，是最便宜的跨线程打断手段。
    if (!isInLoopThread())
    {
        wakeup();
    }
}

// 在当前loop中执行cb
/* 【线程】两种完全不同的语义，务必分清：
 *   - 在 loop 线程调用：cb() 当场同步执行完才返回（此时你正夹在 handleEvent 或
 *     doPendingFunctors 中间，cb 会"插队"在后续事件之前）。
 *   - 在别的线程调用：等价于 queueInLoop ⇒ 只是投递，你不知道 cb 何时跑、跑没跑完，
 *     更不能等它的结果（fire-and-forget）。
 * 【流程】③ loop_->runInLoop(Acceptor::listen)：如果调用方就是主线程，走 if 分支，
 *   listen + enableReading 立刻完成（这也是为什么 start() 返回后端口就已可用）。
 *   ⑥⑦ ioLoop->runInLoop(connectEstablished)：一定走 else 分支（ioLoop 是 subLoop）。
 * 【设计】muduo 到处包一层 runInLoop 而不是直接调目标函数，是为了维持
 *   "单线程所有权"：TcpConnection/Acceptor 的成员只允许它所属 loop 线程碰。
 *   所有入口都收口到 runInLoop，业务类内部就一行锁都不需要。
 * 【易错】形参 Functor cb 是**按值**传的：std::bind 出来的对象（含 shared_ptr 副本）
 *   被拷进 cb，再由 else 分支拷进队列 ⇒ 任务会替对象"续命"到执行完。
 *   反例：捕获裸指针 + 对象先死 = 悬垂，编译期不会提醒你。
 * 【语法】形参是**按值** Functor：调用方传临时 bind 结果时可以移动进来，传左值则多一次
 *  std::function 拷贝（其拷贝可能带一次堆分配）。这里值传参说得通：else 分支还要把
 *  cb 再拷一份投进队列，手里先有自己的副本，语义比"const 引用 + 内部再拷"更直白。
 */
void EventLoop::runInLoop(Functor cb)
{
    if (isInLoopThread()) // 当前EventLoop中执行回调
    {
        cb();
    }
    else // 在非当前EventLoop线程中执行cb，就需要唤醒EventLoop所在线程执行cb
    {
        queueInLoop(cb);
    }
}

// 把cb放入队列中 唤醒loop所在的线程执行cb
/* 【线程】任意线程 —— 本函数与 wakeup 是全库唯一真正的多线程接触点。
 * 【设计】两个动作严格分开：先加锁入队，**出锁之后**再 wakeup。
 *   反过来（持锁 write）有两重坏处：把一次 syscall 塞进临界区；
 *   被叫醒的 loop 线程可能立刻回头抢同一把锁，白白排一次队。
 * 【流程】⑥【线程跳转 1】mainLoop→subLoop、⑪【线程跳转 2/3】的连接建立/销毁
 *   全部经这里；每次都是"入队 + 敲门"。
 * 【面试】"muduo 怎么做到 TcpConnection 不加锁的？"——
 *   答案就是这一层：所有对连接的读写请求都被 queueInLoop 串行化到它所属的
 *   loop 线程执行，锁只存在于任务队列这一处，且临界区只有一句 emplace_back。
 */
void EventLoop::queueInLoop(Functor cb)
{
    {
        // 【语法】手动加一对花括号来缩小 unique_lock 的作用域，是 RAII 锁的常用手法
        //   （等价于常见的"局部对象构造时加锁、析构时解锁"写法，语义完全一样）。
        // 【设计】用 emplace_back(cb) 而不是 push_back：直接在容器里就地构造 std::function，
        //   比 push_back 少一层移动。注意 cb 本身已经是形参副本，这里只是把它搬进容器。
        // 【易错】锁里绝对不能有 wakeup()/syscall/用户回调：临界区越长，
        //   所有其他线程的 queueInLoop 排队越久，高并发下这一步就是性能瓶颈。
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(cb);
    }

    /**
     * || callingPendingFunctors的意思是 当前loop正在执行回调中 但是loop的pendingFunctors_中又加入了新的回调 需要通过wakeup写事件
     * 唤醒相应的需要执行上面回调操作的loop的线程 让loop()下一次poller_->poll()不再阻塞（阻塞的话会延迟前一次新加入的回调的执行），然后
     * 继续执行pendingFunctors_中的回调函数
     **/
/* 【必考】为什么第二个条件 callingPendingFunctors_ 不能省？完整推演一遍：
 * 场景：loop 线程正在 doPendingFunctors() 里执行回调 f1，f1 又调 queueInLoop(f2)（同一个 loop）。
 *   - isInLoopThread() 为真 ⇒ 第一个条件为假；
 *   - 若没有第二个条件，就不会 wakeup。
 *   而 f2 进不了本轮的 functors 快照（swap 已经把旧内容整批搬走了），
 *   doPendingFunctors 返回后 loop 回到 while 顶部，一头扎进 epoll_wait(10000ms)。
 *   结果：f2 最坏要等 10 秒才被执行。表现就是"数据偶发卡 10 秒才发出去"
 *   ——TcpConnection::send 的跨线程分支、connectDestroyed 的二次投递都走这条路。
 * 有了这一位：loop 线程处于"正在执行回调"这个窗口时，任何自己投给自己的任务
 *   都会往自己的 eventfd 写 8 字节 ⇒ 下一轮 epoll_wait 立刻带着 wakeupFd_ 的 EPOLLIN
 *   返回（自唤醒，不产生跨线程上下文切换，代价只是一次 write + 一次 read），
 *   随后本轮 doPendingFunctors 就把 f2 跑掉。
 * 【为什么不写"while 队列非空就一直执行"】
 *   那样一个"自己给自己 queueInLoop"的回调（本仓库 TcpConnection::sendFileInLoop
 *   在 EWOULDBLOCK 时正是这么干的）会把 loop 线程永久锁在任务队列上，
 *   新到的 IO 事件没人处理 —— 所以 muduo 选择"每轮只执行一批"，
 *   用 callingPendingFunctors_ 这个自唤醒标志来弥补延迟，兼顾公平与及时。
 * 【设计】callingPendingFunctors_ 是本线程读 + 别的线程读的标志（写只在本线程），
 *   所以 atomic_bool 足够，不需要放进 mutex_ 的保护范围。
 * 【设计】两个条件写成"或"而不是"且"，因为它们覆盖的对象完全不同：
 *  异线程 ⇒ 对方可能正睡在 epoll_wait 里（必敲）；同线程 ⇒ 只有"正在执行回调"这个
 *  窗口里投给自己的任务会被本轮快照漏掉（自敲），除此之外都不必敲。
 *  面试能讲出"自唤醒（wake up self）"这个概念，这题就赢了。
 */
    if (!isInLoopThread() || callingPendingFunctors_)
    {
        wakeup(); // 唤醒loop所在线程
    }
}

/* 【线程】只在 loop 自己的线程执行：它由本轮 epoll 派发 wakeupChannel_ 而来。
 * 【系统调用】read(wakeupFd_, &one, 8)：读出内核计数器并**清零**。
 *   因为建 fd 时带了 EFD_NONBLOCK，万一此刻计数器已经是 0（重复进入/虚惊一场），
 *   read 返回 -1/EAGAIN 而不会阻塞 loop 线程 —— 这也解释了为什么这里判
 *   n != sizeof(one) 而不是判 n < 0：一次判断同时覆盖了"读失败"和"读到的字节数不对"。
 * 【设计】读出来的值被直接丢弃：唤醒是"布尔语义"，只关心"有人按过门铃"，
 *   不关心按了几次（多次 wakeup 已被内核合并成一个计数）。
 * 【易错】必须读走！不读的话计数器保持非 0，LT 模式下 epoll_wait 每次都立刻返回，
 *   主循环退化成 100% CPU 的死循环。用 pipe 的思维写 eventfd（"留着下次读"）是最常见的错。
 * 【⚠问题】出错只 LOG_ERROR、不区分 errno，也不重试。如果真发生（例如 fd 被误关），
 *   这个 loop 的唤醒机制从此形同虚设，日志里只有一条不起眼的 ERROR 可查。
 * 【设计】这里直接用 ::read 而不是本仓库的 Socket 封装：门铃是 EventLoop 的私有实现，
 *  不进 Socket 的 RAII 体系（fd 的关闭就在 ~EventLoop 里手动做，两者配套出现）。
 */
void EventLoop::handleRead()
{
    uint64_t one = 1;
    ssize_t n = read(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one))
    {
        LOG_ERROR("EventLoop::handleRead() reads %lu bytes instead of 8\n", n);
    }
}

// 用来唤醒loop所在线程 向wakeupFd_写一个数据 wakeupChannel就发生读事件 当前loop线程就会被唤醒
/* 【线程】任意线程，**包括 loop 线程自己**（就是上面 callingPendingFunctors_ 的场景）。
 * 【系统调用】write(wakeupFd_, &one, 8)：内核把计数器 +1 并把该 fd 标为可读，
 *   于是阻塞在 epoll_wait 上的目标线程带着 EPOLLIN 返回。
 *   往 eventfd 写不会阻塞（除非计数器溢出，实际不可能；即便发生也因为有
 *   EFD_NONBLOCK 而返回 EAGAIN），所以调用方永远不会因为"叫别人起床"而卡住。
 * 【设计】"写"为什么能唤醒"读"事件？因为 epoll 关心的是 fd 的可读性，
 *   而 eventfd 的可读条件就是"计数器 != 0"。没有队列、没有锁、没有 CAS，
 *   这就是它能替代 condition_variable 接进 epoll 的全部原理。
 * 【易错】wakeup 与"任务真正被执行"之间没有任何同步：fire-and-forget。
 *   所以调完 queueInLoop 不能假定对端已经收到任务，更不能拿它做远程过程调用；
 *   要结果就在回调里再投回去（muduo 的 send 回执、onWriteComplete 都是这个套路）。
 * 【⚠问题】写失败只打 LOG_ERROR，既不重试也不区分 errno：如果真是 EBADF（fd 被误关）
 *  或 EAGAIN（除非门铃堆到计数器溢出，实际不可能），这次唤醒就永久丢了——
 *  对端 loop 会一路睡到 10 秒 poll 超时，现场只留一行 ERROR 日志。
 */
void EventLoop::wakeup()
{
    uint64_t one = 1;
    ssize_t n = write(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one))
    {
        LOG_ERROR("EventLoop::wakeup() writes %lu bytes instead of 8\n", n);
    }
}

// EventLoop的方法 => Poller的方法
// 【设计】外观模式（Facade）的落地点，三条收益见 EventLoop.h 里 updateChannel 的注释。
//   一句话：Channel 只需要知道"我的 loop"，不需要知道"哪个 epoll、哪个 fd 表"。
// 【线程】只允许 loop 线程调用（EPollPoller 的 channels_ 是无锁 unordered map）。
// //   本仓库整体不用 assert（git grep assertInLoopThread HEAD 无结果），这条契约纯靠约定与审查。
// 【流程】② Acceptor 的 acceptChannel_ 注册、⑦ 新连接 enableReading、
//   ⑨/⑩ enableWriting/disableWriting、⑪ remove，最终都落到这里 → epoll_ctl。
void EventLoop::updateChannel(Channel *channel)
{
    poller_->updateChannel(channel);
}

// 【流程】⑪ connectDestroyed 里 channel_->remove() → 这里 → EPollPoller::removeChannel
//   → epoll_ctl(EPOLL_CTL_DEL)。
// 【设计】DEL 之后内核忘掉这个 fd，但 fd 本身还开着 —— 它的生命周期归 Socket（RAII），
//   真正的 close 发生在 Socket 析构（A2/A5 会讲 TcpConnection 引用计数归零那一刻）。
// 【易错】不要在这里顺手 close(fd)：fd 号会被下一个 open/socket 立刻复用，
//   而 Channel 只是"事件的视图"，不拥有 fd。谁拥有谁关闭。
void EventLoop::removeChannel(Channel *channel)
{
    poller_->removeChannel(channel);
}

// 【⚠问题】定义被整段注释掉了，但 include/EventLoop.h 里的声明还在：
//   现在全库无人调用所以链接得过，谁写一句 loop->hasChannel(ch) 就会在**链接期**
//   报 undefined reference（编译期完全看不出问题）。
//   include/Poller.h 与 src/Poller.cc 里有一份一模一样的情况（见对应文件的注释）。
//   这段被注释的代码请保持原样，它是"接口与实现不对称"最好的活教材。
// bool EventLoop::hasChannel(Channel *channel)
// {
//     return poller_->hasChannel(channel);
// }

/* 【线程】只在 loop 自己的线程里被 loop() 调用（private，且全库唯一调用点就在 loop 里）。
 * 【流程】⑥⑦⑨⑪ 所有跨线程任务的出口；⑦ 里 TcpConnection::connectEstablished
 *   就是在这里被第一次执行的。
 * 【四步 + 每一步的理由】
 *   1) std::vector<Functor> functors;               局部快照（关键中的关键）
 *   2) callingPendingFunctors_ = true;              先置位，后取快照（顺序有讲究）
 *   3) { lock; functors.swap(pendingFunctors_); }   锁内只做一次 O(1) 指针交换
 *   4) for (...) functor();                         在锁外执行；最后复位标志
 * 【为什么 swap 到局部而不是持锁遍历成员容器】两个理由，缺一不可：
 *   (a) 缩短临界区：vector::swap 只换三个指针（begin/end/capacity），是 O(1)，
 *       锁的持有时间与任务个数、任务耗时彻底无关。否则一个耗时 1 秒的用户回调
 *       会让所有其他线程的 queueInLoop 全卡在这把锁上（想象 8 个 IO 线程互相等）。
 *   (b) 避免自我死锁：std::mutex 不可重入。functor() 里如果直接或间接调 queueInLoop
 *       （本库里到处都是：send、connectEstablished、removeConnection…），
 *       同一线程会第二次 lock(mutex_) ⇒ glibc 上表现为永久阻塞（标准上是 UB）。
 *       swap 出锁之后再执行回调，回调里的 emplace_back 拿到的是一把空闲的锁。
 * 【为什么 callingPendingFunctors_ 要在 swap 之前置 true】
 *   queueInLoop 的判断读的是这个 flag。若先 swap 再置位，就留下一个窗口：
 *   快照已经取走、flag 还是 false —— 此时同线程路径投进来的任务看到 false 就不敲门，
 *   于是被压到下一次 poll（正是上面讲的 10 秒）。"先置位、后取快照"保证：
 *   任何在快照之后进入队列的任务，一定看得见 true，宁多敲一次门，绝不漏敲。
 * 【为什么回调里"把对象删掉"也安全】
 *   真实链路（⑪）：closeCallback_ → TcpServer::removeConnectionInLoop → connections_.erase(...)，
 *   如果这是最后一个强引用，TcpConnection 当场就该析构。为什么不会炸？
 *   - 执行中的 Functor 存在**局部 vector functors** 里，它的所有者是当前栈帧，
 *     只有 doPendingFunctors 返回时才析构，绝不可能在 functor() 执行过程中被谁抽走；
 *   - 而 std::bind 按值捕获的 shared_ptr 副本本身就持有引用计数，
 *     map 里那份 erase 掉之后，对象被这个副本续命到回调结束。
 *   反过来说，若遍历的是成员 pendingFunctors_：容器可能在遍历中被别的线程
 *   emplace_back 触发扩容搬走元素（引用失效），也可能被回调链间接销毁，
 *   两种都是"脚下的地板被抽走"。
 * 【易错】functors 里的元素是 Functor 的**副本**（入队时就拷了），swap 只是把整块缓冲区
 *   连副本一起搬进局部变量，所以没有额外拷贝开销；这也意味着"入队后你再改本地变量"没用。
 * 【⚠问题】没有任何任务积压的可观测手段（git grep queueSize HEAD 无结果，也没有
 *  "本轮执行了几个 functor"的日志）：一旦某个回调疯狂自投递，现象只有内存缓慢上涨
 *  和 loop 变慢，很难定位到"是谁在投"。
 * 【⚠问题】没有异常安全：functor() 抛异常会跳过 callingPendingFunctors_ = false，
 *   标志位永远停在 true ⇒ 之后每次同线程 queueInLoop 都白白 wakeup 一次；
 *   同时局部 functors 里剩下的任务被栈展开销毁（静默丢失）。
 *   muduo 的整体假定就是"回调不许抛异常"（它连 RAII 守卫都不写），这点要知道。
 */
void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors;
    // 【设计】置位必须在取快照之前，理由见上面的"先置位后取快照"。
    //   这个标志的唯一读者是 queueInLoop 里那第二个条件。
    callingPendingFunctors_ = true;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 【语法】swap 而不是 move 赋值：两者都是 O(1) 换缓冲区，swap 的写法让
        //   pendingFunctors_ 留在"空但已分配"的状态，下次 emplace_back 可能省一次扩容。
        // 【⚠问题】原注释里"执行 functor() 在临界区内会产生死锁"这句要精确化：
        //   死锁只发生在**同一线程重入**（回调里 queueInLoop → 再 lock 同一把非递归锁）；
        //   若是"回调里调了别的线程的 loop"则不会死锁，只是排队。原注释把两者混在一起说了。
        functors.swap(pendingFunctors_); // 交换的方式减少了锁的临界区范围 提升效率 同时避免了死锁 如果执行functor()在临界区内 且functor()中调用queueInLoop()就会产生死锁
    }

    // 【易错】注意这里出锁了（上面那个花括号的作用域只包到 swap）。
    //   遍历期间 pendingFunctors_ 可能正被别的线程写 —— 不冲突，因为那是另一个容器，
    //   我们读的是 functors。这正是 swap 换来的自由。
    for (const Functor &functor : functors)
    {
        functor(); // 执行当前loop需要执行的回调操作
    }

    // 【流程】一轮 loop 到此结束，回到 while 顶部继续 poll。
    //   如果本轮里有回调调用了 quit()，这里复位标志后 while(!quit_) 判假 → ⑫。
    callingPendingFunctors_ = false;
}
