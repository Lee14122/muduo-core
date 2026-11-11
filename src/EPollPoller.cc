/* ============================================================================
 * 【文件】src/EPollPoller.cc
 * 【一句话】用 epoll 实现 Poller 的三个接口：epoll_create1 建实例、epoll_ctl 改内核登记表、
 *          epoll_wait 每轮取回就绪事件并把它们翻译成 Channel 列表；同时在这里定义 kNew/kAdded/kDeleted。
 * 【Reactor 角色】Demultiplexer（多路解复用器）的唯一具体实现 —— 全库唯一直接调 epoll 的文件。
 * 【依赖】<errno.h>（EINTR、失败时读 errno）、<unistd.h>（::close 关掉 epollfd_）、<string.h>（::memset
 *        清零 epoll_event）、"EPollPoller.h"（本类）、"Logger.h"（LOG_INFO/DEBUG/ERROR/FATAL）、
 *        "Channel.h"（要真的解引用 channel：fd()/events()/index()/set_index()/set_revents()/isNoneEvent()）。
 * 【谁用它】EventLoop：①/③ 构造时经 Poller::newDefaultPoller 造出来，④ 每轮 loop 调 poll()，
 *        ③⑤⑦⑨⑩⑪ 任何开关事件都经 EventLoop::updateChannel 落到本文件的 updateChannel()。
 * 【先修知识】1) epoll 的三张表：interest list（红黑树）/ ready list（双链表）/ 用户数组
 *          2) 水平触发 LT（本项目唯一模式）3) 联合体 union 4) 状态机与「内核态 vs 用户态」一致性
 * 【难度】★★★★★   【建议耗时】50 分钟（全库最值得精读的文件之一）
 * 【自检】① epoll_wait 返回 -1 且 errno==EINTR 为什么必须当成「什么都没发生」？
 *        ② index_==kDeleted 时 channels_ 里还有没有这个 fd？为什么这个前提不能忘？
 *        ③ 为什么 update 里用 data.ptr 而不是 data.fd？用 data.fd 会发生什么？
 * 【口径】本文件凡【对比原版】条目里涉及原版 muduo 具体写法的话，本环境无网络、无原版源码，
 *        一律「以原版仓库为准」，不当作已核实事实；凡写「可核实：…」的都是能在本仓库
 *        直接 grep / 读码验证的事实。本文件的隐患与要点已全部就地打了标签，用 grep 【⚠问题】/【易错】/【对比原版】 即可索引。
 * =========================================================================== */

#include <errno.h>
// 【语法】errno 不是普通全局变量：它是宏（展开为 *__errno_location()），每个线程一份。
//        这也是本文件里 poll() 先 int saveErrno = errno; 再判断的原因 —— 中间只要调用任何库函数
//        （包括打日志），errno 都可能被改写，直接读会看到「假错误码」。【易错】高频坑。
#include <unistd.h>
// 【系统调用】为 ::close(epollfd_) 而引入（析构里）。注意 close 的是「epoll 实例的 fd」，不是被监听的 connfd。
#include <string.h>
// 【系统调用】为 ::memset 而引入，用来在每次 epoll_ctl 前把局部 epoll_event 清零。

#include "EPollPoller.h"
#include "Logger.h"
#include "Channel.h"

// ==========================================================================================
// 【设计】三个状态常量的「家」在本文件，不在任何头文件 —— 这是一次刻意的取舍：
//        index_ 的语义是「本 Poller 实现怎么看这个 channel」，属于 epoll 实现的私事；
//        放进 Channel.h 就等于告诉全库「Channel 有 epoll 状态」，换 poll/devpoll 实现时这些值可能
//        完全不成立（poll 实现用 pollfd_.events 判断，压根不需要 index_）。
//        代价就是【跨文件共享靠约定】：
//          · Channel.h 里 index_ 只能声明成 int（不能用枚举，否则又要暴露常量）
//          · Channel.cc 的构造里只能硬编码 -1（就是 kNew 的值）
//          · 本文件里用 -1/1/2 三个字面量含义必须与 Channel.cc 一致，编译器无法检查
// 【语法】这三个是【文件级 const】，在 C++ 里 namespace 作用域的 const 默认内部链接（internal linkage），
//        等价于加了 static —— 别的 .cc 即使 extern int kNew; 也链接不到这份值。
//        所以「跨文件共享」的准确说法是：只有本文件能用这三个名字，其它文件靠「值相等」的约定共享语义。
// 【设计】为什么 kNew 选 -1、kAdded 选 1、kDeleted 选 2（而不是 0/1/2）：
//        留个非 0 的「非法值 -1」当初始态，任何「忘了赋值」的路径都会落到 -1，
//        而 -1 不等于任何一个合法状态，容易被发现；0 被 kNoneEvent 占了，用作状态容易混淆。
const int kNew = -1;    // 某个channel还没添加至Poller          // channel的成员index_初始化为-1
// 【系统调用】对应「内核 interest list 里没有这个 fd，且 channels_ 这张 map 里也没有它」。
//        Channel 出生就是这个状态（见 Channel.cc 构造里 index_(-1)）。
const int kAdded = 1;   // 某个channel已经添加至Poller
// 【系统调用】对应「已经 EPOLL_CTL_ADD 过，且 channels_ 里有它」。此时 events 变化只需 EPOLL_CTL_MOD。
const int kDeleted = 2; // 某个channel已经从Poller删除
// 【系统调用】对应「已经 EPOLL_CTL_DEL，内核 interest list 里没有了，但 channels_ 里【还留着】这个 fd」。
//        这个不对称是本文件最隐蔽的前提，见 updateChannel 里那个空的 else 分支的【易错】。

// 【流程】① 主线程构造 EventLoop → poller_(Poller::newDefaultPoller(this)) → 这里；
//        ③ EventLoopThreadPool 起的每个 subLoop 线程构造自己的 EventLoop → 各自这里一次。
// 【线程】EventLoop 所在线程（mainLoop 在 main 线程；subLoop#N 在对应 IO 线程）。
// 【设计】每个 EventLoop 独占一个 epollfd_：这就是 muduo「一个 loop 一个 epoll 实例」的落地，
//        各实例的 interest list 互不相干，所以同一个 fd 绝不能注册进两个 epoll 实例
//        （muduo 靠 one loop per thread + Channel 归属唯一 loop 来保证）。
EPollPoller::EPollPoller(EventLoop *loop)
    : Poller(loop)
    , epollfd_(::epoll_create1(EPOLL_CLOEXEC)) 
    // 【系统调用】int epoll_create1(int flags)：
    //   · 作用：创建一个 epoll 实例，内核分配 struct eventpoll（内含 interest 红黑树 + ready 双链表 + 等待队列），
    //     并把它挂进当前进程的 fd 表，返回该 fd。
    //   · 入参 flags：EPOLL_CLOEXEC（八进制 02000000）= 本 fd 在 exec 时自动关闭。
    //     为什么重要：否则 fork+exec 出去的子进程会继承这个 epollfd，里面挂着服务器全部连接，
    //     既泄 fd 又让父进程 close 后连接还被子进程引用着（半关闭语义被破坏）。
    //     flags 只允许 EPOLL_CLOEXEC，写别的（比如 EPOLL_NONBLOCK，不存在）返回 -1 且 errno=EINVAL。
    //   · 返回：成功 = 新 fd（>=0）；失败 = -1，errno 可能 ENFILE（系统级 fd 耗尽）、
    //     EMFILE（进程级 RLIMIT_NOFILE 耗尽）、ENOMEM（内存不足）、EINVAL（非法 flags）。
    //   · 老接口 epoll_create(int size) 已废弃：size 从 Linux 2.6.8 起被忽略但仍要求 >0，
    //     否则 EINVAL —— 用 epoll_create1 更干净。
    //   · 是否阻塞：不阻塞，纯内核对象创建。
    // 【语法】:: 前缀表示「全局命名空间的 epoll_create1」，防止将来本类加了同名成员函数时误调自己。
    // 【设计】放在初始化列表里而不是函数体里：epollfd_ 是 int（不是指针），没有「先默认构造再赋值」
    //        的意义，直接初始化最直接，也让「构造完就一定可用」这条不变式成立。
    , events_(kInitEventListSize) // vector<epoll_event>(16)
    // 【语法】vector 的「count 构造」：直接开 16 个元素（值初始化，epoll_event 内容全 0），
    //        不是 push_back 十六次；size()==capacity()==16。
    // 【设计】为什么 16：epoll_wait 的 maxevents 就是这个数组的长度。16*12B=192B，白送；
    //        而一轮里同时就绪的 fd 通常远小于 16，先用小数组省内存，不够再翻倍（见 poll 里的扩容）。
    //        【易错】16 不是「最多只能监听 16 个连接」—— 能监听的 fd 数只看 RLIMIT_NOFILE 与内核内存，
    //        maxevents 只限制「一次 epoll_wait 最多拷贝多少个回来」。
{
    if (epollfd_ < 0)
    // 【易错】先判失败再用：epollfd_ 可能是 -1，如果不管它，后面 epoll_ctl/epoll_wait 全部 EBADF，
    //        报错点离病因十万八千里。这里当场 FATAL 是对的。
    {
        LOG_FATAL("epoll_create error:%d \n", errno);
        // 【⚠问题】LOG_FATAL 内部是 exit(-1)：不栈展开、不走任何析构、不刷新 std::cout 缓冲，
        //        只在「服务器起不来就别起来」这种场景可接受；通行描述：原版 muduo 用 abort + backtrace 留现场，
        //        本环境无原版源码可查，以原版仓库为准。
        //        （详见 include/Logger.h 的 LOG_FATAL 上方的同条【⚠问题】。）
    }
}

// 【流程】⑫ 退出：~EventLoop → poller_ 这个 unique_ptr 析构 → 基类虚析构派发到这里。
// 【线程】EventLoop 所在线程（谁构造谁销毁）。
EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
    // 【系统调用】close(epollfd) 的语义：销毁这个 epoll 实例，内核把它 interest list 里对各个 fd 的
    //        引用全部释放（被监听的 connfd 因此少一个引用，但 connfd 本身不由这里关闭 —— 那是 Socket 的事）。
    // 【⚠问题】没有检查 close 的返回值；EBADF/EINTR 在这里都无从察觉。教学上无害，生产建议记日志。
    // 【易错】析构时不管 channels_ 里还剩几个 channel：如果某个 Channel 对象还活着而 Poller 先没了，
    //        它对 enableReading 的调用会落到一个 epollfd_ 已失效的实例上（EPollPoller 已析构就是野指针）。
    //        muduo 靠「~EventLoop 之前必须已经把连接全关掉」的析构顺序保证正确性（⑪→⑫ 的先后）。
}

// ==========================================================================================
// 【流程】④：EventLoop::loop() 的 while 循环体第一句就是它，也是【整个线程绝大部分时间待的地方】。
//        返回后紧接着 EventLoop 遍历 activeChannels_ 调 Channel::handleEvent（⑤⑦⑧⑩⑪ 都从这里出发）。
// 【线程】ownerLoop 线程：mainLoop 调它时等的是 listenfd/wakeupfd 等主循环事件；
//        subLoop#N 调它时等的是分给该线程的那些 connfd + 它自己的 wakeupFd。
// 【设计】接口约定：把本轮就绪的 Channel* 塞进调用方给的 activeChannels（调用方已 clear 过），
//        并返回一个统一的时间戳。返回类型是 Timestamp 而不是 bool/int：因为上层要 receiveTime。
// 【系统调用】int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)：
//   · 入参 epfd：epoll_create1 拿到的实例 fd。
//   · 入参 events：用户态数组首地址，内核把就绪事件【拷贝】到这里（不是让内核长期持有这块内存）。
//   · 入参 maxevents：本次数组上限，必须 > 0；上限由内核在 2.6.8 后不再受 EPOLL_MAX_EVENTS 之外的限制。
//   · 入参 timeout（毫秒）：>0 最多等这么久；0 = 立即返回（非阻塞探测）；<0（本项目用不到，-1）= 无限等。
//   · 返回：>0 = 就绪事件个数（可能小于 maxevents）；0 = 超时；-1 = 出错（errno 有效）。
//   · 阻塞行为：无事件时线程进入 epoll 等待队列睡眠（不占 CPU，不是轮询）；
//     被信号打断 → -1 + EINTR；epollfd 被关 → -1 + EBADF；events 不在可写地址 → -1 + EFAULT。
//   · 内核里发生了什么：设备就绪 → 驱动调用 ep_poll_callback → 把该 fd 的 epi 挂进 ready list →
//     唤醒等待队列上的线程 → 把 ready list 前 maxevents 个拷贝到用户数组 → 从 ready list 摘掉（LT 模式下
//     如果 fd 仍然就绪，它会被重新挂回 ready list，所以下一轮还会再来 —— 这就是「水平触发不丢事件」的原理）。
// ==========================================================================================
// 【流程】④：本函数就是「④ 阻塞等待并取回就绪 channel」这一步，返回后进入 ⑤/⑦/⑧/⑩/⑪ 的分发。
// 【线程】ownerLoop 线程：mainLoop 或某个 subLoop#N（谁调 poll，谁就睡在这一行里）。
Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    // 由于频繁调用poll 实际上应该用LOG_DEBUG输出日志更为合理 当遇到并发场景 关闭DEBUG日志提升效率
    LOG_INFO("func=%s => fd total count:%lu\n", __FUNCTION__, channels_.size());
    // 【⚠问题】值得注意：这行是每轮 poll 都打的 INFO 级日志。EventLoop::loop() 每被唤醒一次
    //        （④⑤⑦⑧⑨⑩⑪ 任一事件、或 kPollTimeMs=10s 超时）就打一条，高并发下就是「每包一条日志」：
    //        snprintf 格式化 + 写 stdout 的开销直接压在最热的路径上，且日志量爆炸。
    //        应该用 LOG_DEBUG（本项目未定义 MUDEBUG 时 LOG_DEBUG 是空宏，等于编译期消失，零开销）。
    // 【对比原版】可核实：本仓库的 include/Logger.h 只有 LOG_INFO / LOG_ERROR / LOG_FATAL / LOG_DEBUG
    //        四个宏，【没有】 TRACE 级别，而 LOG_DEBUG 在不定义 MUDEBUG 时是空宏。
    //        通行描述：原版这类热路径日志用 LOG_TRACE（编译期可压掉），本环境无法核实原版，以原版仓库为准。
    // 【易错】channels_.size() 返回 size_t，用 %lu 打印在本平台正确；但它是「注册进本 Poller 的 channel 数」，
    //        不等于「内核里就绪的 fd 数」，也包含处于 kDeleted 状态（已 DEL 但还没 erase）的项 —— 别看名字就以为是活跃连接数。

    int numEvents = ::epoll_wait(epollfd_, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    // 【系统调用】逐参数拆：
    //   1) epollfd_               —— 本实例的 fd。
    //   2) &*events_.begin()      —— 就绪事件的落地地址。等价于 events_.data()（C++11 起才有 data()）。
    //      为什么用「取迭代器再解引用」这种别扭写法：vector 保证元素连续存储，&*begin() 拿到首元素地址；
    //      【易错】空 vector 时 begin()==end()，&*begin() 是解引用尾迭代器 = UB。本类永不为空
    //      （构造时 16，只 resize 翻倍、从不 clear/shrink），所以这条 UB 路径走不到 —— 依赖了别的函数的不变式。
    //   3) static_cast<int>(events_.size())  —— maxevents：size() 是 size_t（无符号 64 位），
    //      内核参数是 int，不转会有 narrowing 警告；有符号/无符号比较也会踩 -Wsign-compare。
    //      【易错】若将来 events_.size() 超过 INT_MAX，这个强转就溢出（现实里不可能，但要知道它是裸转）。
    //   4) timeoutMs              —— 毫秒。EventLoop 传 10000（kPollTimeMs），-1=无限等，0=立即返回。
    // 【系统调用】返回值三态与本项目分支一一对应：>0 就绪事件数；==0 超时；-1 出错（看 errno）。
    // 【设计】这里是「主动让出 CPU」的唯一位置：mainLoop/subLoop#N 一天到晚就睡在这一行里，
    //        有事件（④→⑤/⑦/⑧/⑩/⑪）、被 wakeup()（eventfd 写 8 字节）、或超时才醒。
    // 【易错】poll 只负责「搬运就绪事件」，不负责「处理」：真正的回调在 EventLoop 那边的
    //        Channel::handleEvent 里执行。把 poll 写成阻塞在业务回调里，是初学者的常见误解。
    int saveErrno = errno;
    // 【设计】紧跟系统调用存 errno，是 POSIX 编程的硬规矩：errno 是宏（线程局部），
    //        任何后续库函数（包括下面 LOG_* 里的 printf/malloc/write）都可能改写它。
    //        【易错】注意 epoll_wait 成功返回 0 或 >0 时并【不会】清 errno，
    //        所以「errno 里是上一次的脏值」是常态 —— 只有返回 -1 时它才有意义。
    //        本函数后面在 else 分支才用 saveErrno，逻辑正确。
    Timestamp now(Timestamp::now());
    // 【流程】这个 now 就是 ④ poll 的返回值，会被 EventLoop 存进 pollRetureTime_，
    //        再作为 receiveTime 一路传给 ⑧ 的 readCallback_（用户 onMessage 的第三个参数）。
    // 【设计】为什么在 epoll_wait【之后】取时间：这是「事件被用户态看到的时刻」，
    //        比「内核把它放进 ready list」晚一点，比「用户回调真正执行」早一点，是两者之间最接近的值；
    //        一轮 poll 里所有活跃 channel 共用同一个时间戳（不是每个 fd 各取一次时间）——
    //        这是刻意的性能取舍：省掉 N 次 clock_gettime，代价是同一批内时间相同。
    // 【⚠问题】本项目的 Timestamp::now() 实现是 Timestamp(time(NULL))（A1 的【⚠问题】），
    //        把「秒」塞进了「微秒」字段，所以这里传出去的 receiveTime 精度只到秒。

    if (numEvents > 0)
    // 【设计】三分支：有事件 / 超时 / 出错。顺序按「最常见→最少见」排，利于分支预测。
    {
        LOG_INFO("%d events happend\n", numEvents); // LOG_DEBUG最合理
        // 【⚠问题】又一处每轮 INFO（原注释自己也写了「LOG_DEBUG最合理」）：热路径日志。
        //        它和上面 fd total count 那行是两条，等于每轮至少两条日志 + 两次格式化。
        //        应改成 LOG_DEBUG（未定义 MUDEBUG 时是空宏，零开销）或 LOG_TRACE。
        fillActiveChannels(numEvents, activeChannels);
        // 【流程】把内核结果翻译成 Channel 列表（见本文件后半部分的函数注释）。
        // 【设计】先填表、再判断扩容：扩容只影响【下一轮】的 maxevents，本轮已经拷完了。
        if (numEvents == events_.size()) // 扩容操作
        // 【设计】「取满即翻倍」的理由：
        //        epoll_wait 一次最多取 maxevents 个；如果本轮把数组填满了，说明 ready list 里
        //        很可能还有没取走的事件（下一轮还得再来一次 syscall），而且系统正处于「事件洪峰」，
        //        把缓冲区翻倍可以：① 一次 syscall 处理更多就绪 fd，摊薄每次 syscall 的成本；
        //        ② 用几何增长把 resize 次数压到 O(log N)，均摊后每次 poll 的扩容成本近似常数。
        //        为什么初值是 16 而不是「一开始就开很大」：绝大多数时刻只有极少数 fd 同时就绪，
        //        小初值省内存，突发时再自动长起来 —— 这就是「按需几何扩容」的标准做法。
        // 【易错】比较的是 size()（元素个数）而不是 capacity()：resize 之后 size 也变，
        //        所以容量只会单调上升，永不下调（本文件没有任何 shrink 逻辑）。
        //        另一点：numEvents 是 int、events_.size() 是 size_t，这是有符号/无符号比较，
        //        编译器会给 -Wsign-compare 警告；因为 numEvents>=0 且 size 很小，实际不会出错。
        {
            events_.resize(events_.size() * 2);
            // 【设计】resize 会重新分配并【拷贝旧内容】（vector 不能原地长），所以只可能在「取满」时
            //        发生，摊薄到几十次 poll 之后；这是本函数里唯一的潜在堆操作。
            // 【易错】resize 之后 &*events_.begin() 变了地址 —— 绝不能跨 poll 缓存这个指针。
            // 【易错】可核实：本文件只有这一处 resize，没有任何缩容（shrink）逻辑，也没有任何地方用
            //        EPOLLET / EPOLLONESHOT —— 一个长期运行的服务器若某次突发把容量撑到 4096，
            //        之后即使只剩 10 个连接，这块 48KB 的数组也会一直占着（每 loop 一份）。
            //        本教学项目止步于翻倍扩容。
        }
    }
    else if (numEvents == 0)
    {
        LOG_DEBUG("%s timeout!\n", __FUNCTION__);
        // 【⚠问题】本项目 LOG_DEBUG 在未定义 MUDEBUG 时是【空宏】（见 Logger.h：#define LOG_DEBUG(...) 空展开）：
        //        ① 这行「超时」在 release 下等于不存在，调试 10 秒一次的空转时看不到任何东西；
        //        ② 但参数（__FUNCTION__）也一起消失，所以没有运行时开销 —— 这是「编译期裁剪」而非「运行期过滤」，
        //        与 Logger::logLevel_ 只改前缀不做过滤（A1 的【⚠问题】）是两回事。
        // 【流程】超时不是错误：醒着只是为了让 loop() 重新检查 quit_（⑫ 退出）与跑 doPendingFunctors。
        //        若 ③ 之后某个线程调了 quit() 但没有 wakeup（例如恰好不在 queueInLoop 路径上），
        //        最坏要等满 kPollTimeMs=10000 毫秒才退出 —— 这是本项目的隐性延迟（A4 的【⚠问题】）。
    }
    else
    {
        if (saveErrno != EINTR)
        // 【设计】EINTR 是【必须忽略】的：进程一旦收到信号（SIGCHLD、SIGWINCH、自定义 handler、
        //        甚至调试器 Ctrl-C），epoll_wait 会以 -1 + EINTR 返回，这不是错误而是「等待被打断」。
        //        处理方式就是「什么都不做，下一轮 while 再 poll 一次」，事件一个都不会丢：
        //        水平触发下没被取走的就绪事件仍在 ready list 里（内核不会因一次打断就清空）。
        //        【易错】老代码常写 while (n < 0 && errno == EINTR) continue; 在本项目不需要，
        //        因为外层 EventLoop::loop() 的 while 就是那个重试循环。
        //        如果这里不判 EINTR 直接 LOG_ERROR，服务只要收到一次信号就会刷一屏假错误。
        {
            errno = saveErrno;
            // 【设计】把之前存好的 errno 再写回去：让【调用方】（EventLoop::loop 及其后面的代码）
            //        看到的 errno 仍是 epoll_wait 失败时的真实原因，而不是被中间 LOG_* 调用污染过的值。
            //        这是 POSIX 网络库里的通行写法（保存-恢复 errno）。
            LOG_ERROR("EPollPoller::poll() error!");
            // 【设计】真正失败（EBADF/EFAULT/EINVAL/ENOMEM）只打 ERROR 不 FATAL：
            //        因为 EBADF 一类错误在多 fd 关闭的收尾阶段可能偶发，直接让进程死掉太狠；
            //        【⚠问题】但这也意味着 poll 若持续失败（比如 epollfd_ 意外被关掉），
            //        EventLoop::loop() 会以「每轮立刻返回 + 打一条日志」的方式疯狂空转，CPU 100%。
            //        可核实：本项目对此没有任何补救（没有退避、没有把 poll 失败上报给 loop），值得警惕。
        }
    }
    return now;
}

// ==========================================================================================
// 【流程】下面是本文件最该精读的两台状态机（updateChannel / removeChannel）。
//        先把「三层转发链」钉在脑子里：
//          Channel::enableReading()/disableAll()  ──update()──▶  Channel::update()
//            └─▶ EventLoop::updateChannel(channel)        └─▶ EventLoop::removeChannel(channel)
//                  └─▶ Poller::updateChannel（虚函数）      └─▶ Poller::removeChannel（虚函数）
//                        └─▶ EPollPoller::updateChannel ◀── 本函数 ──▶ EPollPoller::removeChannel
//                              └─▶ EPollPoller::operation ──▶ ::epoll_ctl（真正下内核）
//        为什么 channel 不自己调 epoll_ctl？① Channel 不该有 epollfd_（那是 EventLoop/Poller 的资源）；
//        ② 要经 EventLoop 才能保证「在 ownerLoop 线程里改状态」；③ 保持 Poller 可替换
//        （换成 poll 实现时 Channel 一行都不用改）。
// ==========================================================================================
// channel update remove => EventLoop updateChannel removeChannel => Poller updateChannel removeChannel
void EPollPoller::updateChannel(Channel *channel)
// 【线程】ownerLoop 线程（mainLoop 处理 listenfd/wakeupfd 时；subLoop#N 处理 connfd 时）。
// 【设计】核心思想：Channel 只管改自己的 events_ 位图，「该用 ADD/MOD/DEL 哪个操作」由这里的
//        状态机决定。为什么需要状态机？因为 epoll_ctl 对三种 op 的【前置条件】不同：
//          ADD 一个已存在的 fd  → 返回 -1 + EEXIST
//          MOD 一个不存在的 fd  → 返回 -1 + ENOENT
//          DEL 一个不存在的 fd  → 返回 -1 + ENOENT
//        所以必须记住「这个 fd 现在到底在不在内核 interest list 里」，这就是 index_ 的全部职责。

// ┌─────────────── index_ 状态迁移表（本项目真实行为，逐条可对着代码验） ────────────────┐
// │ 当前 index  │ 条件                 │ 内核操作              │ channels_ 操作  │ 新 index  │
// ├─────────────┼──────────────────────┼───────────────────────┼─────────────────┼───────────┤
// │ kNew  (-1)  │ 任何 update()        │ EPOLL_CTL_ADD         │ 插入 fd→channel │ kAdded(1) │
// │ kAdded(1)   │ isNoneEvent()==true  │ EPOLL_CTL_DEL         │ 【不动，仍保留】│ kDeleted  │
// │ kAdded(1)   │ 仍有感兴趣事件       │ EPOLL_CTL_MOD         │ 不动            │ kAdded(1) │
// │ kDeleted(2) │ 任何 update()        │ EPOLL_CTL_ADD         │ 【不插入】      │ kAdded(1) │
// └─────────────┴──────────────────────┴───────────────────────┴─────────────────┴───────────┘
//   （removeChannel 另有一条：erase(fd) + 若 kAdded 则 EPOLL_CTL_DEL，最后 index_ 置回 kNew。）
// 【易错】表里两处【不动/不插入】是同一件事的两面，见下面那个空的 else 分支。
{
    const int index = channel->index();
    // 【设计】先把状态读进局部变量：① 下面要用两次（判分支 + 打日志），避免重复调用；
    //        ② 更重要的是【快照语义】：set_index 会在本函数中途改变 channel 里的值，
    //        如果每处都重新读，逻辑会自相矛盾（这是状态机代码的经典坑）。
    // 【⚠问题】const 局部 + 非 const 的 channel->index()：因为 Channel::index() 没标 const，
    //        在 const Channel& 上根本调不了（本项目没踩到，但值得注意）。
    LOG_INFO("func=%s => fd=%d events=%d index=%d\n", __FUNCTION__, channel->fd(), channel->events(), index);
    // 【⚠问题】又一次每事件 INFO 日志（热路径）：⑦⑨⑩⑪ 每个开关动作都会打一条，
    //        高频短连接场景下日志量正比于连接数。应为 LOG_DEBUG/LOG_TRACE。
    //        四个参数正好是状态机的「输入快照」，调试时非常好用 —— 留着的价值大于代价，但级别必须降。

    // 【设计】两个状态合成一个分支，是因为它们对内核的动作【完全相同】：都要 EPOLL_CTL_ADD。
    //        区别只在「要不要往 channels_ 里插一条」—— 于是内层再分一次。
    //        写成 if (index == kNew || index == kDeleted) 而不是「先判 kAdded 走 else」，
    //        等价于「凡是不在 kernel interest list 里的，一律 ADD」，语义更贴近本质。
    if (index == kNew || index == kDeleted)
    {
        if (index == kNew)
        {
            int fd = channel->fd();
            channels_[fd] = channel;
            // 【设计】operator[] 是「有则覆盖、无则插入」，这里 kNew 保证是首次插入。
            //        以 fd 为 key，是为了 removeChannel 能 O(1) 按 fd 摘除；value 是不拥有的裸指针。
            // 【易错】这张表的隐含不变式是「一个 fd 在同一 Poller 里只能对应一个 Channel」，
            //        可核实的破坏方式：若两个 Channel 包着同一个 fd，后注册的那个会覆盖
            //        channels_[fd]，前一个就成了「内核里还在监听、表里却查不到」的幽灵，
            //        之后对它再 ADD 会撞 EEXIST → update() 里 LOG_FATAL 直接结束进程。
        }
        else // index == kDeleted
        // 【易错】★重点：隐蔽前提★ 这个 else 分支是【空的】，但它【不能删、也不能改成插入】：
        //   · 为什么可以什么都不做：kDeleted 是「刚才 updateChannel 里 DEL 过」得到的状态，
        //     而 DEL 那条路径【故意没有 erase】channels_ 里的记录（见上面迁移表的 kDeleted 行）。
        //     所以此时 fd→channel 这条映射【还在表里】，无需再插一次；再插只是重复赋值，浪费而已。
        //   · 真正的前提：只有 removeChannel 才会 erase。如果有人把上面 DEL 分支改成
        //     「update(EPOLL_CTL_DEL, channel); channels_.erase(fd);」（看起来很合理！），
        //     那 kDeleted 状态下的表里就没有这条映射了，本分支必须补上 channels_[fd] = channel，
        //     否则 epoll 里加回了 fd，登记表却查不到它 —— 后续 removeChannel 会漏 erase。
        //   · 反过来，如果本分支错写成「insert」也不是 UB（值相同、幂等），只是多一次哈希；
        //     真正危险的是【反过来】：若这里判断错、走到 else 的外层（当成已注册）去 EPOLL_CTL_MOD，
        //     fd 已不在 interest list，内核返回 ENOENT，本文件的 update() 会 LOG_FATAL 直接结束进程。
        //   · 空分支是「靠别处的顺序保证的正确性」，属于典型的【必须写注释】的代码。
        {
        }
        channel->set_index(kAdded);
        // 【设计】先改状态、后下内核调用：顺序上看似危险（若 epoll_ctl 失败，index 已经骗人），
        //        但本项目对 add/mod 失败的处理是 LOG_FATAL（直接 exit），根本不会再回来读这个状态，
        //        所以这个顺序在当前错误策略下是自洽的。若将来改成「失败可恢复」，
        //        必须换成「成功后再置状态」，否则状态机与内核从此不一致（【易错】）。
        update(EPOLL_CTL_ADD, channel);
        // 【系统调用】EPOLL_CTL_ADD：把 fd 挂进 interest list（内核里是一棵以 fd 为键的红黑树），
        //        并把 event 的 events 位与 data 复制进那个 epitem。同 fd 重复 ADD → EEXIST。
        //        注意：ADD 时内核【不会】立即扫描该 fd 的当前状态就返回 —— 但它会在加入后马上检查
        //        该 fd 是否已经就绪，若是，立刻挂进 ready list，于是下一轮 epoll_wait 就能看到。
        //        所以「先有数据、后 enableReading」不会丢事件（这正是 LT 的友好之处）。
    }
    else // channel已经在Poller中注册过了
    // 【设计】到这里 index 只可能是 kAdded（kNew/kDeleted 被上一个分支接走）。
    //        这是状态机里很重要的性质：两个分支【互补且无遗漏】。
    //        【易错】如果有人新增第四个状态（比如 kError），这条 else 会把它误当成 kAdded 处理；
    //        可核实的风险：这里写的是【裸 else】，如果将来有人加第四个状态（比如 kError），
    //        它会被静默当成 kAdded 处理去 MOD 一个没注册的 fd → ENOENT → LOG_FATAL 结束进程。
    {
        int fd = channel->fd();
        // 【⚠问题】可核实：这个局部 fd 在本分支里【没有任何使用点】（update(EPOLL_CTL_*) 自己会再取一次
        //        channel->fd()），属于改代码时留下的死变量；开 -Wunused-variable 会报 warning。
        //        不影响正确性，但读到这里不要误以为「下面按 fd 查了表」—— 本分支压根没查 channels_。
        if (channel->isNoneEvent())
        // 【设计】events_==0 时不能停在「MOD 成 0」：MOD 一个 events 为 0 的 epitem 虽然合法，
        //        但 fd 会一直留在 interest list 里占着内核内存（struct file 的引用也被持着），
        //        且每次该 fd 上有错误/挂断还会惊动 epoll。所以正确做法是 DEL 掉。
        //        这就是 ⑪ handleClose→disableAll() 的真实效果：把连接从 epoll 摘干净，
        //        但【保留 channels_ 里的映射】，以便万一有人再 enableReading 时能低成本地 ADD 回来。
        {
            update(EPOLL_CTL_DEL, channel);
            // 【系统调用】EPOLL_CTL_DEL：把 fd 从 interest list 摘除，内核释放对应 epitem。
            //        【易错】三点常见误解必须澄清：
            //        ① DEL【不会】关闭 fd，也不影响别的 epoll 实例对同一 fd 的注册（各实例互不相干）；
            //        ② 一个 fd 被 close 时，内核会把它从所有 epoll 的 interest list 里自动摘掉，
            //           所以「忘了 DEL 就 close」不会让内核泄漏注册项 —— 但会让本文件的 channels_
            //           留下一条指向已析构 Channel 的悬垂映射，这才是真正的危险（必须走 removeChannel）；
            //        ③ DEL 不要求 fd 当前处于任何特定状态，但对没注册过的 fd 执行 DEL 返回 ENOENT。
            //        【设计】注意这里是先 DEL、下一行才 set_index(kDeleted)：与 ADD 分支一致，
            //        都依赖「失败即 FATAL 退出」的错误策略。
            channel->set_index(kDeleted);
            // 【设计】状态变 kDeleted 而【不 erase】：这是整个状态机最容易被误改坏的一行，
            //        配套前提是上面那个空 else 分支（两处必须一起理解）。
        }
        else
        {
            update(EPOLL_CTL_MOD, channel);
            // 【系统调用】EPOLL_CTL_MOD：只改 interest list 里那条 epitem 的 events 位与 data，
            //        不改 fd 本身。⑨ 里 enableWriting() 就是走这条：从只关心 EPOLLIN(0x3)
            //        变成 EPOLLIN|EPOLLOUT(0x7)；⑩ 发完后 disableWriting() 再 MOD 回 0x3。
            //        【易错】MOD 对没注册过的 fd 返回 ENOENT —— 这正是状态机存在的理由；
            //        本项目里 MOD 失败会 LOG_FATAL 结束进程，等于「状态机走偏 = 不可恢复故障」。
        }
    }
}

// 【流程】⑪ 的最尾一步：TcpConnection::connectDestroyed() → channel_->remove() →
//        EventLoop::removeChannel → 这里（在该 conn 的 subLoop#N 上执行）。
// 【线程】ownerLoop 线程。【设计】与 updateChannel 的本质区别：这里是「Channel 对象本身即将消亡」，
//        所以不仅要让内核忘掉这个 fd，还要把【本文件的映射表】也抹掉 —— 否则 channels_ 里
//        留一枚指向已释放 Channel 的裸指针，下一次有 fd 号复用时就会取到野指针。
// 从Poller中删除channel
void EPollPoller::removeChannel(Channel *channel)
// 【线程】ownerLoop 线程。【流程】⑪。
// 【⚠问题】没有任何前置校验：不看 index_，也不 assert(channel->ownerLoop() 是当前 loop)。
//        通行描述：原版 EventLoop::removeChannel 里有 assertInLoop 与 ownerLoop 断言（以原版仓库为准）；
//        可核实：本项目连 assert 都不用，这两处校验完全缺失。
//        本项目在别的线程误调 removeChannel，就是无锁保护的 unordered_map 并发修改 —— 可能崩，
//        而且崩在离病因很远的地方。
{
    int fd = channel->fd();
    channels_.erase(fd);
    // 【设计】先 erase 后 DEL（顺序无害：DEL 用的是 fd 数值，已经从 channel 里取出来了；
    //        而 channel 对象此刻必须还活着，因为下面还要读它的 index_）。
    // 【易错】unordered_map::erase(key) 对「key 不存在」是安全的（返回 0），所以 kNew 状态下
    //        误调 removeChannel 不会崩，只是没意义 —— 这是本项目对「重复 remove」的隐性容忍。

    LOG_INFO("func=%s => fd=%d\n", __FUNCTION__, fd);

    int index = channel->index();
    // 【设计】同样先取快照。注意【这里不能复用上面那个 const int index】：两个函数各自独立。
    // 【易错】可核实：这里没有任何一致性检查 —— 传进来的 channel 若从没在这个 Poller 注册过
    //        （index_==kNew），erase 会静默返回 0，调用方毫不知情；状态机走偏只能靠日志事后追。
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
        // 【设计】只在 kAdded 时 DEL。为什么不 DEL 也无害：kDeleted 表示早先已 DEL 过，
        //        kNew 表示从没 ADD 过 —— 这两种情况下内核里都没有这个 fd 的注册项，
        //        再去 DEL 只会得到 ENOENT，被 update() 里的 LOG_ERROR 记成假错误。
        //        【易错】fd 若已经被 close（本项目的 ⑪ 顺序是先 remove 后由 ~Socket 关 fd，
        //        所以正常路径不会），对已关闭 fd 调 EPOLL_CTL_DEL 返回 EBADF。
    }
    channel->set_index(kNew);
    // 【设计】状态回到 kNew（而不是 kDeleted）：语义是「Poller 与 Channel 从此再无关系」，
    //        与 Channel 构造时的初值一致 —— 这样同一个 Channel 若被再次 enableReading，
    //        状态机会正确地重新 insert + ADD，不会因为残留 kDeleted 而漏插表（见上面的空 else）。
    //        【易错】如果这里错置成 kDeleted，就会留下一个「表里没有、状态说表里有」的谎言，
    //        下一次 ADD 不插表 → removeChannel 找不到 → 悬垂映射。这条链路值得亲手推一遍。
}

// 【流程】④ 的一部分（poll 内部，numEvents>0 时）。
// 【线程】ownerLoop 线程（就是刚跑完 epoll_wait 的那个线程）。
// 填写活跃的连接
void EPollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
// 【设计】const 成员函数：只读本对象的 events_，不改 epollfd_/channels_；
//        它「写 channel->revents_」不算破坏 const，因为那些 Channel 不是本类的成员（只是成员里存的指针）。
//        这一步是「内核世界 → 对象世界」的唯一翻译口，函数短到 6 行，却是整个 Reactor 的枢纽。
{
    for (int i = 0; i < numEvents; ++i)
    {
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
        // 【系统调用】为什么这个 static_cast 合法且必须正确：
        //        epoll 不解释 data 的内容，它只是把【ADD/MOD 时你塞进 event.data 的那 8 字节】
        //        原样存在 epitem 里，事件就绪时再原样拷贝回用户数组。
        //        本文件 EPollPoller::update 里写了 event.data.ptr = channel，所以此处取回的必然
        //        是同一个 Channel*（由 update 写入、由内核搬运、由这里读回 —— 一条闭环）。
        // 【设计】为什么用 data.ptr 而不是 data.fd：
        //        ① 语义：我们要的是「哪个 Channel 对象」，用 fd 还得再 hash 一次 channels_ 才能拿到对象，
        //           等于把 O(1) 的查找省掉了 —— 这是 epoll 相比 select/poll 的关键优势之一（不需要遍历）。
        //        ② 可行性：epoll_data 是【联合体】，{ void *ptr; int fd; uint32_t u32; uint64_t u64; }，
        //           四个成员共用同一段内存。写 data.fd 会把 data 整体覆盖成「低 4 字节=fd，高 4 字节未定」，
        //           此时再按 ptr 读出来就是一枚由 fd 拼出来的野指针（【易错】这正是本项目保留注释掉的那行的原因）。
        //        ③ 平台性：fd 只有 4 字节，指针在 64 位上是 8 字节，装不下 fd 反过来却「看起来能装下」，
        //           错误不会编译失败，只会运行时崩 —— 所以必须靠约定只用一个成员。
        // 【语法】static_cast 而非 reinterpret_cast：从 void* 转回对象指针属于上行转换，static_cast 语义正确
        //        （有类型的 void* 上行），可读性也表明「这里没有类型猜测，只有类型还原」。
        // 【易错】data.ptr 的【生命周期】不在内核手里：内核只搬 8 字节，不持有引用。
        //        若在 ④ poll 与本轮 handleEvent 之间 Channel 被析构（正常路径不可能，因为都在同一线程、
        //        同一段代码里），这 8 字节就成了野指针 —— 这正是 muduo 把「摘除」严格限制在 ownerLoop 线程的原因。
        channel->set_revents(events_[i].events);
        // 【设计】把「本轮发生了什么」写回 channel，供 handleEventWithGuard 读位分发。
        //        这是一次「跨函数隐式传参」：fillActiveChannels 写，稍后的 handleEventWithGuard 读，
        //        中间隔着 EventLoop::loop 的那圈 for。读代码时容易看不懂 revents_ 从哪来（【易错】）。
        // 【易错】events_[i].events 里可能带着【从没注册过】的 EPOLLERR(0x8)/EPOLLHUP(0x10)，
        //        内核强制上报；所以 revents_ 不能拿去当「我感兴趣的事件」用（那是 events_ 的职责）。
        activeChannels->push_back(channel); // EventLoop就拿到了它的Poller给它返回的所有发生事件的channel列表了
        // 【流程】指针交给调用方；EventLoop::loop 接下来 for (Channel *channel : activeChannels_)
        //        逐个 handleEvent(pollRetureTime_)，即 ⑤⑦⑧⑩⑪ 的分发点。
        // 【设计】只 push 指针，不 push 拷贝：本轮结束后 Poller 与 EventLoop 谁都不拥有它（拥有者仍是业务对象），
        //        所以这个 vector 每轮 clear 都不会漏什么。
    }
}

// 【设计】注意函数名重载带来的阅读陷阱：这是【私有】的 update(int operation, Channel*)，
//        和 Channel::update()、EventLoop::updateChannel()、EPollPoller::updateChannel() 是四件不同的事。
//        本函数是全库【唯一】真正调用 epoll_ctl 的地方 —— 找到它，就找到了 muduo 与内核的边界。
// 更新channel通道 其实就是调用epoll_ctl add/mod/del
void EPollPoller::update(int operation, Channel *channel)
// 【线程】ownerLoop 线程（由 updateChannel/removeChannel 调用）。
// 【流程】③⑤⑦⑨⑩⑪ 的最终落点。入参 operation ∈ {EPOLL_CTL_ADD(1), EPOLL_CTL_DEL(2), EPOLL_CTL_MOD(3)}。
{
    epoll_event event;
    // 【设计】每次调用都在【栈上】新建一个 12 字节的 epoll_event：
    //        epoll_ctl 是「把 event 的内容拷进内核」，不是「让内核长期指向这块内存」，
    //        所以局部变量完全安全（对比：epoll_wait 的 events 数组必须由调用方活得足够久）。
    // 【对比原版】★重点条目★ 本函数每次都【新建 + memset 一个局部 epoll_event】。
    //        可核实：本文件里 epoll_event 只有 event 这一个局部对象；Channel.h 里没有任何
    //        epoll_event 成员（因为 Channel.h 刻意不 include <sys/epoll.h>，见那边的【设计】）。
    //        通行描述：原版把它缓存成 Channel 的成员以省 memset —— 本环境无原版源码，以原版仓库为准；
    //        两种写法的差异只是 12 字节的清零开销，相对紧随其后的 epoll_ctl 系统调用可以忽略。
    // 【语法】未初始化的 POD 局部变量内容是垃圾值 —— 所以下一行 memset 是【必需】的，不是洁癖。
    ::memset(&event, 0, sizeof(event));
    // 【系统调用】为什么必须清零：struct epoll_event { uint32_t events; epoll_data_t data; } 在
    //        __attribute__((packed)) 下是 12 字节；union epoll_data 是 8 字节。
    //        若只写 data.ptr，padding/未用字节里的垃圾会被拷进内核并原样回传，
    //        虽然本项目只读 ptr，但不清零属于「把未定义字节交给内核」，是必须避免的写法。
    //        【易错】memset 之后又给 events 和 data.ptr 赋值，看起来重复清零浪费；
    //        真正不能省的是【结构里可能存在的填充字节】，所以保留 memset 是稳妥的。

    int fd = channel->fd();

    event.events = channel->events();
    // 【设计】这一步把「Channel 的位图」原样交给内核 —— 之所以能零转换，
    //        是因为 Channel::kReadEvent/kWriteEvent 就是用 EPOLLIN|EPOLLPRI/EPOLLOUT 定义的
    //        （见 src/Channel.cc 顶部），数值完全同构。
    //        【易错】这里【没有】把 EPOLLERR/EPOLLHUP 写进去，也没写 EPOLLET：前者内核强制上报，
    //        后者代表本项目全程使用水平触发 LT（Edge-Trigger 需要「一次读干净」的配套代码）。
    //这里是联合体所以data.fd写了没有意义会被data.ptr覆盖
    // 【易错】★重点：联合体说明★ 这行原注释说得对，补充完整：
    //   union epoll_data { void *ptr; int fd; uint32_t u32; uint64_t u64; };
    //   四个成员是【同一块 8 字节】，不是并列的四个字段；给任何一个赋值都覆盖其余全部。
    //   若先写 event.data.fd = 5; 再写 event.data.ptr = channel; → fd 那位被覆盖，无影响；
    //   若只写 event.data.fd = 5; 却在 fillActiveChannels 里读 data.ptr →
    //   读出的是「低 32 位=5、高 32 位=栈垃圾」拼成的 64 位数当指针用 → 段错误或更糟的静默踩内存。
    //   另外内核按 8 字节原样搬运 data，所以【两种写法都可行】，只是本项目选 ptr（省去 fd→channel 的查找）。
    // event.data.fd = fd;
    event.data.ptr = channel;
    // 【设计】这一行是「epoll 事件能 O(1) 找回 Channel」的全部秘密，与 fillActiveChannels 里那次
    //        static_cast 构成一对（一写一读）。
    //        【易错】MOD 时也会重新写 data.ptr：因为 epoll_ctl 是「整个 event 覆盖式更新」，
    //        不是只改 events 位 —— 漏写 data 会让内核把 data 当成新值（这里恰好还是同一个 channel，无碍）。

    // 【系统调用】int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)：
    //   · 作用：增删改这个 epoll 实例 interest list 里关于 fd 的那条注册项。
    //   · op 与前置条件（本项目的状态机就是围着这一列建的）：
    //       EPOLL_CTL_ADD：fd 必须【还没】在本实例里 —— 否则 -1 + EEXIST
    //       EPOLL_CTL_MOD：fd 必须【已经】在本实例里 —— 否则 -1 + ENOENT
    //       EPOLL_CTL_DEL：fd 必须【已经】在本实例里 —— 否则 -1 + ENOENT
    //                    · Linux ≥2.6.9 时 DEL 的 event 参数被忽略（可传 NULL）；本项目仍传了 &event，无害。
    //   · 其它失败：EBADF（epfd 或 fd 无效/已关闭）、EINVAL（fd 不支持 epoll，比如普通文件在老内核上、
    //     或 event 里有非法位）、EFAULT（event 指针不在进程地址空间）、ENOSPC（超 fs.epoll.max_user_watches）。
    //   · 是否阻塞：不阻塞（纯红黑树插入/删除/查找 + 可能的立即就绪检查）。
    //   · 成功返回 0。
    // 【易错】<0 而不是 ==-1 判断：习惯上 epoll_ctl 只返回 0/-1，但内核 API 有时用返回值携带别的含义，
    //        用 <0 更保险。
    // 【设计】为什么把错误处理分两级（下面两个分支）：见各自注释。
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR("epoll_ctl del error:%d\n", errno);
            // 【设计】DEL 失败 → 只记 ERROR，继续跑：因为 DEL 失败几乎总是 ENOENT/EBADF，
            //        发生在「连接本来就要没了」的收尾路径（⑪），此时最不该做的事就是让服务挂掉；
            //        而状态机若真是 kAdded，说明该 fd 已随 close 被内核自动摘除，继续跑是安全的。
            // 【⚠问题】没有把 index_ 修正回来（比如改成 kNew）：若真是状态漂移，之后每次都会重复报错。
        }
        else
        {
            LOG_FATAL("epoll_ctl add/mod error:%d\n", errno);
            // 【设计】ADD/MOD 失败 → 直接 FATAL 结束进程：这两种失败意味着「index_ 与内核状态不一致」
            //        （ADD 撞上 EEXIST，或 MOD 撞上 ENOENT），属于程序 bug 而不是运行时故障；
            //        继续跑会让这个 fd 永远收不到事件（连接假死），不如当场暴露。
            //        【易错】ADD 遇到 EEXIST 有个真实成因：同一个 fd 被两个 Channel 注册进同一 epoll 实例。
            // 【⚠问题】LOG_FATAL 内部是 exit(-1)：不走栈展开、不析构局部对象、不刷新缓冲区，
            //        所有正在处理的连接被硬切。通行描述：原版用 abort 触发 core dump 以便事后定位；
            //        本环境无法核实原版，以原版仓库为准（可核实：本项目 LOG_FATAL 宏里就是 exit(-1)）。
        }
    }
}