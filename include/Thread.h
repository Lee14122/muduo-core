/* ============================================================================
 * 【文件】include/Thread.h
 * 【一句话】给 std::thread 套一层壳：起线程后能拿到子线程的内核 tid、能起名、
 *          能选 join 还是 detach，并且保证析构时不会让进程 terminate。
 * 【Reactor 角色】线程壳（基础设施）。它不属于 Reactor 三件套，
 *          只是 EventLoopThread 用来"把 loop() 跑到另一个线程里"的工具。
 * 【依赖】<functional> ThreadFunc = std::function<void()>；
 *        <thread> 真正创建系统线程；<memory> shared_ptr 托管 std::thread；
 *        <unistd.h> pid_t（tid 的类型）；<string> 线程名；<atomic> 计数用的 atomic_int；
 *        noncopyable.h 一个线程对象代表一个真实线程，复制它等于凭空多一个线程，禁止。
 *        注意本文件不 include CurrentThread.h，tid 的获取发生在 src/Thread.cc 里。
 * 【谁用它】EventLoopThread 有一个 Thread 值成员（thread_），用它把 threadFunc 扔到子线程；
 *          main() 里的 baseLoop 不走这里（主线程就是主线程）。
 * 【先修知识】std::thread 的 join/detach 与"析构时 joinable 会 std::terminate"；
 *          内核 tid（gettid）与 pthread_t、std::thread::id 三者区别；POSIX 信号量；
 *          静态数据成员必须在类外定义一次。
 * 【难度】★★★☆☆   【建议耗时】20 分钟（配合 src/Thread.cc 的时序图）
 * 【自检】a) 为什么不直接用 std::thread 而要多这一层？说出三点理由。
 *        b) tid_ 为什么不能在主线程里 new 完 std::thread 就拿到？
 *        c) 析构函数里少了那个 detach()，程序会怎样？
 * ============================================================================ */

#pragma once

#include <functional>
#include <thread>
#include <memory>
#include <unistd.h>
#include <string>
#include <atomic>

#include "noncopyable.h"

// 【一句话】一次 Thread 对象 = 一次线程生命周期的管理者（不是线程本身）。
// 【设计】为什么要自封装 std::thread（面试爱问"muduo 为什么不直接用 std::thread"）：
//   (1) 拿 tid：std::thread 不提供内核 tid，只给一个不透明的 std::thread::id。
//       muduo 全库用 pid_t tid 做归属判断与日志（EventLoop::threadId_、Logger 前缀），
//       必须由**子线程自己**调 syscall(SYS_gettid) 再回传给主线程 —— 见 start() 的信号量。
//   (2) 拿名字：线程名（日志里 Thread1/Thread2）要和编号绑定，编号得由构造函数分配。
//   (3) 控制 join/detach：std::thread 析构时如果还 joinable 会直接 std::terminate() 杀进程；
//   这道"要么 join 要么 detach"的选择题是 std::thread 强加给所有封装者的
//   （析构时还 joinable 就 terminate），没有第三种正确答案。
//   (4) 两阶段初始化：构造 Thread 只登记函数与名字，start() 才真创建线程，
//       这让 EventLoopThread 的构造函数里可以先备好 thread_ 而不惊动操作系统。
// 【易错】Thread 对象本身不是线程安全的：start()/join() 预期由创建它的那个线程依次调用。
//   started_/joined_ 都是普通 bool，没有 atomic 也没有锁。
class Thread : noncopyable
{
public:
    // 【语法】成员 using，全名 Thread::ThreadFunc；EventLoopThread 构造时
    //   std::bind(&EventLoopThread::threadFunc, this) 的结果就是要塞进这里。
    // 【设计】线程体统一成"无参无返回值"，所有上下文靠 bind 提前绑好并按值拷贝，
    //   和 EventLoop::Functor 是同一个设计哲学（任务不引用调用方的栈）。
    using ThreadFunc = std::function<void()>;

    // 【语法】explicit 禁止"从 lambda 隐式构造一个 Thread"：
    //   没写 explicit 时 Thread t = []{...}(); 能过编译（单参构造 = 隐式转换），
    //   于是"随手赋值"就会悄悄起一个线程，非常危险；第二参有默认值也不影响
    //   explicit 对单参形态的约束。
    // 【设计】形参 ThreadFunc 按值接收（sink 参数惯用法），配合实现里的 std::move 免拷贝；
    //   name 用 const std::string&（不需要拥有，只是拷进 name_）。
    // 【易错】构造**不会**创建线程；忘记调 start() 的话这个对象就是一个纯数据的壳，
    //   析构时 started_==false ⇒ 既不 join 也不 detach，安全无事。
    explicit Thread(ThreadFunc, const std::string &name = std::string());
    // 【易错】这个析构的实际行为：started_ && !joined_ 时它 detach（见 src/Thread.cc），
    //   也就是"忘了 join 也能活"，代价是留下一个还在后台跑的分离线程；
    //   而"忘了 detach 也忘了 join"更直接：std::thread 析构时若还 joinable 就 terminate。
    //   这道选择题是所有 std::thread 封装者都躲不掉的（没有第三条路），
    //   本项目选了 detach 兜底。正解永远是宿主自己 join——EventLoopThread 的析构就
    //   老老实实 join 了（见 src/EventLoopThread.cc 里 ~EventLoopThread 那段）。
    //   本类真正缺的是防呆：start() 里没有 if (!started_) 判断，重复 start 会走到
    //   shared_ptr 赋值那条路（后果见 src/Thread.cc 里 start 的注释）。
    ~Thread();

    // 【线程】由"将来要等它的人"调用（本库里是 EventLoopThread::startLoop，即主线程）。
    // 【流程】③ 起 subLoop 的第一步；返回时子线程 tid_ 已就绪（靠 sem_wait 保证）。
    // 【易错】同一个 Thread 对象调两次 start() 是 UB（见 .cc 里对 shared_ptr 赋值的分析）。
    void start();
    // 【线程】阻塞调用方，直到子线程函数返回。EventLoopThread::~EventLoopThread 里
    //   先 quit() 再 join()，就是靠它把子线程收尸（⑫）。
    // 【易错】如果子线程的 loop 还在阻塞跑着而你先 join()，主线程就永久卡死；
    //   必须先让 loop 退出（quit → wakeup），join 才会返回。
    // 【对比】join vs detach：join 是"我等你跑完"，detach 是"从此各走各路"；
    //   detach 之后绝对不能再访问 thread_，而且线程函数引用的对象必须自己保证活得够久。
    void join();

    // 【语法】三个 getter 都没加 const（本项目风格如此，功能无碍），
    //   只有 tid()/name() 加了 const —— 不一致是历史遗留，不是设计。
    // 【设计】started() 与 tid() 是 start() 之后的只读视图；
    //   tid() 能返回有效值的前提是 start() 已经返回（信号量保证）。
    bool started() { return started_; }
    pid_t tid() const { return tid_; }
    const std::string &name() const { return name_; }

    // 【设计】静态成员函数：不依赖对象，问的是"这个进程一共造过几个 Thread"。
    //   返回 int 时 atomic_int 会隐式 load（memory_order_seq_cst），所以读是原子的。
    // 【易错】它数的是 Thread **对象**个数，不是"当前活着的线程数"——
    //   线程退出后计数不会减；EventLoopThreadPool 里 8 个 subLoop 会让它 +8。
    static int numCreated() { return numCreated_; }

    // 【设计】以下全是"只由创建 Thread 对象的线程写"的状态，唯一的跨线程写入是 tid_
    //   （由子线程写，主线程在 sem_wait 之后读），这也是整个类里唯一需要小心同步的地方。
private:
    void setDefaultName();

    // 【易错】两个都是普通 bool，无锁无 atomic：契约是"同一个 Thread 只能被一个线程
    //   操作"（本库里就是 EventLoopThread 的创建线程）。多线程同时 start() 就是数据竞争。
    //   joined_ 只被 ~Thread 用来决定"要不要 detach"，所以它必须和 join() 的调用严格配套。
    bool started_;
    bool joined_;
    // 【语法/设计】为什么包一层 shared_ptr 而不是直接 std::thread thread_：
    //   std::thread **不可拷贝**（拷贝构造被删除），只可移动。若做成值成员：
    //   (1) "构造 Thread 对象时不创建线程，start() 时才创建"这个两阶段初始化，
    //       只能靠移动赋值 thread_ = std::thread(...) 实现；而移动赋值会先析构旧对象，
    //       旧对象若还 joinable 就直接 std::terminate —— 指针版本则只是换个新对象，
    //       把"是否 joinable"的判断权完整地留在 start() 这一处。
    //   (2) shared_ptr 让"线程句柄"可以在异常/多次赋值间安全共享（本项目只用它当
    //       可空容器：start() 前为空 shared_ptr，start() 后指向真线程对象）。
    //   代价：一次堆分配 + 一层间接（只在 start() 发生一次，无所谓）。
    // 【⚠问题】用 shared_ptr 而不是 unique_ptr 没有道理，语义上 unique_ptr 更准确；
    //   副作用是"能拷贝 Thread 内部状态"这件事被悄悄允许了（虽然 Thread 本身 noncopyable）。
    // 【对比原版/⚠问题】语义上更准确的容器是 unique_ptr<std::thread>：一个线程只该有一个主人。
    //   用 shared_ptr 等于承认"线程句柄可以被共享"，于是"谁负责 join/detach"变得不确定
    //   （虽然 Thread 本身 noncopyable，实际共享不出来）。
    //   更讲究的做法是把线程体、名字、tid、握手对象打包进一个私有结构，再交给一个静态
    //   入口函数执行，避免闭包直接捕获 this（本环境无法联网核对，细节以原版源码为准）。
    std::shared_ptr<std::thread> thread_;
    // 【设计】注释"在线程创建时再绑定"点出了本类的核心难点：tid 只有**子线程自己**
    //   调 syscall(SYS_gettid) 才拿得到，主线程只能等它写完（同步手段见 Thread::start 的 sem_t）。
    // 【易错】类型是 pid_t 而不是 std::thread::id / pthread_t，三者不可互换比较；
    //   构造函数里初值 0：如果在 start() 前读 tid()，你会得到 0（0 在 Linux 上是主线程/
    //   进程 id 的语义，容易误判成"就是主线程"）。
    pid_t tid_;       // 在线程创建时再绑定
    // 【设计】线程体在构造时按值存下（bind 结果），start() 里由子线程执行。
    //   存副本而不是引用参数，是为了让"传临时 lambda 进来"也安全。
    // 【易错】func_ 里绑的往往是宿主对象的 this（EventLoopThread 绑的是
    //   &EventLoopThread::threadFunc, this）⇒ 宿主必须先于线程结束（靠 join 保证）。
    ThreadFunc func_; // 线程回调函数
    // 【设计】仅用于日志与调试（"IO0"/"Thread1"），不参与任何逻辑判断。
    //   注意本项目**没有**调用 pthread_setname_np，这个名字进不了 top/htop/gdb，
    // //   可核实的是：本仓库没有调用 pthread_setname_np（git grep 它 HEAD 无结果），
    // //   所以这个名字只出现在 muduo 自己的日志里；想让 top/gdb 看到得另写 setname。
    std::string name_;
    // 【设计】为什么是 std::atomic_int：numCreated_ 是**全局共享**的计数器，
    //   不同线程可以同时构造 Thread（例如两个 TcpServer 分别在不同线程 start()），
    //   ++ 是 read-modify-write，非原子就会丢号（两个线程拿到同一个编号）。
    // 【语法】static 数据成员必须在类外定义且只定义一次（src/Thread.cc 第一行那个定义），
    //   否则链接期 undefined reference；C++17 起可以写 inline static 省掉类外定义，
    //   本项目仍用传统写法（兼容性更好）。
    // 【易错】即使命名为 numCreated 也不代表"当前活跃线程数"，见 numCreated() 的注释。
    static std::atomic_int numCreated_;
};