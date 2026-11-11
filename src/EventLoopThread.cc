/* ============================================================================
 * 【文件】src/EventLoopThread.cc
 * 【一句话】让一条子线程在自己的栈上造一个 EventLoop 并永久跑 loop()，
 *          主线程则通过 mutex + condition_variable 与它握手拿到那个 loop 指针。
 * 【Reactor 角色】线程壳 + Loop 宿主（③ 起 subLoop、⑫ 收 subLoop 的地方）。
 * 【依赖】"EventLoopThread.h" 自己的声明；"EventLoop.h" —— 这里要真正构造它，
 *        所以必须拿到完整类型（头文件里只前向声明过一次）。
 * 【谁用它】EventLoopThreadPool::start 逐个调 startLoop()；
 *          EventLoopThreadPool 析构 → ~EventLoopThread → quit + join。
 * 【先修知识】条件变量的 wait(lock, pred) 到底循环了什么；
 *          虚假唤醒(spurious wakeup)与丢通知(lost notify)的区别；
 *          "对象生命周期 = 表达式所在栈帧"的 RAII 规则；析构发生在哪个线程为什么重要。
 * 【难度】★★★★☆（两个线程交错，务必先看懂握手时序图再往下看）
 *          【建议耗时】40 分钟
 * 【自检】a) startLoop 为什么能在返回时保证 loop 已经构造完？谁在等谁？
 *        b) threadFunc 里 EventLoop 是栈对象，为什么这样反而是对的？
 *        c) loop.loop() 之后那两行加锁清 loop_ 少了会怎样？
 * ============================================================================ */

#include "EventLoopThread.h"
#include "EventLoop.h"

// 【流程】TcpServer 构造 EventLoopThreadPool 时（②/③ 之间）不会建这个对象；
//   是 EventLoopThreadPool::start 里循环 new 时才走到这里。
// 【设计】初始化列表里就把线程体绑好：thread_(std::bind(&EventLoopThread::threadFunc, this), name)。
// 【易错】在构造函数里使用 this 做 std::bind 是安全的 —— 因为 bind **只记录**指针，
//   并不调用；真正调用它的是 startLoop() → Thread::start → 子线程，
//   那时对象早已构造完。但如果写成 std::thread(&EventLoopThread::threadFunc, this)
//   放在初始化列表里（也就是"构造即起线程"），子线程就可能读到还没构造的成员 ⇒ 竞态。
//   对比一下就能理解 muduo 为什么坚持"两阶段：构造登记 / start 起线程"。
// 【语法】初始化列表顺序 loop_ → exiting_ → thread_ → mutex_ → cond_ → callback_
//   与头文件声明顺序一致（否则 -Wreorder；顺序写反且成员间有依赖时会出隐蔽 bug）。
EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
                                 const std::string &name)
    : loop_(nullptr)
    , exiting_(false)
    , thread_(std::bind(&EventLoopThread::threadFunc, this), name)
    , mutex_()
    , cond_()
    , callback_(cb)
{
}

/* 【流程】⑫：subLoop 线程的死亡流程从这里开始 ——
 *   exiting_ = true（本项目里没人读，见头文件【⚠问题】）
 *   → loop_->quit()：跨线程写 EventLoop::quit_ 并 wakeup()（写它自己的 eventfd）
 *   → thread_.join()：阻塞等 threadFunc 返回
 *   → 成员析构（callback_/cond_/mutex_/thread_…）。
 * 【设计】"先 quit 再 join"这个顺序是生死攸关的：
 *   直接 join 会死锁 —— 子线程正阻塞在 epoll_wait(10000ms)，没人给它事件，
 *   主线程永远等在 join 上（最坏 10 秒后 poll 超时、看到 quit_ 才能退出，
 *   而 quit_ 也是这次 quit() 设的，所以"先 join 后 quit"更是不行的写法）。
 * 【线程】运行在主线程（调用 ~EventLoopThread 的那个线程，通常是 TcpServer/Pool
 *   的析构路径，即 mainLoop 线程）。
 * 【⚠问题】这里读 loop_ 没有加锁：按代码约定"startLoop 返回后到析构前，
 *   子线程只会写 loop_ 两次（赋值与清 nullptr）"，所以实际风险是
 *   "读到 nullptr 而对象其实还活着"或"读到非空而对象刚被销毁"的窗口。
 *   规范做法是 lock_guard 后再取一份本地副本再用。
 * 【对比原版】本项目这段的形状（置标志 + 判空 + quit + join）与上游一致，
 *  上游在同一条判空旁边还留着"承认这里不是百分之百无竞态"的注释
 *  （以原版源码为准，本环境无法联网核对）。也就是说本条【⚠问题】是继承来的已知取舍，
 *  不是教学版新添的坑：它实际不出事，靠的是"quit 之后立刻 join、join 一定等到
 *  threadFunc 返回、而 threadFunc 返回前已经把 loop_ 清空"这条顺序链。
 */
EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    // 【易错】判空的意义：EventLoopThread 允许"构造了但从没 startLoop()"
    //   （例如 setThreadNum(0) 之外的路径里 Pool 建好却没 start，或者 start 抛异常）。
    //   这种情况 thread_ 从未 start ⇒ started_==false ⇒ ~Thread 既不 join 也不 detach ⇒ 安全。
    //   少了这个判空，就会对空指针调 quit()（段错误），或对没起的线程调 join()（抛异常）。
    if (loop_ != nullptr)
    {
        // 【线程】跨线程调用：执行在 mainLoop 线程，影响的是 subLoop 线程。
        // 【流程】⑫：quit 里发现 !isInLoopThread() ⇒ wakeup() ⇒
        //   子线程 epoll_wait 立刻带着 wakeupFd_ 的 EPOLLIN 返回 → handleRead 清计数
        //   → 回到 while 判定 quit_ 为真 → loop() 返回。
        // 【设计】这是"跨线程打断 epoll_wait"的标准一次往返，全库靠它收尾。
        loop_->quit();
        // 【线程】阻塞主线程，直到子线程的 threadFunc 真正 return（含 EventLoop 析构）。
        // 【易错】join 之后 loop_ 指向的那个栈对象**已经不存在了**（threadFunc 最后
        //   一个作用域退出时析构的）—— 所以主线程这里绝不能再拿 loop_ 去调任何方法。
        //   这也是 threadFunc 里"loop.loop() 返回后必须加锁清 loop_"的原因（见下面）。
        thread_.join();
    }
}

/* 【握手时序图】startLoop() 与 threadFunc() —— 两个线程如何交错（背下来）
 *
 *   主线程：startLoop()                        子线程：threadFunc()
 *   -------------------------                  -------------------------
 *   thread_.start()   ── Thread::start 里的 sem 保证返回时 tid_ 就绪
 *   EventLoop *loop = nullptr;
 *   lock(mutex_)
 *   cond_.wait(lock, [&]{return loop_ != nullptr;})
 *        谓词为假 → 原子地"解锁 + 挂起"
 *                                            EventLoop loop;   ← 栈对象，构造在子线程
 *                                            if (callback_) callback_(&loop);
 *                                            lock(mutex_)
 *                                            loop_ = &loop;    ← 谓词变量必须在锁内改
 *                                            cond_.notify_one()
 *                                            unlock（出花括号）
 *        被唤醒 → 重新抢锁 → 再判一次谓词 → 真 ┘
 *   loop = loop_;   （仍在锁内，读到的是最新值）
 *   } unlock
 *   return loop;                              loop.loop()  ← 子线程从此常驻 epoll_wait
 *   （主线程继续：loops_.push_back(loop)）        ... 处理 IO / 执行 doPendingFunctors ...
 *                                             (别处 quit 之后) loop.loop() 返回
 *                                             lock(mutex_); loop_ = nullptr; unlock
 *                                             ~EventLoop（在子线程！）→ threadFunc 返回
 *
 * 【设计】startLoop() 是一个真正的同步点：它返回时子线程的 EventLoop 必定已构造完，
 *   所以 EventLoopThreadPool::start 里可以立刻把返回值塞进 loops_，
 *   随后 mainLoop 里 getNextLoop() 拿到的永远是有效对象（⑥ 分配新连接的前提）。
 * 【易错】返回的裸指针不拥有对象；调用方（Pool）只是"通讯录"，
 *   对象的生死由子线程的栈帧决定。
 */
EventLoop *EventLoopThread::startLoop()
{
    // 【流程】③ 的第一步。注意它**先于**任何等待：Thread::start 内部还会用
    //   sem 等子线程 tid_ 写好才返回（见 src/Thread.cc 的时序图），
    //   所以这里返回时"线程活着 + tid 已知"两件事都成立。
    // 【易错】如果 start() 抛异常（线程创建失败），下面 cond_.wait 会永久阻塞 ⇒
    //     //   更稳妥的写法是这里先挡住"已经启动过"（本项目完全不设防，后果见 threadFunc 那段）。
    thread_.start(); // 启用底层线程Thread类对象thread_中通过start()创建的线程

    // 【设计】局部副本 loop：出锁之后还要把值带出去，但**不能**在锁外读 loop_
    //   （那是别的线程会写的共享变量），所以趁持锁时拷一份指针到栈上。
    EventLoop *loop = nullptr;
    {
        // 【语法】unique_lock 而不是 lock_guard：condition_variable::wait 需要
        //   能在等待期间**解锁**、醒来时**重新加锁**的锁，只有 unique_lock 提供
        //   lock/unlock 接口（lock_guard 不可复制、不能提前解锁，配合不了 cond_）。
        std::unique_lock<std::mutex> lock(mutex_);
        // 【必考】wait(lock, pred) 展开就是 while (!pred()) wait(lock);
        //   为什么必须带谓词（两个独立理由，面试要分开说）：
        //   (1) 虚假唤醒（spurious wakeup）：pthread_cond_wait 允许在没人 notify 时返回
        //       （Linux 早期实现、信号干扰、多核竞态都可能）。裸 wait 会误以为"有人通知了"，
        //       带着还没准备好的 loop_ 往下走 ⇒ 返回一个空指针给整个线程池。
        //   (2) 丢通知 / 条件竞争：notify 可能发生在**你还没进入 wait 之前**。
        //       子线程极快：它可能在主线程 cond_.wait 之前就跑完 loop_=&loop; notify_one()，
        //       条件变量不会"记住"这次通知 ⇒ 没有谓词的话主线程会睡到天荒地老（死锁）。
        //       有了谓词，先判 loop_ != nullptr 直接为真 ⇒ 一次都不睡，正常返回。
        //   规则一句话：**条件变量传递的是"事件"，谓词检查的是"状态"；状态不会丢，事件会丢。**
        // 【语法】lambda 里写 [this] 才能访问成员 loop_/mutex_；缺捕获就是编译错误。
        // 【易错】wait 期间锁是被释放的 —— 所以子线程那句加锁赋值才能抢到锁；
        //   如果在 wait 之前就 unlock（手工）或者用 try_lock 玩花活，就是竞态。
        // 【设计】"手写 while + wait"和"wait(lock, pred)"是同一件事的两种写法
        //   （谓词重载相当于编译器替你写那个循环），本项目的 lambda 谓词版更不容易写漏。
        //   上游用的是哪种、有没有"重复 startLoop"的断言防呆，以原版源码为准。
        //   可以核实的是：本项目这里毫无防呆——对同一个 EventLoopThread 调两次 startLoop，
        //   第二次会走到 Thread::start 里的 shared_ptr 赋值，撞上"joinable 的 std::thread
        //   被销毁"⇒ 直接 std::terminate。
        cond_.wait(lock, [this](){return loop_ != nullptr;});
        loop = loop_;
    }
    return loop;
}

// 下面这个方法 是在单独的新线程里运行的
/* 【线程】本函数在**子线程**执行（由 Thread::start 的 lambda 调 func_() 而来）。
 * 【一句话】本函数的栈帧就是 subLoop 的一生：造 loop → 跑 loop → 清指针 → 析构。
 * 【设计】它必须"永不返回直到 quit"：一旦 return，线程就结束，
 *   这个 subLoop 上所有连接立刻变成孤儿（fd 还开着，但没人 epoll_wait 了）。
 *   所以 loop.loop() 这一行是子线程的主体，不是"调用一下试试"。
 */
void EventLoopThread::threadFunc()
{
    // 【必考】EventLoop 是**栈对象**，为什么这样才正确：
    //   (1) 构造发生在子线程 ⇒ EventLoop::threadId_ 记的是子线程 tid，
    //       __thread t_loopInThisThread 也登记在子线程 ⇒ one loop per thread 天然成立。
    //       （若改成主线程 new 好再传指针进来，threadId_ 记成主线程，
    //        子线程里 isInLoopThread() 恒假，runInLoop 全变成"投进没人消费的队列"。）
    //   (2) 生命周期由栈帧决定 ⇒ 不需要任何引用计数，也不会被主线程误 delete。
    //       这就是 EventLoopThreadPool 析构里那句 "Don't delete loop, it's stack variable" 的由来。
    //   (3) 析构也发生在子线程：loop.loop() 返回 → 清 loop_ → 函数 return 时 ~EventLoop，
    //       而 ~EventLoop 里要 disableAll/remove/close(wakeupFd_) 并把 TLS 置空 ——
    //       这些都必须在本线程做（TLS 置错线程就是给别的线程解登记）。
    // 【易错】栈对象 ≠ 不能长期存在：它的栈帧就是 threadFunc，会一直活着直到线程结束。
    // 【⚠问题】没有异常保护：threadFunc 里任何一处抛异常 ⇒ 栈展开时 loop 析构，
    //   但 loop_ 不会被清成 nullptr ⇒ 主线程若还拿着这个指针用就是悬垂。
    //   本库整体的假定是"回调不许抛异常"（连 RAII 守卫都不写，doPendingFunctors 有同款问题），
    //   所以这里也不该指望 catch；真要保证安全，就把 threadFunc 的尾巴包进 try/catch 里清 loop_。
    EventLoop loop; // 创建一个独立的EventLoop对象 和上面的线程是一一对应的 级one loop per thread

    // 【设计】判空是必须的：std::function 空对象被调用会抛 std::bad_function_call。
    // 【流程】③ 的线程初始化回调（TcpServer::setThreadInitCallback 传进来的那条链）。
    // 【易错】它在 loop.loop() **之前**执行、在子线程执行，所以此时这个 loop 还不能
    //   runInLoop（没人跑它的队列）；回调里应当只做"注册 Channel/enableReading"这类
    //   同步动作，不要指望"投个任务等它执行"。
    if (callback_)
    {
        // 【线程】子线程执行，参数是**本线程栈上那个 loop 的地址**。
        //   【设计】这条规则记成一句话更好用："线程初始化回调的执行次数 = loop 的个数"，
        //   与线程数是 0 还是 8 无关。另一半在 src/EventLoopThreadPool.cc 的 start 里。
        callback_(&loop);
    }

    // 【设计】持锁改 loop_ 是"必须"而不是"保险"：
    //   (1) 与主线程 cond_.wait 的谓词读同一个变量 ⇒ 无锁就是数据竞争（UB）；
    //   (2) 必须在锁内 notify：如果先 unlock 再 notify，主线程可能恰好夹在
    //       "判完谓词为假"和"真正挂起"之间 ⇒ 通知丢失、永久阻塞。
    //       持锁 notify 保证"改状态 + 发通知"对等待者是一个不可分割的窗口。
    //   一对花括号把锁限定在这几行（RAII 锁的常见写法），绝不带着锁进 loop.loop()。
    // 【易错】带着锁去 loop 会把主线程的 startLoop 卡死（它在等这把锁）。
    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }
    // 【流程】④/⑦：子线程从这里开始常驻在 poller_->poll（epoll_wait）。
    //   之后这个线程所有的 IO 事件、所有跨线程任务（doPendingFunctors）都在这一行内部发生。
    // 【易错】它会"返回"只有两种情况：别的线程（~EventLoopThread）调了 quit；
    //   或者 loop 里某个回调调了 quit。前者是正常退出路径，后者意味着
    //   "subLoop 自己结束自己"，此时主线程还阻塞在 join 上，同样能收尸。
    loop.loop();    // 执行EventLoop的loop() 开启了底层的Poller的poll()
    // 【必考】loop.loop() 返回后为什么必须加锁把 loop_ 置空：
    //   下一句（本函数右花括号）就是 ~EventLoop —— 对象马上就不存在了。
    //   如果 loop_ 还留着那个地址：
    //     - 主线程 ~EventLoopThread 里 if (loop_ != nullptr) loop_->quit() 会对
    //       已析构对象调成员函数（读 atomic、往已 close 的 wakeupFd_ 写）⇒ UB/段错误；
    //     - 任何还在用 loops_ 里那份裸指针的代码同理，全是悬垂。
    //   置空是"发布对象即将死亡"的信号，配合上面的 join，让主线程能看到
    //   "要么 loop_ 有效、要么已经没人能再用它"。
    // 【易错】这里必须重新加锁：loop_ 是跨线程共享变量，无锁写就是数据竞争；
    //   而且不加锁的话主线程可能读到撕裂/过期的指针值。
    // 【⚠问题】置空与对象析构之间仍有窗口：主线程可能刚在锁外看到 loop_!=nullptr
    //   并调 quit()，而子线程此刻正好清空并析构 —— 规范做法是 quit() 只在持锁时取副本、
    //   或干脆把 loop_ 的所有权交给 shared_ptr。本项目靠"主线程 quit 之后立刻 join、
    //   join 一定等到 threadFunc 返回"这个顺序把窗口挡住了（quit 的对象在 join 前仍然活着，
    //   因为 loop.loop() 还没 return，栈帧还在）—— 这是正确性依赖顺序的典型例子。
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = nullptr;
}