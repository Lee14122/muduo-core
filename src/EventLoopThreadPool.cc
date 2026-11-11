/* ============================================================================
 * 【文件】src/EventLoopThreadPool.cc
 * 【一句话】按 numThreads_ 起 N 条子线程、逐个向它们讨要 EventLoop 指针存进 loops_，
 *          再用一个自增取模的下标把新连接轮流分给这些 loop。
 * 【Reactor 角色】线程池 / 负载均衡器：mainReactor 与 subReactor 之间的分发点（⑥）。
 * 【依赖】<memory> unique_ptr 接管新建的 EventLoopThread；
 *        "EventLoopThreadPool.h" 自己的声明；"EventLoopThread.h" —— 这里要 new 它，
 *        必须看到完整类型（头文件里只前向声明过，~EventLoopThreadPool 也因此只能放 .cc）；
 *        "Logger.h" 提供日志宏（本文件目前没打日志，include 属于历史遗留）。
 * 【谁用它】TcpServer::start 调 start()；TcpServer::newConnection 调 getNextLoop()；
 *          TcpServer 析构（threadPool_ 是 shared_ptr 成员）触发 ~EventLoopThreadPool。
 * 【先修知识】master/slave Reactor 分工；unique_ptr 进 vector 的移动手法；
 *          "取模轮询"与 hash/最少连接 的差异；threadFunc 栈对象的生命周期。
 * 【难度】★★☆☆☆（逻辑最浅，但"所有权"是这里最容易在面试里说错的地方）
 *          【建议耗时】25 分钟
 * 【自检】a) 为什么 loops_ 里存的指针不能 delete？它指向的内存由谁释放、在哪个线程？
 *        b) numThreads_==0 时 start() 还做了件什么事？为什么？
 *        c) getNextLoop 在单线程模式下返回什么？会不会越界访问空 loops_？
 * ============================================================================ */

#include <memory>

// 【易错】include 顺序上 EventLoopThreadPool.h 排在标准库之后但在自己的实现依赖之前；
//   这里少 include 了 "EventLoop.h"，因为本文件从未解引用 EventLoop*
//   （只存指针、只传指针）——这正是"前向声明 + 只用指针"带来的编译收益。
#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"
#include "Logger.h"
// 【流程】② TcpServer 构造里 new EventLoopThreadPool(loop, name_)：只登记，不起线程。
// 【设计】numThreads_ 默认 0（单线程模式），next_ 从 0 开始，started_ 关着：
//   构造完的池子是一个"完全惰性"的对象，即便用户从不 start()，析构也不会有任何副作用
//   （threads_ 空、loops_ 空）。
// 【语法】一个初始化列表塞五个成员、写成两行，是本项目自己的紧凑排版（不推荐模仿，
//   但读代码时要知道：成员初始化顺序仍然只由头文件声明顺序决定，
//   这里写的顺序恰好与声明顺序一致：baseLoop_ → name_ → started_ → numThreads_ → next_）。
// 【语法】name_(nameArg) 是一次 std::string 拷贝构造（形参本身是 const 引用）。
EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg)
    : baseLoop_(baseLoop), name_(nameArg), started_(false), numThreads_(0), next_(0)
{
}

/* 【流程】⑫：这是"关掉整个服务器"时 IO 线程的死亡路径——
 *   ~EventLoopThreadPool（本函数体什么都不做）
 *   → 成员按声明逆序析构：loops_（只是指针数组，析构无副作用）
 *   → threads_（vector<unique_ptr<EventLoopThread>>）逐个 delete
 *   → ~EventLoopThread：loop_->quit()（跨线程 wakeup）+ thread_.join()
 *   → 子线程 loop() 退出 → 它的栈上 EventLoop 析构 → 线程函数返回 → join 返回。
 * 【设计】函数体是空的，全部威力来自"对象图 + RAII 析构链"：
 *   这就是本文件标注的【⚠问题】"EventLoopThreadPool 没有优雅停止接口，
 *   靠 unique_ptr 析构链触发 join"的落点。想"停服务器但不停进程"是做不到的，
 *   必须销毁 TcpServer（或它里面的 shared_ptr<EventLoopThreadPool>）。
 * 【易错】N 条线程是**串行**收尾的：第一个 EventLoopThread 的 join 会等它自己退完才轮到
 *   下一个的 quit。整体退出时间 = 各线程退出时间之和的上界，
 *   而不是并发等待。极端情况下（某个回调卡住）整个析构会一直卡在那一条上。
 * 【设计】"空函数体的析构 + 成员析构链"是 C++ 里常见的收尾写法，但它把停止顺序完全
 *  交给了成员的声明顺序：loops_ 声明在 threads_ 之后（头文件里正是如此），所以它先
 *  析构，此时指针全都还有效；之后才轮到 threads_ 逐个 delete → quit → join。
 *  【易错】真要改这个类，先想清楚"谁先被销毁"再动声明顺序——这类问题不报错，只在退出时崩。
 */
EventLoopThreadPool::~EventLoopThreadPool()
{
    // 【必考】这句原注释是本项目所有权设计的题眼，完整展开：
    //   loops_ 里每个 EventLoop* 都指向"对应子线程 threadFunc 函数体内的**栈对象**"。
    //   它们的释放时机是子线程自己的 threadFunc 返回（离开作用域 → ~EventLoop），
    //   发生在**那个子线程**上。所以：
    //     - 不能 delete：栈内存不能被 delete（直接破坏主调函数的栈帧，通常当场崩）；
    //     - 也不能在别的线程析构：~EventLoop 里要 disableAll/remove 操作自己的 epoll、
    //       还要清 __thread t_loopInThisThread（TLS 写错线程 = 给别的线程解登记）。
    //   换句话说，"loop 由谁构造、由谁析构"必须是同一个线程，本注释就是这个约定的凭据。
    // Don't delete loop, it's stack variable
}

// 【流程】③ 的核心：TcpServer::start 第一件事就是它（在 Acceptor::listen 之前）。
//   顺序很关键：先把 IO 线程池备好，才能保证后面 newConnection 时 getNextLoop()
//   一定拿得到有效 loop（否则第一个连接到达时 loops_ 还是空的，会被分到 baseLoop）。
// 【线程】在 mainLoop（通常就是主线程）执行；但函数返回时，N 条子线程已经各自跑起了
//   自己的 loop()（startLoop 里的条件变量握手保证了这点）。
// 【易错】没有 if (started_) return; 之类的防重入 ⇒ 调两次 start() 会重复起线程、
//   loops_ 里出现两份指针（【⚠问题】）。当前只有 TcpServer::start 用 started_ 的
//   fetch_add 挡住了上层重入，池子自己不设防。
void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    // 【设计】先翻旗标再起线程：即使后面的 startLoop 阻塞，从外部看这个池子也算"启动了"。
    //   （本项目没人读 started_，所以更多是给人看的语义标记。）
    started_ = true;

    // 【设计】循环体里"起名 → 建壳 → 起线程并拿 loop"三步一气呵成，
    //   所以 threads_[i] 与 loops_[i] 严格一一对应（头文件里强调的同序等长就是这里保证的）。
    // 【易错】numThreads_ 是 int，i 也是 int：无符号比较警告没有，但传负数就等于不循环
    //   （退化成单线程模式），不会崩，也不会给任何提示。
    for (int i = 0; i < numThreads_; ++i)
    {
    // 【对比原版/⚠问题】char buf[name_.size() + 32] 是**变长数组（VLA）**，
    //   这是 GNU C99 扩展，C++ 标准从来没有 VLA！用 -pedantic 编译会看到
    //   "ISO C++ forbids variable length array"警告；换成 MSVC 直接编译失败。
    //   标准写法有三种：std::vector<char>、std::string 直接拼接、或固定大缓冲区 + snprintf。
    //   更麻烦的是它让编译期的边界检查失效：数组大小由运行时决定，
    //   静态分析与 -Wall 关于缓冲区的那套警告对它一概不起作用。
    //   这个写法看着像从上游继承下来的（能否对应原版以原版源码为准，本环境无法联网核对）。
    // 【易错】容量语义也别读错：size 是"名字长度 + 32"，多出的 32 字节用来放序号和结束符，
    //   所以不会溢出（snprintf 也会截断兜底）；但它和"名字 + 序号"的真实需要量无关，
    //   如果哪天有人把 +32 写成 +2 就会静默截断线程名。
    // 【栈开销】VLA 分配在栈上，几十字节，无所谓；真正的坏处是"非法语法 + 逃逸检查"。
        char buf[name_.size() + 32];
    // 【系统调用】snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i)：
    //   sizeof buf 在这里是**数组总字节数**（VLA 的 sizeof 是运行时算的，仍然正确），
    //   所以截断安全。拼出来的样子就是 "IO0"、"IO1"（nameArg 来自 TcpServer 的名字，
    //   所以日志里能看到 mainLoop 名 + 序号，便于把连接和线程对上）。
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
    // 【语法】先 new 再用 unique_ptr 接管：中间隔了一句 push_back 参数构造，
    //   如果那个 push_back 因为扩容抛 std::bad_alloc，t 就泄漏了（裸指针没人管）。
    //   【易错】异常安全的写法是 threads_.emplace_back(std::unique_ptr<EventLoopThread>(t))
    //   一步到位，或者用 make_unique 直接构造。本项目这样写是为了"看得见所有权转移"，
    //   读到 t 这个变量名要知道它中间有裸奔状态。
    // 【设计】为什么用堆上 EventLoopThread：它内含 mutex/condition_variable/Thread，
    //   既不可拷贝也不可移动，vector 装不了值，只能装它的 unique_ptr。
        EventLoopThread *t = new EventLoopThread(cb, buf);
    // 【设计】转移所有权的一刻：从此池子对这条线程的生命周期负责，
    //   池子析构时它才被 delete（→ quit + join，见上面析构函数的注释）。
        threads_.push_back(std::unique_ptr<EventLoopThread>(t));
    // 【流程】③ 里最关键的一行：t->startLoop() 会**阻塞**直到子线程把
    //   它自己的 EventLoop 构造好并通过条件变量交出来（见 EventLoopThread.cc 的握手时序图）。
    // 【设计】存的只是地址：这个 EventLoop 活在子线程的栈帧里，
    //   loops_ 只是一张"通讯录"（所以是裸指针 + 不拥有 + 永不 delete）。
    // 【易错】这里存在一个"启动顺序的隐性依赖"：startLoop 返回时子线程的 loop
    //   还没进 loop()（它在 callback_ 之后才调 loop.loop()），所以主线程后续
    //   queueInLoop 到这个 loop 是安全的——任务只是被放进队列 + wakeup，
    //   等子线程真跑起来时自然会消费掉。这正是 eventfd 方案比"同步调用"优越的地方：
    //   投任务不需要对方已经在跑。
        loops_.push_back(t->startLoop()); // 底层创建线程 绑定一个新的EventLoop 并返回该loop的地址
    }

    // 【必考】numThreads_ == 0（单 Reactor 单线程模式）时的补偿动作：
    //   依然要把线程初始化回调 cb 跑一次，只不过跑在**主线程**、传的是 baseLoop_。
    // 【设计】为什么必须跑：上层（TcpServer::setThreadInitCallback）对 cb 的契约是
    //   "每个 loop 线程启动时恰好执行一次"，回调里常常注册 per-thread 的定时器 fd、
    //   绑定 UDP socket、初始化 thread-local 资源。如果 numThreads_==0 就跳过，
    //   那么单线程模式下这些初始化永远不做，行为会随线程数变化而不同 ——
    //   这是最容易被漏掉的"配置相关的语义漂移"。
    // 【易错】&& cb 判的是 std::function 是否为空（operator bool）：
    //   用户没设回调时不能调，空调用会抛 std::bad_function_call。
    // 【设计】把这条规则记成一句话："线程初始化回调的执行次数 = loop 的个数"。
    //   多线程时是 N 次（各在自己的子线程里），单线程时是 1 次（在主线程里），
    //   于是上层代码完全不必为 numThreads_==0 写特判——这是本函数最容易看漏的一致性契约。
    if (numThreads_ == 0 && cb) // 整个服务端只有一个线程运行baseLoop
    {
        cb(baseLoop_);
    }
}

// 如果工作在多线程中，baseLoop_(mainLoop)会默认以轮询的方式分配Channel给subLoop
/* 【流程】⑥ 的第一动作：TcpServer::newConnection（mainLoop）里拿它挑一个 subLoop，
 *   然后 ioLoop->runInLoop(connectEstablished) 完成第一次【线程跳转 mainLoop → subLoop】。
 * 【线程】只可能在 mainLoop 执行（因为 newConnection 只被 mainLoop 上的 Acceptor 回调触发）。
 *   这句是 next_ 敢用普通 int 的全部依据。
 * 【算法】round-robin：先取 loops_[next_]，再 ++next_，越界则归零。
 *   注意"先取后加" ⇒ 第一个连接拿到 loops_[0]，第 N+1 个连接又回到 loops_[0]。
 * 【设计】为什么用轮询而不是别的：
 * - 最少连接：短期更均衡，但要维护"每个 loop 当前有几个连接"，而这个数天然被别的
 *   线程改 ⇒ 又得加锁或上原子，换来的收益在连接时长均匀时几乎为零；
 *   - fd hash 取模：同客户端可能集中，且线程数一变所有映射都变；
 *   - 轮询：O(1)、零共享状态、连接数长期绝对均匀（短连接场景最优），
 *     代价是"长连接堆叠"时可能不均 —— 这也是高并发面试的标准讨论点。
 * 【易错】loops_ 为空时（单线程模式）不进 if，直接返回 baseLoop_ ⇒ 所有连接都由主线程
 *   自己处理，程序依然正确，只是没有并行度。
 * 【⚠问题】if (next_ >= loops_.size()) 是 int 与 size_t 比较（-Wsign-compare 会警告）：
 *   这里因为 next_ 恒 >= 0 所以行为正确（int 被提升为无符号）；
 *   但如果 next_ 哪天被改成负数，就会被当成巨大无符号数、条件立刻成立、归零 ——
 *   看起来"也对"，其实是巧合。稳妥写法是 next_ %= (int)loops_.size() 或 size_t 类型。
 */
EventLoop *EventLoopThreadPool::getNextLoop()
{
    // 如果只设置一个线程 也就是只有一个mainReactor 无subReactor 
    // 那么轮询只有一个线程 getNextLoop()每次都返回当前的baseLoop_
    EventLoop *loop = baseLoop_;    

    // 通过轮询获取下一个处理事件的loop
    // 如果没设置多线程数量，则不会进去，相当于直接返回baseLoop
    if(!loops_.empty())             
    {
        // 【语法】loops_[next_] 不做越界检查（operator[] 不检查，at() 才检查）：
        //   安全性靠上面"先归零再返回"的轮询逻辑保证 —— 一旦 start() 之后有人
        //   再改 numThreads_ 或清空 loops_，这里就是越界读（不报错，直接跑飞）。
        // 【易错】注意 next_ 只在 loops_ 非空的分支里递增：单线程模式下永远保持 0，
        //   之后如果有人在运行期塞一个 loop 进来，也是从 0 开始轮询（无害）。
        loop = loops_[next_];
        ++next_;
        // 轮询
        if(next_ >= loops_.size())
        {
            next_ = 0;
        }
    }

    return loop;
}



// 【一句话】给外部一份"所有 loop"的快照；没建线程池时返回只含 baseLoop 的一元数组。
// 【设计】返回 {baseLoop_} 的用意：让调用方**永远**可以用同一套循环处理"所有 loop"，
//   不必再写 if (loops_.empty()) 的特判。
//   典型用法是"给每一个 loop 都投递同一个收尾任务"（关闭、刷新、通知）——
//   若这里返回空数组，单线程模式下就一个 loop 都收不到那个任务。
// 【易错】本仓库目前没人调它（git grep getAllLoops HEAD 只有声明与定义），
// 【语法】std::vector<EventLoop *>(1, baseLoop_) 是"长度 1、值填充"的构造函数写法，
//   比 push_back 少一次可能的扩容；C++11 之后写 {baseLoop_} 更直白。
// 【设计】按值返回 = 拷贝一份指针数组：调用方拿到的是快照，之后池子怎么变都不影响它，
//   但**指针本身仍然可能悬垂**（线程退出后 loop 就没了）—— 快照 ≠ 所有权。
std::vector<EventLoop *> EventLoopThreadPool::getAllLoops()
{
    if (loops_.empty())
    {
        return std::vector<EventLoop *>(1, baseLoop_);
    }
    else
    {
        return loops_;
    }
}