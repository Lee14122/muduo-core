/* ============================================================================
 * 【文件】include/EventLoopThreadPool.h
 * 【一句话】把 N 个 EventLoopThread 管成一个数组，对外提供"下一个该用哪个 loop"
 *          的轮询选择，以及"所有 loop"的清单。
 * 【Reactor 角色】线程池（多个 subReactor 的注册表 + 负载均衡器）。
 *          它本身不含任何 IO 逻辑，只负责"生线程、收 loop、发 loop"。
 * 【依赖】noncopyable.h 池子拷贝了会重复起线程，禁止；
 *        <vector> threads_ 与 loops_ 两个数组；<memory> unique_ptr 独占 EventLoopThread；
 *        <string> 线程名前缀；<functional> ThreadInitCallback。
 *        只前向声明 EventLoop/EventLoopThread：本头文件只存它们的指针，
 *        而 unique_ptr<EventLoopThread> 要求 EventLoopThread 完整类型 ——
 *        所以析构函数必须写在 .cc 里（那里 include 了 EventLoopThread.h）。
 * 【谁用它】TcpServer：构造时建池（只存参数）、start() 时 threadPool_->start(cb) 起线程、
 *          newConnection 时 threadPool_->getNextLoop() 选 IO 线程（⑥）。
 * 【先修知识】master/slave Reactor（mainReactor 只 accept，subReactor 只读写）；
 *          裸指针不拥有对象；unique_ptr 放在 vector 里的移动语义；
 *          "单线程所有权"为什么能让 next_ 不用原子。
 * 【难度】★★☆☆☆   【建议耗时】20 分钟（难点在 .cc 的 start 与 loops_ 的所有权）
 * 【自检】a) loops_ 里的 EventLoop* 指向哪块内存？谁负责销毁？为什么这里绝不 delete？
 *        b) numThreads_==0 时 getNextLoop 返回什么？程序还能跑吗？
 *        c) next_ 为什么不是 atomic_int？什么情况下这个前提会被破坏？
 * ============================================================================ */

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <memory>

#include "noncopyable.h"
// 【语法】两个前向声明：本文件只用 EventLoop*/EventLoopThread* 存进 vector，
//   不需要完整类型。注意 EventLoop 与 EventLoopThread 是"两个不同的东西"：
//   一个是循环本体（可能在主线程栈上，也可能在子线程栈上），
//   一个是"线程 + 它的循环"的壳。池子持有壳（拥有），只登记循环（不拥有）。
class EventLoop;
class EventLoopThread;

// 【一句话】EventLoopThreadPool = "baseLoop（别人的）+ N 个 subLoop（各子线程的）"的花名册。
// 【设计】多线程 Reactor 的分工（这就是本项目必须有线程池的原因）：
//   - mainLoop/baseLoop（主线程）：只干三件事——accept 新连接、执行跨线程任务、
//     把连接派给某个 subLoop（⑤⑥）。它不碰任何 connfd 的读写。
//   - subLoop#1..#N（IO 线程）：只管自己名下连接的读写（⑦⑧⑨⑩⑪）。
//   好处：accept 不会因为某个连接的用户回调慢而被拖住；IO 处理被均摊到多个核；
//   每个连接从生到死只由一个线程处理 ⇒ 全程无锁。
// 【易错】"线程数 = 连接数"是错的；线程数一般取 CPU 核数（甚至核数的一半），
//   再多就只是把连接切碎、降低每个 loop 的批量处理能力。
// 【设计】本仓库里池子的对外面很窄：setThreadNum / start / getNextLoop / getAllLoops /
//   started / name 六个（可以在头文件里数出来）。没有停止接口，没有"每个 loop 当前
//   几个连接"的统计，也没有运行期改线程名的入口（git grep "void stop" HEAD 无结果）——
//   教学版的取舍是"只保留 Reactor 分发必需的三件事：生线程、收 loop、发 loop"。
class EventLoopThreadPool : noncopyable
{
public:
    // 【设计】别名与 EventLoopThread 里的同名：两个类各自定义，语义一致
    //   （给"每个 loop 线程"跑一次的初始化钩子），不是共享定义。
    //   TcpServer 里的 ThreadInitCallback_ 成员用的就是这类 std::function<void(EventLoop*)>，
    //   靠"结构相同"互相赋值。
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    // 【流程】② TcpServer 构造时建池：此刻只记 baseLoop 和名字，**不起任何线程**。
    // 【设计】构造参数只有裸指针 baseLoop（不拥有）：那个 EventLoop 活在 main() 的栈上，
    //   它的生死由用户决定；池子只保证"用它的时候它还在"。
    // 【易错】muduo 明确要求 baseLoop 生命周期长于 TcpServer（含线程池），
    //   否则 numThreads_==0 时 loops_ 为空、getNextLoop 会返回悬垂的 baseLoop_。
    EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg);
    // 【流程】⑫ 的隐藏环节：析构里没有任何显式停止代码，
    //   靠 threads_（unique_ptr 数组）逐个析构 → ~EventLoopThread → quit + join。
    // 【⚠问题】没有 stop()/quit() 之类的优雅停止接口：想停只能销毁整个池子；
    //     //   本仓库连 TcpServer 都没有 stop()（git grep "void stop" HEAD 无结果），是有意的简化。
    //   另外它是非 virtual 的普通析构，放进 vector 的 unique_ptr 里靠的是"精确类型析构"，
    //   没问题；但别把它当多态基类用。
    ~EventLoopThreadPool();

    // 【一句话】设置 subLoop 个数（= IO 线程数），0 或 1 之外的语义见 .cc。
    // 【线程】只在主线程、且必须在 start() 之前调用（start 之后改它毫无意义）。
    // 【易错】非 const、无同步、无范围校验：传负数则 start 的 for 一次都不进（等价单线程），
    //   传超大值则一窝蜂起线程（每线程默认 8MB 栈，靠 overcommit 撑着）。
    //   TcpServer::setThreadNum 里还有个同名局部变量遮蔽成员的小坑（见对应文件的注释）。
    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    // 【流程】③ 的主战场：循环 N 次"new EventLoopThread + startLoop() 收 loop 指针"。
    // 【设计】cb 用 const 引用传（而不是存成员）：初始化回调只对"本次 start 起的这批线程"有效，
    //   生命周期由调用方（TcpServer）保证，池子只是转发给每个 EventLoopThread。
    // 【易错】start() 是**阻塞**的：每起一个线程都要等它的 EventLoop 构造完才继续
    //   （cond_.wait），所以 start() 返回时线程池已经完全可用。
    void start(const ThreadInitCallback &cb = ThreadInitCallback());

    // 如果工作在多线程中，baseLoop_(mainLoop)会默认以轮询的方式分配Channel给subLoop
    // 【一句话】轮询选一个 subLoop 来接手新连接；没线程池就返回 baseLoop。
    // 【流程】⑥ TcpServer::newConnection 里唯一的一次调用点 ⇒ 只在 mainLoop 线程执行。
    // 【设计】"轮询"而不是"最少连接"或"按 fd hash"：无共享状态、O(1)、长期均匀；
    //   缺点是连接生命周期差异大时短期不均（长连接堆在同一个 loop 上）。
    //   【易错】正因为只被一个线程访问，next_ 才能是普通 int —— 见它的成员注释。
    EventLoop *getNextLoop();

    // 【一句话】返回"全部 loop"的清单；没建线程池时退化成只含 baseLoop 的单元素数组。
    // 【设计】返回**值**（拷贝 vector）而不是引用：调用方拿到的是快照，
    //   不会看到池子后续变化，也不会因池子析构而悬垂。
    // 【易错】本仓库里它没有任何调用点（git grep getAllLoops HEAD 只有声明与定义两处），
    //   留着是为了 API 完整性：想"对全部 loop 统一收尾"时，它就是那个遍历入口。
    std::vector<EventLoop *> getAllLoops(); // 获取所有的EventLoop

    // 【设计】started_ 只是给外面看看的"我 start 过了"旗标；
    //   注意它不是原子、也不是 start() 内部逻辑的一部分（start 里不会 if (started_) 防重入，
    //   所以调两次 start() 会把线程起两遍、loops_ 里塞重复项 ——【⚠问题】）。
    // 【语法】name() 按值返回 std::string：每次调用都要拷一个 string（多半带一次堆分配）。
    //   更省的写法是返回 const std::string&（零拷贝，代价是调用方要保证对象活得比引用久）。
    //   本仓库选按值返回：多一次堆分配换省心，属于可接受的取舍。
    bool started() const { return started_; } // 是否已经启动
    const std::string name() const { return name_; } // 获取名字

private:
    // 【设计】裸指针 + 不拥有：baseLoop_ 是用户（main）在栈上造的那个 EventLoop，
    //   池子对它只有"用它"的权利，没有任何"销毁它"的权利。
    //   这也是 getNextLoop() 在单线程模式下能直接把它发出去的原因。
    EventLoop *baseLoop_; // 用户使用muduo创建的loop 如果线程数为1 那直接使用用户创建的loop 否则创建多EventLoop
    // 【设计】线程名前缀：start() 里拼成 "名字+序号"（如 IO0/IO1）传给每个 EventLoopThread。
    // 【语法】函数 const std::string name() const 按值返回会拷贝；
    //   更省的写法是返回 const std::string&（零拷贝，代价是调用方要保证对象活得比引用久）。
    //   这里按值返回只在"偶尔打印"的场景无伤，但属于不必要的退化。
    //   另外 name_ 声明行右侧的原有中文注释说"线程池中 EventLoopThread 名称依赖线程池名称"，
    //   这是对的：start() 里拼好的 buf 会作为 name 传给 EventLoopThread，再传给 Thread，
    //   Thread 最后判空决定要不要退回默认的 Thread%d。
    std::string name_;//线程池名称，通常由用户指定，线程池中EventLoopThread名称依赖于线程池名称。
    // 【易错】普通 bool，无同步：只在 start() 里被同线程读写，靠"用前必 start"的约定保证。
    bool started_;//是否已经启动标志
    // 【设计】int 而不是 size_t：setThreadNum(int) 是用户视角的"几个线程"，
    //   并且 for (int i = 0; i < numThreads_; ++i) 这样写不会有符号比较警告。
    int numThreads_;//线程池中线程的数量
    // 【必考】为什么 next_ 是普通 int 而不是 atomic_int：
    //   getNextLoop() 的唯一调用点是 TcpServer::newConnection，而 newConnection 只在
    //   mainLoop（accept 的那个线程）里被触发 ⇒ 自增只发生在一个线程，
    //   构成"单线程所有权"，加原子纯属浪费（还给人"可以多线程调"的错误暗示）。
    // 【易错】如果哪天有人把新连接的分发挪到用户线程里直接调 getNextLoop()，
    //   ++next_ 立刻变成数据竞争（丢失更新 → 同一 loop 被连选两次，或越界读）。
    //   这就是"性能优化建立在契约上"的典型例子：契约破了不报错，只在压测时随机出问题。
    // 【设计】所以这条契约写进注释比写进代码更重要：本仓库没有任何机制阻止你在用户线程
    //   调 getNextLoop()，编译器也不会警告（上游在这类地方通常有"必须在 loop 线程"的
    //   断言，具体以原版源码为准）。
    int next_; // 新连接到来，所选择EventLoop的索引
    // 【设计】拥有语义：池子负责 N 个 EventLoopThread 对象的生死（析构 → quit + join）。
    //   vector<unique_ptr<T>> 是"多态/不可移动对象数组"的标准容器：
    //   EventLoopThread 里有 mutex/condition_variable/Thread，全都不可拷贝也不可移动，
    //   所以只能放指针，不能放值（push_back(EventLoopThread(...)) 编译不过）。
    // 【易错】new + push_back(unique_ptr(t)) 的写法在 push_back 触发扩容抛异常时
    //   会漏掉 t（概率极低，但确实是非异常安全写法）——本项目的 .cc 里有更细的说明。
    //   本项目为了看得清楚写成了两步，见 .cc 里的具体标注。
    std::vector<std::unique_ptr<EventLoopThread>> threads_;//IO线程的列表
    // 【设计】**不拥有**的裸指针清单：每个元素指向对应 EventLoopThread 的
    //   threadFunc 里那个**栈上** EventLoop。这就是
    //   "不要 delete loop, it's stack variable"的全部含义。
    // 【流程】⑥ getNextLoop() 从这里按 next_ 取一个 loop，把新连接交给它。
    // 【易错】两个数组必须严格等长、同序（start 里成对 push_back 保证），
    //   所以 loops_[i] 与 threads_[i] 一一对应；若有人在中间 erase 一个 thread，
    //   对应 loop 指针就成悬垂 —— 本类不提供任何删除接口正是为了杜绝这件事。
    std::vector<EventLoop *> loops_;//线程池中EventLoop的列表，指向的是EVentLoopThread线程函数创建的EventLoop对象。
};