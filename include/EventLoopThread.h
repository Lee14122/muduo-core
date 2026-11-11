/* ============================================================================
 * 【文件】include/EventLoopThread.h
 * 【一句话】"一个线程 + 这个线程上的一个 EventLoop"的封装：startLoop() 起线程、
 *          等子线程把 loop 建好，然后把那个 loop 的裸指针交给调用方。
 * 【Reactor 角色】线程壳 + Loop 的宿主（把 Loop 搬进另一个线程的那层）。
 * 【依赖】noncopyable.h 一个宿主对象对应一条真实线程，禁止拷贝；
 *        Thread.h 真正创建系统线程（内含信号量握手）；
 *        <mutex> <condition_variable> 主线程与子线程交接 loop_ 用；
 *        <functional> ThreadInitCallback；<string> 线程名。
 *        只前向声明 EventLoop：这里只持有它的指针，构造/析构都在 .cc 里做。
 * 【谁用它】EventLoopThreadPool::start 里每个 IO 线程一个 EventLoopThread，
 *          startLoop() 的返回值被存进 loops_ 供 getNextLoop 轮询分配（③⑥）。
 * 【先修知识】条件变量必须搭配谓词（虚假唤醒 / 丢通知）；
 *          "对象在哪个线程构造、哪个线程析构"为什么重要（EventLoop 的 threadId_ 与 TLS）；
 *          裸指针不拥有对象意味着什么。
 * 【难度】★★★★☆   【建议耗时】30 分钟（重点在 .cc 的握手时序）
 * 【自检】a) startLoop() 返回的 EventLoop* 指向的对象在谁的栈上？谁能 delete 它？
 *        b) loop_ 为什么要用 mutex_ 保护？保护的是"写写竞争"还是"读写时序"？
 *        c) exiting_ 这个成员现在有什么实际作用？（这是本项目的【⚠问题】之一）
 * ============================================================================ */

#pragma once

#include <functional>
#include <mutex>
#include <condition_variable>
#include <string>

#include "noncopyable.h"
#include "Thread.h"

// 【语法】前向声明即可：本文件只出现 EventLoop*（成员、返回值），
//   不需要它的大小和成员，避免 EventLoop.h 与本文件互相包含。
class EventLoop;

// 【一句话】一个 EventLoopThread = 一条子线程 + 该线程栈上的一个 EventLoop。
// 【设计】one loop per thread 的"线程侧"实现：不是主线程 new 好 loop 再丢给线程，
//   而是**让子线程自己在它的函数里构造 loop**（见 .cc 的 threadFunc），
//   这样 EventLoop::threadId_ 记到的就是子线程，TLS 也登记在子线程上。
//   顺序反了（主线程构造、子线程 loop）整个线程归属判断就会全线失效。
// 【流程】③ TcpServer::start → EventLoopThreadPool::start → 循环 new EventLoopThread
//   + startLoop() 得到 N 个 subLoop；⑫ 析构时 quit + join 收尾。
// 【易错】它不是池：一个 EventLoopThread 只有一条线程、一个 loop。
//   "多路复用很多连接"是那个 loop 的事（one loop per thread + epoll）。
class EventLoopThread : noncopyable
{
public:
    // 【设计】回调参数是 EventLoop*：因为回调要在**子线程里**执行，
    //   主线程的对象还没准备好，唯一合理的作用域就是"刚建好的那个 loop"。
    //   典型用途：在各自的 loop 上注册定时器 fd / 初始化 per-thread 资源 / 建 UDP socket。
    // 【语法】std::function 默认构造为空对象，用 if (callback_) 判空调用；
    //   空 std::function 一旦被调用会抛 std::bad_function_call，所以判空是必须的。
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    // 【设计】两个参数都有默认值 ⇒ 可以 EventLoopThread t; 直接构造（无名、无初始化回调）。
    // 【设计】形参用 const 引用再拷进成员（没走 sink 参数 + std::move，是因为
    //   std::function 的移动收益在这个规模上可以忽略；两种写法都常见，够用就好）。
    // 【易错】构造只登记，不起线程；起线程的唯一入口是 startLoop()。
    //   这允许"先建好对象数组、再逐个 startLoop"（EventLoopThreadPool 正是这么用的）。
    EventLoopThread(const ThreadInitCallback &cb = ThreadInitCallback(),
                    const std::string &name = std::string());
    // 【流程】⑫：析构 = 请求退出（quit）+ 收尸（join），顺序不能反。
    // 【易错】绝不能在子线程正在 loop.loop() 时就直接销毁宿主对象：
    //   threadFunc 里绑的是宿主 this，宿主先没 ⇒ 子线程访问已释放内存。
    ~EventLoopThread();

    // 【一句话】起线程，并**阻塞等到子线程的 EventLoop 构造完**，然后把那个 loop 交出来。
    // 【线程】调用方通常是主线程（EventLoopThreadPool::start 在 mainLoop 里跑）。
    // 【设计】返回裸指针 = "不拥有"。这个 EventLoop 活在子线程的栈上，
    //   只能由子线程自己在 threadFunc 结束时销毁，调用方永远不许 delete
    //   （EventLoopThreadPool 析构里那句 "Don't delete loop, it's stack variable" 就在说这个）。
    // 【流程】③ 的关键同步点：start() 返回后 loops_ 立刻可用，
    //   随后 TcpServer::start 里的 runInLoop(Acceptor::listen) 不会看到空 loop。
    EventLoop *startLoop();

private:
    // 【线程】唯一的"子线程入口"：Thread 对象里存的就是 std::bind(&EventLoopThread::threadFunc, this)，
    //   由 Thread::start 的 lambda 在新线程中调用。
    // 【设计】private：外部只能通过 startLoop() 触发它，不允许直接调（在错误的线程调
    //   会导致 loop 建在错误的线程上，整个模型崩掉）。
    void threadFunc();

    // 【设计】裸指针 + 不拥有：指向子线程 threadFunc 栈上那个 EventLoop。
    //   生命周期约定：threadFunc 构造它 → 加锁写 loop_ → 一直 loop → 退出后加锁清 loop_ → 析构。
    //   所以 loop_ != nullptr 的期间，对象一定是活的（但拿到指针之后能不能用是另一回事，
    //   见 .cc 里"退出竞态"的分析）。
    EventLoop *loop_;
    // 【⚠问题】本项目的退化：这个标志只在 ~EventLoopThread 里被写 true，
    //   上游 muduo 里同名变量会在退出流程中被读一次（收尾时断言"我是被要求退出才结束"），
    //   而且上游另有按时间/按延时停止的一整套机制（本环境无法联网核对，以原版源码为准）。
    //   教学版把那些都删了，只剩这个没人读的 bool——读代码时别以为它有用。
    //   读代码时别被它误导：它现在纯属装饰，真正控制退出的是 EventLoop::quit_。
    bool exiting_;
    // 【设计】值成员（不是指针）：Thread 随宿主一起构造，构造函数里就能把
    //   线程体 bind 好（见 .cc 初始化列表）。
    // 【易错】成员声明顺序决定构造/析构顺序：thread_ 声明在 mutex_/cond_/callback_ 之前，
    //   所以**析构时 thread_ 最后被销毁**（析构按声明逆序：callback_ → cond_ → mutex_ → thread_ → …）。
    //   如果顺序反了（thread_ 放最前），析构时线程对象还在跑，而 mutex_/cond_ 已经被销毁 ⇒ UB。
    //   本项目靠"~EventLoopThread 函数体里先 quit + join，才轮到成员析构"双重保证，
    //   所以这个坑当前被绕过了，但成员顺序依然是值得记住的知识点。
    Thread thread_;
    // 【设计】只保护 loop_ 这一个指针的读写，临界区极短（绝不包含 syscall）。
    // 【易错】它不保护 EventLoop 对象内部任何东西 —— loop 的线程安全是另一套机制
    //   （单线程所有权 + pendingFunctors_ 上那把独立的锁）。
    std::mutex mutex_;             // 互斥锁
    // 【设计】与 mutex_ 配对，用于"子线程把 loop_ 备好"这一个事件的一次性通知。
    // 【面试】注意它不是用来唤醒 loop 的（那是 eventfd 的活）：
    //   cond_ 只能叫醒"正在 pthread_cond_wait 里等"的线程，
    //   而 loop 线程平时阻塞在 epoll_wait，cond_.notify 对它毫无作用 ——
    //   这正是 EventLoop 跨线程通信用 eventfd 而不用条件变量的原因。
    std::condition_variable cond_; // 条件变量
    // 【设计】线程初始化回调，被 std::bind 的结果间接带进子线程执行。
    //   声明在最后、初始化列表里也排最后，与声明顺序一致（无 -Wreorder 问题）。
    ThreadInitCallback callback_;
};