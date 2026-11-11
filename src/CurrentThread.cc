/* ============================================================================
 * 【文件】src/CurrentThread.cc
 * 【一句话】定义那个"每线程一份"的 tid 缓存变量，并实现"没缓存就去问内核一次"的 cacheTid()。
 * 【Reactor 角色】基础设施（线程身份的落地处：include/CurrentThread.h 里声明的东西全在这里定义）。
 * 【依赖】只有 "CurrentThread.h"；syscall(SYS_gettid) 的原型与 SYS_gettid 常量都由那个头带进来。
 * 【谁用它】不直接被业务代码调用：src/Thread.cc 的线程函数、src/EventLoop.cc 的构造、
 *           include/EventLoop.h 的 isInLoopThread() 都是通过 CurrentThread::tid()（inline）
 *           间接走到这里，且只有"该线程第一次问 tid"时才会真的执行本文件的代码。
 * 【关于"原版"】本环境不能联网（github/raw 域名均不可达），仓库里也没有原版副本，
 *             所以下文所有【对比原版】条目都写成"通行描述"，只用来对照设计思路，
 *             接口名与写法一律以原版仓库为准；凡是标了"实测 / grep / man"的结论，才是本机可复现的事实。
 * 【先修知识】1) 声明与定义、链接（extern）；
 *             2) syscall(2) 与errno；
 *             3) 线程/进程/ TGID 的关系；
 *             4) 为什么 __thread 变量不需要锁。
 * 【难度】★☆☆☆☆（14 行，但藏着一道必问的面试题）   【建议耗时】15 分钟
 * 【自检】1) getpid()、pthread_self()、syscall(SYS_gettid) 三者哪个能写进日志用来区分线程？为什么？
 *         2) 删掉本文件的 __thread int t_cachedTid = 0; 会出什么错？（实测：undefined reference）
 *         3) cacheTid() 需要加锁或 atomic 吗？为什么？
 * ========================================================================== */
#include "CurrentThread.h"

namespace CurrentThread
{
    // 【语法】这一行是 t_cachedTid 的唯一定义（CurrentThread.h 里那行是 extern 声明）。
    //         【易错】删掉它，链接期报 undefined reference to 'CurrentThread::t_cachedTid'（实测）；
    //         把它挪回头文件（去掉 extern）则报 multiple definition（同样实测）。
    //         = 0 是显式初值，含义是"本线程还没查过自己的 tid"，0 兼作哨兵值（内核 tid 恒 > 0）。
    //         带初始化的 __thread 变量落在 .tdata/.tbss，pthread 创建线程时按模板给每个线程复制一份，
    //         所以"一个变量名"对应"N 个线程 × 1 份存储"，而链接上仍然只有这一份定义。
    // 【设计】为什么这里"整型 + 0 哨兵"就够，不需要 atomic/mutex：写它的人永远只有当前线程。
    //         【对比原版】通行描述：原版在同一个文件里还缓存过 tid 的字符串形式与线程名之类，
    //         所以它的日志能带上可读的线程标记；本项目只留 t_cachedTid 一个变量，日志里也就没有任何线程信息
    //         （原版细节以原版仓库为准，本环境无法核实）。
    __thread int t_cachedTid = 0;

    // 【流程】被 include/CurrentThread.h 的 tid() 调用，也就是被 ①（EventLoop 构造）
    //         和 ③（Thread::start 的线程 lambda）这两处间接触发；其余时刻 tid() 走缓存，不会进这个函数。
    //         【线程】哪个线程第一次调用它，它就在哪个线程里执行，并且只改自己那份 t_cachedTid。
    //         【易错】本函数"被调用"和"起作用"是两件事：即使被调用，若缓存已有值就直接返回，
    //         所以一个线程最多只真正执行一次里面的 syscall。
    void cacheTid()
    {
        // 【易错】这里再判一次 == 0，不是防御式的多余代码，而是让 cacheTid() 单独调用也安全：
        //         调用方（tid()）已经判过，所以正常情况下进来时必然还是 0，这次判断成本是一次寄存器比较。
        //         【设计】因为 t_cachedTid 是 __thread 的，这个"检查-然后-赋值"不会被别的线程插队，
        //         所以既不需要 std::atomic，也不需要经典的 double-checked locking 那套麻烦；
        //         【面试】被问"这里要不要加锁"的标准答案：不要，线程局部变量天然每线程独占。
        if (t_cachedTid == 0)
        {
            // 【系统调用】syscall(2) 是 libc 提供的"直接系统调用"可变参数包装：
            //         第 1 个参数是系统调用号，之后是它自己的参数。SYS_gettid 即 gettid(2) 的调用号
            //         （本机 x86-64 实测 186，SYS_getpid 是 39；调用号是架构相关的，arm64 上是另一个数）。
            //         成功返回本线程的 tid（内核里就是 task_struct->pid），失败返回 -1 并把错误码放进 errno；
            //         gettid 现实中没有失败路径（内核只是读当前 task 的字段），所以这里不检查返回值是可以接受的，
            //         【易错】但请顺手养成意识：以后照这个写法去包别的 syscall（例如 tgkill、futex）就必须判 -1。
            //         它不阻塞、不需要处理 EINTR，也不会有"半完成"状态 —— 这类"取信息"型系统调用是安全的。
            //         前缀 :: 表示"到全局命名空间找"（libc 的 C 函数都在全局），
            //         作用是避免与同名的 std::/命名空间内符号混淆，同时告诉读代码的人"这是 C 库函数，不是本项目的方法"。
            //         【语法】syscall() 的返回类型是 long，而缓存变量是 int/pid_t，
            //         所以必须 static_cast<pid_t>：把 8 字节收窄成 4 字节。
            //         这里安全的原因是内核 tid 的上限：本机 cat /proc/sys/kernel/pid_max = 4194304，
            //         远小于 int 的 21 亿，所以收窄不会溢出（换到 pid namespace 或调大该值也仍在 int 范围内）。
            //         远小于 int 的 21 亿；不加 cast 则 GCC 的 -Wconversion 会报警。
            // 【面试】三个"线程身份"的区别（高频，务必能一口说出）：
            //         1) syscall(SYS_gettid) → 内核 TID：本机 47790 这种小整数，全机唯一，
            //            出现在 /proc/<pid>/task/<tid>、top -H、ps -L、perf、strace -f、gdb 的 info threads，
            //            是"操作系统怎么称呼这个线程"的唯一答案，可以直接打印、可以直接比较。
            //            本仓库可核实的是 EventLoop 用 threadId_ 存它（src/EventLoop.cc 构造列表），
            //            但 Logger 从没把 tid 打进输出行，所以日志分不清是哪条 loop 打的（只能靠 this 指针）。
            //            本机 man 2 gettid 的 NOTES 明确：它返回的 thread ID 与 POSIX thread ID
            //            （即 pthread_self(3) 返回的不透明值）不是一回事；线程组首领的 getpid() 与 gettid() 相同，
            //            这正好解释了我们实测到的"主线程两个值相等"。
            //         2) getpid() → 进程 ID（内核术语 TGID，thread group id）：
            //            同一进程里所有线程返回值完全相同，所以它无法区分子线程；
            //            实测本机主线程 getpid()==gettid()（47788），子线程 getpid() 仍是 47788、gettid() 是 47790。
            //            【易错】"fork 出来的子进程 pid 是新线程 tid"这种直觉是错的，两个概念完全不同层级。
            //         3) pthread_self() → pthread_t：glibc 用户态的不透明句柄（本机实测值 140374214768448，
            //            本质是指向线程描述符的指针），标准没规定它能转成整数，比较必须用 pthread_equal()，
            //            不跨进程有效，内核和外部工具都不认识它 —— 拿它打日志毫无用处；
            //            本机 man 7 pthreads 还写明：pthread_t 只在进程内唯一，线程被 join/detach 之后
            //            该 ID 可能被复用，"使用生命周期已结束的 thread ID 行为未定义"。
            //            它确实比 syscall 快得多（只是读 libc 自己维护的 TLS，不陷入内核），
            //            但"快"换不来"能和 top/perf/gdb 对上号"，所以框架仍然选 tid，并且把 tid 缓存起来抵消那一次开销。
            //         【一句话】getpid 认进程，gettid 认内核线程，pthread_self 认 libc 自己。
            // 【流程/线程】写这份缓存的动作发生在"本线程"：主线程在 ①（EventLoop 构造），
            //         子线程在 ③（Thread::start 的 lambda，sem_post 之前），
            //         此后 ⑥⑨⑩⑪ 里 isInLoopThread() 的比较用的就是这个值，不再进内核。
            //         【易错】别把它和 EventLoop 的 one-loop-per-thread 检查搞混：那个检查用的是
            //         src/EventLoop.cc 里另一个独立的 __thread 变量 t_loopInThisThread（存 EventLoop*，不是 tid），
            //         与本缓存互不依赖；这里这份 tid 只服务"我是不是这个 loop 的主人"那种比较（isInLoopThread）。
            t_cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}