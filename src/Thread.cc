/* ============================================================================
 * 【文件】src/Thread.cc
 * 【一句话】实现"起一个线程并当场拿到它的内核 tid"：用 POSIX 无名信号量在主线程和
 *          子线程之间做一次握手，另外负责 join/detach 与自动编号命名。
 * 【Reactor 角色】线程壳（基础设施）。EventLoopThread 靠它把 loop() 搬到子线程。
 * 【依赖】"Thread.h" 自己的声明；"CurrentThread.h" 拿 syscall(SYS_gettid) 的真实 tid；
 *        <semaphore.h> sem_init/sem_wait/sem_post（POSIX unnamed semaphore）。
 * 【谁用它】EventLoopThread::startLoop 调 thread_.start()；
 *          EventLoopThread::~EventLoopThread 调 thread_.join()；
 *          业务代码从不直接碰 Thread。
 * 【先修知识】std::thread 析构时 joinable ⇒ std::terminate；
 *          信号量是"计数器 + 等待队列"，post 不会丢票；
 *          happens-before（为什么 sem_post/sem_wait 配对之后共享变量就可见）；
 *          lambda 按引用捕获 [=]/[&] 的生命周期风险。
 * 【难度】★★★★☆（并发时序是重点，务必亲手画一遍下面那张时序图）
 *          【建议耗时】35 分钟
 * 【自检】a) 画主线程/子线程时序，说明 sem 解决的是什么竞态，去掉它会看到什么值。
 *        b) 为什么 sem 是 start() 的栈上局部变量却是安全的？
 *        c) 析构里那个 detach 能不能删？删了会发生什么（具体到进程行为）？
 * ============================================================================ */

#include "Thread.h"
#include "CurrentThread.h"

#include <semaphore.h>

// 【语法】static 数据成员"类内声明、类外定义"：这一行才是 numCreated_ 这个变量本体，
//   放在 .cc 是为了保证整个进程只有一份定义（放头文件会多重定义，C++17 才可用 inline static）。
// 【设计】初值 0 而不是 1，因为 setDefaultName 里用的是前置 ++numCreated_，
//   第一个线程就叫做 Thread1（更符合直觉）。
std::atomic_int Thread::numCreated_(0);

// 【流程】EventLoopThread 构造自己的 thread_ 成员时会走到这里（值成员，随宿主构造）。
// 【设计】线程体 func_ 按值收下并 std::move，此刻**还没有创建任何线程**；
//   这就是"两阶段初始化"：构造登记，start() 才惊动内核。
// 【语法】std::move(func) 把形参（已经是值）搬进成员，避免 std::function 再拷一次
//   （拷贝 std::function 可能触发堆分配，移动则只是搬指针）。
// 【易错】初始化列表顺序必须与头文件声明顺序一致（started_ → joined_ → tid_ → func_ → name_），
//   本项目是对的；写反了 -Wreorder 会警告"实际按声明顺序初始化"。
//   注意 setDefaultName() 在函数体里调用，那时所有成员都已经构造完，读 name_ 才安全
//   ——【易错】"在成员函数里读成员"永远安全，"在初始化列表里读别的成员"才要命的顺序问题。
Thread::Thread(ThreadFunc func, const std::string &name)
    : started_(false)
    , joined_(false)
    , tid_(0)
    , func_(std::move(func))
    , name_(name)
{
    // 【设计】放在构造函数体末尾：只有调用者没给名字时才补一个 Thread%d。
    //   本项目的 EventLoopThreadPool 会传名字（"IO0"/"IO1"…），所以线程池里的线程
    //   通常不会用到默认名；只有裸用 Thread 的人才会看到 Thread3 这种编号。
    setDefaultName();
}

// 【流程】⑫ 的一部分：EventLoopThread 已经 join 过 ⇒ joined_ 为 true ⇒ 这里什么都不做。
// 【必考】为什么这里必须 detach（或 join）：
//   std::thread 的析构函数规定——如果对象仍 joinable（线程已启动且既没 join 也没 detach），
//   就调用 std::terminate() 直接终止整个进程（不是抛异常，try/catch 拦不住）。
//   所以"忘 join 就崩"是 std::thread 的默认行为，本类的做法是：
//   你既然忘了 join，那我就 detach，让线程继续在后台跑，进程至少不死。
// 【易错】detach 换来的安全是假的：线程函数 func_ 里绑的是宿主对象的 this
//   （EventLoopThread::threadFunc, this）。宿主析构完线程还在跑 ⇒ 它访问的是已销毁对象
//   ⇒ 未定义行为（常见现象：随机段错误、日志里出现半个字符串、死锁）。
//   正确姿势永远是"宿主析构前自己 join"，本项目 EventLoopThread 正是这么做的，
//   所以这条 detach 分支实际上只兜住"根本没人管 Thread 对象"的错误用法。
// 【设计】为什么选 detach 而不是 join 兜底：join 会把"析构"变成阻塞操作，
//   在异常路径或容器析构里更容易写出死锁；detach 至少保证进程不死。
//   但 detach 只解决"不 terminate"，不解决生命周期问题——该 join 的还是得 join。
Thread::~Thread()
{
    if (started_ && !joined_)
    {
        // 【系统调用/语法】detach() 之后 thread_ 指向的那个 std::thread 对象
        //   内部句柄被释放（joinable() 变 false），线程与 C++ 对象脱钩，由内核继续调度；
        //   之后再也不能对同一个 std::thread 调 join/detach（都会抛 invalid_argument）。
        // 【易错】detach 是"不阻塞"的，它不等线程结束，只解除关系。别把它当成"温和地结束线程"——
        //   C++ 没有强制结束线程的 API（只能靠标志位让线程函数自己 return，或 cancel 这类危险操作）。
        thread_->detach();                                                  // thread类提供了设置分离线程的方法 线程运行后自动销毁（非阻塞）
    }
}

/* 【流程】③ 的一环：EventLoopThread::startLoop 第一句就是它；返回后主线程立刻
 *   可以安全地读 thread_.tid()（这是本函数存在的全部意义）。
 * 【时序图】为什么必须要 sem —— "主线程得拿到子线程的 tid 才能返回"：
 *
 *     主线程（调 start）                        子线程（内核何时调度？不确定！）
 *     ----------------                          ------------------------------
 *     started_ = true
 *     sem_init(&sem, false, 0)   计数=0
 *     new std::thread(lambda)  ───────────────>  lambda 开始执行
 *     sem_wait(&sem)  [无票 → 睡眠]                tid_ = CurrentThread::tid()
 *          ...阻塞中...                              sem_post(&sem)  计数 0→1
 *     被唤醒，sem_wait 返回    <───────────────┘
 *     start() 返回（tid_ 已可读、且可见）        func_()  ← 真正的线程体（threadFunc）
 *
 *   没有 sem 会怎样：std::thread 构造返回后，主线程与子线程是并发关系。
 *   主线程若在 start() 一返回就读 tid_，可能子线程还没被调度 ⇒
 *   读到构造时的初值 0，甚至读到半写状态（pid_t 的写在实践中是原子的，
 *   但标准上仍是 data race ⇒ UB）。这是"创建线程后立刻取子线程信息"的经典竞态。
 *   反过来让主线程 sleep(10ms) 之类的"猜时间"更是要命的写法。
 * 【设计】sem 提供了两件事：(1) 阻塞等待；(2) happens-before —— sem_post 之前的写
 *   （tid_ = ...）保证对 sem_wait 之后的读可见。这正是"用同步原语换内存序"的标准案例。
 * 【面试】"muduo 怎么把子线程的 tid 传给主线程的？" —— 答 sem 握手 + 画这张图，
 *   再补一句"现代写法可以用 std::promise/std::future，语义相同还不用管栈对象生命周期"。
 */
void Thread::start()                                                        // 一个Thread对象 记录的就是一个新线程的详细信息
{
    // 【易错】started_ 先置 true 是必须的（否则若后面立刻抛异常/返回，析构会以为没启动）；
    //   但它和"线程真的启动了"没有因果关系，真正的启动点是下一行的 new std::thread。
    started_ = true;
    // 【语法】sem 是 start() **栈上的局部变量**，靠下面的 sem_wait 保证
    //   "start() 返回之前，子线程已经不再需要它"——这是"用同步换生命周期"的典型手法。
    // 【易错】如果哪天有人把 sem_wait 挪到别的函数里、或者改成 detach 后立刻 return，
    //   子线程就会对着一个已销毁的 sem_t 调 sem_post ⇒ UB（实测常表现为崩溃或踩栈）。
    //   阅读这段代码时，把"sem_wait 是 sem 的生命周期守卫"这句话记住就够了。
    sem_t sem;
    // 【系统调用】sem_init(&sem, pshared, value)：
    //   pshared=false ⇒ "进程内、线程间共享"（写 true 是给 fork 共享内存用的跨进程信号量，
    //     本项目场景用不上，传 true 反而是错的）。
    //   value=0 ⇒ 初始无票：主线程一 sem_wait 就会睡，必须等子线程 sem_post 才醒。
    //     如果写成 value=1，主线程会直接冲过去 ⇒ 握手失效，回到"可能读到 tid_==0"的竞态。
    //   成功返回 0，失败 -1（ENOMEM/EINVAL）。
    // 【⚠问题】返回值被忽略。实际上一个刚在栈上分配的 sem_t 几乎不可能失败，
    //   但严格讲应该判 != 0 并处理；另外函数结尾没有 sem_destroy(&sem)——
    //   POSIX 要求"不再使用应销毁"。glibc 的无名信号量没有额外内核资源，
    //   漏掉 sem_destroy 在这里不泄漏任何东西，但属于不严谨（面试被问到要能答上来）。
    sem_init(&sem, false, 0);                                               // false指的是 不设置进程间共享
    // 开启线程
    // 【语法】lambda 用 [&] 按引用捕获：捕获了栈上的 sem 和成员 tid_（经由隐式 this）。
    //   注意按引用捕获 this 是"**拷贝 this 指针本身**"，不是拷贝整个对象；
    //   所以 tid_ = ... 写的是 this->tid_，也就是调用 start() 的那个 Thread 对象的成员。
    // 【易错】两条生命周期红线，本项目都靠 sem_wait 与 join 压住了：
    //   1) [&] 捕获的 sem 在 sem_wait 返回后仍被子线程持有（只是不再使用）；
    //   2) 捕获的 this 在 func_() 执行期间必须活着 —— Thread 对象若先析构，
    //      子线程里这句 tid_ = CurrentThread::tid() 就是写已释放内存。
    //      更现实的场景是 detach：detach 后 Thread 对象被销毁，而 func_() 正在跑，
    //      它绑的宿主（EventLoopThread）也没了 ⇒ 全线悬垂。
    // 【设计】为什么这里不用 [=] 或显式捕获：tid_ 是成员，只能经由 this 写；
    //   值捕获 sem 会把信号量拷进闭包（sem_t 拷贝是 UB），所以必须按引用。
    // 【⚠问题】std::thread 构造函数如果抛 std::system_error（线程数超限 EAGAIN），
    //   此时 started_ 已经是 true、sem 已 init、线程没起来 ⇒ 主线程 sem_wait 永久阻塞。
    //     //   上游是否处理这条分支以原版源码为准；可核实的是本仓库没有任何 catch，知道有坑即可。
    // 【语法】shared_ptr 的赋值：右边 new 出一个真线程对象再包成 shared_ptr。
    //   如果 thread_ 之前非空且旧线程还 joinable，旧 std::thread 会在引用计数归零时
    //   析构 ⇒ std::terminate ⇒ 进程死。这就是"同一个 Thread 不能调两次 start()"的原因，
    //   本类没有加 started_ 判断来防呆（【⚠问题】）。
    thread_ = std::shared_ptr<std::thread>(new std::thread([&]() {
        // 【设计】顺序至关重要：先写 tid_，再 sem_post。反过来（先 post 再写）
        //   主线程醒来读到的还是 0 —— 握手形同虚设。这是"发布数据后放行等待者"的通用范式。
        // 【线程】这一行在**子线程**执行：CurrentThread::tid() 读的是本子线程的
        //   __thread 缓存（首次会 syscall(SYS_gettid) 并缓存），所以拿到的必然是新线程的 tid。
        // 【易错】千万别在主线程里写这个赋值：那只会得到主线程自己的 tid。
        tid_ = CurrentThread::tid();                                        // 获取线程的tid值
        // 【系统调用】sem_post：把计数 +1；若有线程在 sem_wait 上睡觉，唤醒其中一个。
        //   post 是"存票"语义，即使主线程还没走到 sem_wait，票也不会丢（这点优于
        //   没有配对的 cond_.notify + 布尔标志的裸写法）——所以这里不存在
        //   "子线程先 post、主线程后 wait ⇒ 永久阻塞"的丢信号问题。
        sem_post(&sem);
        // 【流程】③/⑦ 的分岔口：func_() 就是 EventLoopThread::threadFunc，
        //   从这里开始，子线程进入自己的 EventLoop 世界（④ poll 循环）。
        // 【易错】func_() 返回 ⇒ 线程函数结束 ⇒ 子线程退出。所以 threadFunc 里
        //   不能 return 得太早（它的 loop.loop() 就是防止提前返回的结构）。
        func_();                                                            // 开启一个新线程 专门执行该线程函数
    }));

    // 这里必须等待获取上面新创建的线程的tid值
    // 【系统调用】sem_wait：计数 >0 则 -1 并立即返回；==0 则阻塞（不可被信号中断的
    //   版本；被信号打断时 POSIX 规定返回 -1/EINTR，glibc 通常自己重试）。
    // 【设计】这一行同时干了三件事：阻塞主线程直到握手完成、建立内存可见性、
    //   守卫上面那个栈上 sem 的生命周期。
    // 【易错】它是**取消点**（cancellation point）：如果同进程里有 pthread_cancel 玩法，
    //   线程可能在这里被终止，sem 就泄漏了——本项目不涉及，仅提示。
    // 【对比原版】同一件事在 C++11 里还有更地道的写法：std::promise<pid_t> + std::future
    //   （子线程 set_value，主线程 get 拿值，天然带 happens-before，也不用操心栈上对象的
    //   生命周期），或者 mutex + condition_variable + 一个 bool 标志（自研 Latch 就是封装）。
    //   上游用的是哪一类以原版源码为准（本环境无法联网核对）；sem_t 的取舍是代码最短。
    sem_wait(&sem);
}

// C++ std::thread 中join()和detach()的区别：https://blog.nowcoder.net/n/8fcd9bb6e2e94d9596cf0a45c8e5858a
// 【流程】⑫ 的收尸动作：由 EventLoopThread::~EventLoopThread 调用（先 quit 再 join）。
// 【线程】调用方（主线程）在这里阻塞，直到子线程的 threadFunc 返回。
// 【易错】joined_ = true 必须在 join() 之前写：如果 join 抛异常（比如被子线程自己 join，
//   会抛 resource_deadlock_would_occur），标志位也得是对的，否则 ~Thread 会再去 detach
//   一个已经不可 detach 的线程 ⇒ 再抛一次异常，析构里抛异常 = terminate。
// 【⚠问题】没检查 started_：对没 start() 过的 Thread 调 join()，thread_ 是空 shared_ptr ⇒
//   解引用空指针，段错误。EventLoopThread 的析构用 loop_ != nullptr 间接挡住了这条路。
void Thread::join()
{
    joined_ = true;
    thread_->join();
}

// 【一句话】没传名字就按"已创建线程数"编一个 Thread%d。
// 【设计】++numCreated_ 无条件执行（即使本次有名字不占编号），
//   所以编号可能"跳号"：三个线程里两个自带名字，默认名的那个可能叫 Thread3。
//   所以日志里出现"Thread3"而进程其实只起过两个线程，是正常现象，别当 bug 查。
// 【语法】char buf[32] = {0} 把整个数组零初始化（只有第一个元素写 0，其余由标准保证为 0）。
// 【系统调用】snprintf(buf, sizeof buf, "Thread%d", num)：sizeof 在这里是对的，
//   因为 buf 是定长数组（不是指针），sizeof 编译期算出 32；"Thread" 6 字符 +
//   int 最多 11 字符 + 结束符 = 18，绝不溢出。
// 【易错】如果哪天有人把形参改成 char* buf 传进来，sizeof buf 就变成 8（指针大小），
//   截断/溢出一触即发——这是 snprintf 最经典的坑。
// 【语法】name_ = buf：const char* 触发 std::string 的隐式转换赋值，正确且直观。
void Thread::setDefaultName()
{
    int num = ++numCreated_;
    if (name_.empty())
    {
        char buf[32] = {0};
        snprintf(buf, sizeof buf, "Thread%d", num);
        name_ = buf;
    }
}