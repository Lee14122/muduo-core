/* ============================================================================
 * 【文件】include/CurrentThread.h
 * 【一句话】让任意一行代码用最便宜的方式知道"我现在在哪个线程"：第一次问内核拿 tid，之后只读线程本地缓存。
 * 【Reactor 角色】基础设施（线程身份）。EventLoop 靠它认定"我是谁的 loop"，
 *                   isInLoopThread() 是全框架跨线程投递任务的守门员。
 * 【依赖】<unistd.h>（syscall 的声明、pid_t、较新 glibc 的 gettid）；
 *         <sys/syscall.h>（SYS_gettid 这类"系统调用号"常量）。
 *         除了这两个系统头，不依赖本项目任何其他类 —— 它必须能被任何文件安全 include（包括 Logger 那种底层文件）。
 * 【谁用它】src/EventLoop.cc 构造列表 threadId_(CurrentThread::tid())（①）；
 *           src/Thread.cc 子线程 lambda 里 tid_ = CurrentThread::tid()（③ 起线程）；
 *           include/EventLoop.h 的 isInLoopThread()（⑥ 线程跳转、⑨ 用户 send 判线程、⑪ 关闭与删连接）。
 * 【关于"原版"】本环境不能联网（github/raw 域名均不可达），仓库里也没有原版副本，
 *             所以下文所有【对比原版】条目都写成"通行描述"，只用来对照设计思路，
 *             接口名与写法一律以原版仓库为准；凡是标了"实测 / grep / man"的结论，才是本机可复现的事实。
 * 【先修知识】1) 线程与进程在内核里是什么关系（一个 task_struct 一个 tid）；
 *             2) 什么是"系统调用"，为什么它比一次普通函数调用贵；
 *             3) inline 函数与链接（多个 .cc include 同一个定义会怎样）；
 *             4) extern 声明与定义的区别。
 * 【难度】★★☆☆☆（文件只有 20 行，但每行都是考点）   【建议耗时】20 分钟
 * 【自检】1) __thread 和 thread_local 有什么差别？这里为什么能用 __thread？
 *         2) 去掉 extern 之后，两个 .cc 各有一份 t_cachedTid，缓存会发生什么？链接会怎样？（实测：multiple definition）
 *         3) __builtin_expect 写反了会导致功能错误吗？
 *         4) 为什么 tid() 要缓存，而 pthread_self() 不需要缓存？
 * ========================================================================== */
#pragma once

#include <unistd.h>
#include <sys/syscall.h>

// 【依赖】两个 include 各管一件事：<sys/syscall.h> 给 SYS_gettid（系统调用号，x86-64 实测 186；
//         SYS_getpid 是 39），<unistd.h> 给 syscall() 这个包装函数与 pid_t 类型。
//         真正的调用发生在 src/CurrentThread.cc，这里 include 是因为头文件里的 inline 函数
//         在别的 TU 展开时需要看见这些声明（头文件必须自给自足）。
// 【设计】用 namespace 而不是 class：这组东西没有对象语义、没有构造析构，
//         只是"关于当前线程的几个函数"，用 namespace 不进继承树、不产生对象，
//         调用方写 CurrentThread::tid() 本身就把"当前线程的 tid"说清楚了，
//         不必为一个无状态的工具专门造一个类、再操心该造几个对象、能不能拷贝这些问题。
//         【对比原版】通行描述：原版同样是 namespace CurrentThread + __thread 缓存这条思路，
//         另外还提供过取 tid 的自由函数与线程名相关接口（因为原版的日志行要打线程名与线程 id，
//         而本项目的日志里根本没有线程字段）；接口名与细节以原版仓库为准。
namespace CurrentThread
{
    // 【系统调用】先弄清楚"线程身份"到底有几种（面试高频，详见 src/CurrentThread.cc 的对比表）：
    //         Linux 内核里"线程 = 一个可被调度的 task"，每个 task 有自己的 tid；
    //         同一进程内的所有线程共享一个 TGID，而 getpid() 返回的正是这个 TGID。
    //         所以"进程里第 3 个线程"只能用 tid 表达，不能用 pid。
    // 【语法】__thread 是 GCC/Clang 的线程局部存储（TLS）关键字：
    //         加了它，这个变量被放进 .tbss/.tdata 段，每个线程启动时都获得独立的一份（并按定义处的初值初始化），
    //         于是"一个全局变量"变成"每线程一个变量"，读写它不需要任何锁。
    //         与 C++11 的 thread_local 的关系：thread_local 覆盖面更大（可用于带非平凡构造/析构的对象，
    //         比如 std::string，退出时会自动析构），__thread 只适用于 POD/静态存储期类型；
    //         这里缓存的是一个 int（POD、静态存储期），两者都能用；本仓库写的是 __thread。
    //         【易错】TLS 变量的地址在线程退出后就失效，绝不能把 &t_cachedTid 存到别处晚点用。
    // 【语法】extern 表示"这只是声明，定义在别的翻译单元里"（唯一定义在 src/CurrentThread.cc）。
    //         【易错】如果把带初值的定义直接写进本头文件（去掉 extern），
    //         那么每个 include 它的 .cc 都会试图生成自己的一份，链接期直接报
    //         multiple definition of 'CurrentThread::t_cachedTid'（实测，报在 .tbss 段）；
    //         反之把这一行删掉只留定义在 .cc，别的 TU 又看不到声明，编译不过。
    //         头文件"extern 声明 + .cc 定义"是这里唯一正确的分工 ——
    //         注意"一份定义"讲的是链接（编译单元之间），不是线程：线程之间仍然是每人一份。
    //         【易错】另外 inline 函数里如果放的是非 extern 的变量定义，就会出现"每个 TU 一份副本"的
    //         经典坑（各副本互不相干，缓存看起来永远不生效），本项目通过 extern 绕开了它。
    // 【⚠问题】类型写成 int 而不是 pid_t：语义上 tid 是内核的 pid_t；
    //         定义处（src/CurrentThread.cc）做的是 static_cast<pid_t>，而这里声明成 int，
    //         在 Linux 上 pid_t 就是 int，所以没差别，但换平台（某些 BSD 上 pid_t 是 long）就不一致了。
    //         （通行描述：原版也按整数处理 tid，以原版源码为准；本仓库可核实的是声明为 int、定义处 cast 成 pid_t。）
    extern __thread int t_cachedTid; // 保存tid缓存 因为系统调用非常耗时 拿到tid后将其保存

    // 【设计】cacheTid() 只在头文件里声明，实现放 src/CurrentThread.cc：
    //         函数主体是一次陷入内核的 syscall，一个线程一辈子只真正执行一次（第二次进来 if 不成立就返回），
    //         内联进头文件只会让每个 .cc 多一份代码副本，收益为零。
    //         【流程】两个时机：① 主线程构造 EventLoop 时第一次触发；
    //                 ③ 每个 subLoop 线程在 Thread::start 的 lambda 里第一次触发
    //                 （那行 tid_ = CurrentThread::tid() 在 sem_post 之前，所以 Thread::start 返回时值已就绪）。
    //         【线程】永远是"当前线程"填"当前线程自己那份缓存"，所以本函数不需要 atomic/mutex。
    void cacheTid();

    // 【语法】inline 为什么必须写在这里（而不是把定义搬进 .cc）：
    //         函数体就放在头文件里，会被很多 .cc include；inline 让"多处出现同一份定义"合法
    //         （ODR 豁免）并由链接器合并成一个符号。实测把 inline 去掉后链接报
    //         multiple definition of 'N::get()' —— 这个函数正是同样形状的"头文件里的非 inline 定义"。
    //         【易错】inline 不是"命令编译器必须内联"：编译器可以自己决定不内联，
    //         也可以内联一个没写 inline 的函数。这里的 inline 主要作用有两个：
    //         ① 链接层面的合法性；② 让 tid() 有机会在调用点展开成 3 条指令，而不是真的 call 一次。
    //         为什么不"只声明、定义在 .cc"：那样每次调用都是一次跨 TU 的函数调用，编译器无法内联（除非开 LTO），
    //         而 tid() 在热路径上（下面【谁调用】），所以必须内联。
    //         【易错】原注释说"内联函数只在当前文件中起作用"不准确：inline 的定义对每个包含它的 TU 都可见，
    //         真正"只在当前文件可见"的是匿名命名空间或 static 函数（内部链接）。
    // 【设计】为什么要缓存 tid，而不是每次 syscall：
    //         系统调用要走 syscall 指令 → CPU 切到内核态 → 保存/恢复寄存器与栈 → 内核读当前 task 的 tid 字段
    //         → 原路返回，量级是几百纳秒（比一次用户态函数调用贵一到两个数量级，还会打断流水线/污染分支预测），
    //         并且 Spectre 缓解措施会进一步抬高成本。
    //         而 tid() 的调用点在框架里非常密集：isInLoopThread() 出现在
    //         ⑥（runInLoop/queueInLoop 把连接建立投给 subLoop）、⑨（用户在任何线程 conn->send() 都要判线程）、
    //         ⑩⑪（handleWrite/shutdown/removeConnection 一路判），
    //         不缓存等于每个网络操作都白白多一次内核往返。
    //         缓存后：一次 TLS 变量读 + 一次整数比较 + 返回，全部在用户态，纳秒级以下。
    //         【易错】缓存的前提是"tid 在线程活着期间不会变"，这个前提由内核保证；
    //         线程退出后 tid 会被内核回收重用，所以不要把 tid 存进长期容器当 key 又不处理复用。
    //         【面试】"怎么高效获取当前线程 id？" 答：第一次 syscall(SYS_gettid) 之后缓存到
    //         thread_local/__thread 变量，后续只读缓存 —— glibc 2.30 的 gettid() 内部就是这么做的。
    // 【语法】__builtin_expect(表达式, 期望值) 是给编译器的分支预测提示，这里期望 0（假）：
    //         含义是"t_cachedTid == 0 这种情况极少发生（只有本线程第一次调用时才是真）"，
    //         于是编译器把 cacheTid() 这段冷路径挪到函数尾部/冷代码段，让热路径（直接 return）
    //         连续排列，减少指令 cache miss 和分支预测失败。
    //         【易错】它**不改变**条件为真/假的语义，写反了（把期望值写成 1）功能依然正确，只是布局变差；
    //         它是纯优化提示，不是断言、不是开关。C++20 的标准写法是
    //         if (t_cachedTid == 0) [[unlikely]] { ... }（或 [[likely]]），不必依赖 GCC 扩展。
    //         【设计】用"== 0"当"还没缓存"的哨兵值：内核分配的 tid 恒为正数，0 不会与任何真实 tid 撞车，
    //         所以拿 0 兼作"未初始化"标记是安全的，比额外加一个 bool 少一份状态。
    inline int tid() // 内联函数只在当前文件中起作用
    {
        if (__builtin_expect(t_cachedTid == 0, 0)) // __builtin_expect 是一种底层优化 此语句意思是如果还未获取tid 进入if 通过cacheTid()系统调用获取tid
        {
            cacheTid();
        }
        return t_cachedTid;
    }
    // 【易错】这份缓存是"每线程一份"，所以：
    //         ① 主线程调用过 tid() 之后，子线程第一次调用仍然会走一次 syscall（它自己那份还是 0）；
    //         ② 子线程拿到的 tid 永远不会污染主线程的值；
    //         ③ 也正因为它线程独占，整个文件没有出现任何 mutex/atomic —— 这是"用状态隔离替代加锁"的典型样本，
    //            与 src/Logger.cc 里那份共享的 logLevel_（有 data race）正好是反面教材。
    //         【对比原版】通行描述：原版在这里还缓存过 tid 的字符串形式与线程名，所以它的日志
    //         能带可读的线程前缀；本项目只缓存数值、日志里也没有线程字段（变量名以原版仓库为准）。
    //         因此在本项目里想知道"是哪条 loop 打的日志"，只能靠 src/EventLoop.cc 那些 LOG_INFO 里打印的 this 指针。
}