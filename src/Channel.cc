/* ============================================================================
 * 【文件】src/Channel.cc
 * 【一句话】实现「事件位 → 回调调用」的分发器，并用 weak_ptr 把回调的存活期钉在宿主对象上；
 *          同时在这里给出三个事件常量的真值（因为这里才允许看见 <sys/epoll.h>）。
 * 【Reactor 角色】Handler 分发器（Demultiplexer 的下游、业务回调的上游）。
 * 【依赖】<sys/epoll.h>（EPOLLIN/EPOLLPRI/EPOLLOUT/EPOLLERR/EPOLLHUP 位定义，用来给常量赋值、
 *        以及在 handleEventWithGuard 里判位）、"Channel.h"（本类）、"EventLoop.h"（update/remove
 *        要真的调 loop_ 的成员函数，这里必须有完整类型）、"Logger.h"（LOG_INFO 打印 revents_）。
 * 【谁用它】EventLoop::loop() 遍历活跃 channel 时调 handleEvent（④→⑤/⑦/⑧/⑩/⑪）；
 *        Acceptor/TcpConnection/EventLoop 自己调 enable/disable/remove/tie。
 * 【先修知识】1) weak_ptr::lock() 提升语义与引用计数 2) epoll 事件位（见 Channel.h 的位表）
 *          3) RAII 与 unique_ptr 成员析构顺序 4) 「对象析构 vs 成员函数正在执行」这一类 UB
 * 【难度】★★★★★（tie/handleEvent 是全库最经典的面试点）  【建议耗时】40 分钟
 * 【自检】① guard 这个局部变量多活几行，到底买到了什么保证？
 *        ② 如果 TcpConnection 已经析构而 channel 还挂在 activeChannels_ 上，会发生什么？
 *        ③ handleEventWithGuard 为什么第一个分支是「EPOLLHUP 且非 EPOLLIN」而不是直接判 EPOLLHUP？
 * 【口径】本文件凡【对比原版】条目里涉及原版 muduo 具体写法的话，本环境无网络、无原版源码，
 *        一律「以原版仓库为准」，不当作已核实事实；凡写「可核实：…」的都是能在本仓库
 *        直接 grep / 读码验证的事实。本文件的隐患与要点已全部就地打了标签，用 grep 【⚠问题】/【易错】/【对比原版】 即可索引。
 * =========================================================================== */

#include <sys/epoll.h>
// 【设计】epoll 头文件只出现在 .cc：这就是 Channel.h 里三个常量只能声明、不能定义的根本原因，
//        「谁需要内核位值，谁才包含内核头」，把依赖收在最小的编译单元里。

#include "Channel.h"
#include "EventLoop.h"
// 【设计】头文件里对 EventLoop 只有前向声明，本行才把完整类型补上 ——
//        因为下面 update()/remove() 要写 loop_->updateChannel(this)，要解引用就必须知道成员。
//        这一行 + Channel.h 里的 class EventLoop; 合起来就是「循环依赖的编译期解法」：
//        头文件之间靠声明断开，源文件负责让两边都会合。
#include "Logger.h"

// ==========================================================================================
// 【设计】三个事件常量的「唯一真值点」。写成 .cc 里的定义（而不是 .h 里的 constexpr），
//        是因为它们的值要用 <sys/epoll.h> 的宏去算：放在头文件就等于把 epoll 塞进全库每个编译单元。
//        这样带来的性质：Channel 存的位值 == epoll 的位值，可以直接塞给 epoll_event.events，
//        不需要任何映射表；换成 poll/select 实现时，需要改的就是这几行 + Poller 实现。
// 【语法】static const int 成员「类内声明 + 类外定义」是必须的搭配；少了这三行定义，
//        链接期报 undefined reference to Channel::kReadEvent（编译阶段发现不了）。
//        定义处不能重复写 static。
// ==========================================================================================
const int Channel::kNoneEvent = 0; //空事件
// 【系统调用】0：对 epoll 来说「什么都不感兴趣」。EPollPoller::updateChannel 见到 isNoneEvent()
//        就会打 EPOLL_CTL_DEL 把这个 fd 从 interest list 摘掉。
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI; //读事件
// 【系统调用】数值拆解：EPOLLIN(0x0001) | EPOLLPRI(0x0002) = 0x0003 = 十进制 3。
//        所以 enableReading() 一次置的是两个位（bit0 普通可读、bit1 带外可读），
//        handleEventWithGuard 里对应的判据也是 revents_ & (EPOLLIN | EPOLLPRI)。
// 【易错】EPOLLPRI 只对带外数据（TCP urgent）有意义；本项目在 Acceptor/TcpConnection
//        都没显式注册它，它是被 kReadEvent 顺带打开的 —— 因此读事件永远等于 3，不是 1。
//        面试常问「kReadEvent 是几」：答 3（=0x3），并说清它包含 EPOLLPRI。
const int Channel::kWriteEvent = EPOLLOUT; //写事件
// 【系统调用】EPOLLOUT = 0x0004 = 十进制 4，只有一个位（bit2）。
// 【易错】EPOLLERR(0x0008)/EPOLLHUP(0x0010) 故意不在这里注册：内核无论如何都会上报它们，
//        在 events 里写了也不会改变行为，反而让人误以为「不写就收不到」。

// EventLoop: ChannelList Poller
// 【流程】②（Acceptor 构造 acceptChannel_）、①（EventLoop 构造 wakeupChannel_）、
//        ⑥（TcpServer::newConnection 里 new TcpConnection → 构造 channel_）。
// 【线程】对象在谁的线程被 new 就在谁的线程构造（connfd 的 channel 在 mainLoop 里构造），
//        但注册进 epoll 一定发生在 ownerLoop 线程（⑦ 的 connectEstablished）。
// 【设计】注意「构造 ≠ 注册」：此时 events_=0、index_=-1，epoll 还不知道这个 fd 的存在。
Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop)
    , fd_(fd)
    , events_(0)
    // 【设计】初值 0 与 kNoneEvent 等价：新 channel 默认对任何事件都不感兴趣，
    //        必须由业务显式 enableReading()/enableWriting() 才会进 epoll。
    //        这避免了「Acceptor 一 new 出来就收 accept 事件但回调还没设好」的空窗期危险。
    , revents_(0)
    // 【设计】还没发生任何事件，初值 0 保证就算误调 handleEventWithGuard 也不会命中任何分支。
    , index_(-1)
    // 【设计】-1 就是 kNew 的值，但这里不能写 kNew —— kNew/kAdded/kDeleted 三个常量定义在
    //        src/EPollPoller.cc（文件作用域的全局 const，跨文件共享，没有头文件声明！），
    //        Channel 看不见它们，只能硬编码 -1。这是本项目一个【易错】的隐式契约：
    //        如果哪天有人把 kNew 改成别的值，EPollPoller 的分支会认为「新 channel 是未知状态」，
    //        直接走 else 分支去 EPOLL_CTL_MOD 一个从没 ADD 过的 fd → ENOENT → LOG_FATAL 退出。
    //        【对比原版】可核实：本仓库 include/ 下没有任何一处声明这三个名字（全库 grep 只命中
    //        本文件这三行定义），Channel 侧只能把 kNew 的值写死成 -1，两边靠「数值相等」耦合。
    //        原版是否把它们收进某个命名空间并在头文件回指，本环境无法核实，以原版仓库为准。
    , tied_(false)
    // 【设计】默认不 tie：Acceptor 的 channel、wakeup 的 channel 都不 tie（宿主不会先于它析构），
    //        只有 TcpConnection 的 channel 会 tie。handleEvent 的热路径因此只多读一个 bool。
{
}

// 【设计】空析构，这是刻意的。fd 的关闭责任属于 Socket（TcpConnection::socket_ 的 ~Socket() 里
//        ::close(sockfd_)），Channel 只是「监听者」。
// 【⚠问题】如果有人误以为 ~Channel 会关 fd，就会造成 fd 泄漏（Socket 已被 unique_ptr 复位但没人关）
//        或 double close（两边都关）。判断依据只有一条：谁 open/accept 出来，谁负责 close。
//        TcpConnection 的成员顺序是 socket_ 在前、channel_ 在后，析构顺序相反：
//        ~TcpConnection 先析构 channel_（关 nothing），再析构 socket_（真正 close）—— 顺序恰好安全。
// 【易错】析构前必须已经 disableAll()+remove()（⑪ 的 connectDestroyed 做了这两步）。
//        否则 epoll 的 interest list 里可能还留着一个 data.ptr 指向已释放 Channel 的表项，
//        下一次 epoll_wait 就会把野指针当 Channel 交给 fillActiveChannels —— 典型 use-after-free。
Channel::~Channel()
{
}

// channel的tie方法什么时候调用过?  TcpConnection => channel
/**
 * TcpConnection中注册了Channel对应的回调函数，传入的回调函数均为TcpConnection
 * 对象的成员方法，因此可以说明一点就是：Channel的结束一定晚于TcpConnection对象！
 * 此处用tie去解决TcpConnection和Channel的生命周期时长问题，从而保证了Channel对象能够在
 * TcpConnection销毁前销毁。
 **/
void Channel::tie(const std::shared_ptr<void> &obj)
// 【流程】⑦ TcpConnection::connectEstablished() 里 channel_->tie(shared_from_this())，仅这一处。
// 【线程】subLoop#N（connectEstablished 被 runInLoop 派发到该连接所属的 IO 线程执行）。
// 【设计】两行代码：把强引用「降级」成弱引用存下来（tie_ = obj 会做类型擦除，shared_ptr<TcpConnection>
//        → shared_ptr<void> → weak_ptr<void>，控制块共用，不增加引用计数），再把开关拨到 true。
//        用 weak 而不用 shared 的原因：channel_ 是 TcpConnection 的成员，若 Channel 反过来又持
//        强引用 → 循环引用 → 两边永不归零 → 连接与 fd 全泄漏。
//        用 shared_ptr<void> 作参数的原因：Channel 不想（也不能）知道宿主是什么类型，
//        类型擦除后 lock() 出来的 shared_ptr<void> 照样能靠控制块里的删除器正确管理生命周期。
// 【易错】原注释写的「Channel 的结束一定晚于 TcpConnection 对象」，方向要说清楚：真实约束是
//        「TcpConnection 先析构时，它的 Channel 会跟着一起析构（成员关系）」，
//        tie 要处理的是这个瞬间前后的竞态：EventLoop 手里的裸 Channel* 可能已经指向被释放的内存。
//        换句话说：tie 保护的是「宿主没了就别再调它的回调」，它并不能阻止 channel 自身被析构。
// 【面试】问「muduo 里 tie 解决什么问题」：一句话 ——
//        解决「TcpConnection 已析构但 Channel 还在跑回调」造成的悬垂 this 调用，
//        手段是 weak_ptr + handleEvent 里的 lock() 提升（配合 shared_ptr guard 延长宿主存活期）。
{
    tie_ = obj;
    tied_ = true;
}
//update 和remove => EpollPoller 更新channel在poller中的状态
/**
 * 当改变channel所表示的fd的events事件后，update负责再poller里面更改fd相应的事件epoll_ctl
 **/
// 【流程】所有 enable/disable（③ listen、⑦ 开读、⑨ 开写、⑩ 关写、⑪ disableAll）的公共尾巴。
// 【线程】必须与 channel 的 ownerLoop 同线程执行（本项目无断言，靠调用方自律）。
// 【设计】三层（数上其实是四层）转发链，每一层都只做「自己该做的事」：
//        Channel::update()
//          └─→ EventLoop::updateChannel(channel)     # 只负责转交给自己的 poller_（不判状态）
//                └─→ Poller::updateChannel(channel)  # 纯虚接口，编译期不知道具体实现
//                      └─→ EPollPoller::updateChannel  # 看 index_ 决定 ADD/MOD/DEL 语义
//                            └─→ EPollPoller::update   # 填 epoll_event + 真正的 epoll_ctl 系统调用
//        为什么要绕这么多层？① Channel 不该持有 Poller*（Poller 是 EventLoop 的私事）；
//        ② Channel 不该知道 epoll 的状态机；③ EventLoop 想保持「我只认 Poller 抽象接口」。
//        代价：每次开关事件 4 层调用 + 1 次系统调用。
void Channel::update()
{
    // 通过channel所属的eventloop，调用poller的相应方法，注册fd的events事件
    loop_->updateChannel(this);
    // 【设计】传 this 而不是传 fd：Poller 需要的是整个 channel（要读 events()、写 set_index()、
    //        把 data.ptr 指回它）。这也是「channels_ 里存裸 Channel*」能对上的原因。
    // 【易错】loop_ 是构造时定死的裸指针，若 EventLoop 已析构（⑫ 退出阶段）而还有人在调
    //        enableReading，就是野指针调用；muduo 用「loop 比所有 channel 活得久」保证不出现。
}

// 在channel所属的EventLoop中把当前的channel删除掉
// 【流程】⑪ TcpConnection::connectDestroyed() 的最后一步（subLoop#N），把 channel 从 Poller 除名。
// 【线程】subLoop#N（该连接所属 IO 线程）。
void Channel::remove()
{
    loop_->removeChannel(this);
    // 【⚠问题】这里没有 assert(isNoneEvent())，也没看 index_：即使 events_ 还非零、还处于 kNew，
    //        remove 也会照转不误。通行描述：原版强制「先 disableAll() 再 remove()」，
    //        顺序错了在原版会当场 assert 挂掉（以原版仓库为准）；本项目只有 EPollPoller::removeChannel 里 if(index==kAdded)
    //        会兜住「没注册过却来 remove」的情况，但语义上的错用不再报警。
    // 【易错】remove() 也不 close fd、不 delete this —— 它只是「除名」。
}

// ==========================================================================================
// 【流程】④ epoll_wait 返回后 EventLoop::loop() 对每个活跃 channel 调它 —— 全库所有事件分发的入口。
//        ⑤ accept 就绪、⑦ 首次可读、⑧ 数据到达、⑩ 可写、⑪ 挂断/EOF，全都从这一函数进去。
// 【线程】ownerLoop 线程：mainLoop（listenfd 与 wakeupFd 的 channel）或 subLoop#N（connfd 的 channel）。
// 【设计】handleEvent 与 handleEventWithGuard 拆两层的唯一理由：把 guard 的作用域精确地
//        套在「真正会碰宿主对象的那段代码」外面，既不多留（延迟析构）也不少留（悬垂）。

// ★★★ 本项目最经典面试点：逐行讲清 handleEvent 在防什么 ★★★
// 背景问题：readCallback_ 里绑的是 TcpConnection 的成员函数（this 已被 std::bind 捕获）。
//          若在「事件已经上报、回调还没执行/正在执行」这段时间里，TcpConnection 对象被别的
//          路径析构了（引用计数归零），那么它持有的 unique_ptr<Channel> channel_ 会一起被释放，
//          EventLoop::activeChannels_ 里那枚裸 Channel* 就指向已释放内存 ——
//          再调 channel->handleEvent(...) 就是 use-after-free，而且 this->tied_/readCallback_
//          读到的是垃圾数据，通常表现为跳进随机地址或段错误。
// 典型场景（⑦/⑧/⑨）：用户在自己的 onMessage 里 conn->shutdown()，或对同一连接
//          提前调了 TcpServer::stop()/erase 掉 connections_ 里唯一的 shared_ptr，
//          导致同一轮 for 循环里排在后面的那个 channel 已经属于一个死掉的连接。
// 解法：weak_ptr 提升。tie_ 是弱引用，宿主一死 lock() 就返回空 shared_ptr，
//          于是「宿主已析构」这个事实能被安全地探测到（而不是靠读已被释放的内存）。
// ==========================================================================================
// 【流程】④→⑤⑦⑧⑩⑪：EventLoop::loop() 遍历 activeChannels_ 时逐个调用，是全库事件分发的总闸口。
void Channel::handleEvent(Timestamp receiveTime)
{
    if (tied_)
    // 【设计】先查 bool 再碰 weak_ptr：绝大多数 channel（listenfd、wakeupfd）根本没 tie 过，
    //        一个分支就跳过了后面所有开销，热路径几乎零成本。
    // 【易错】tied_ 为 true 不代表宿主还活着，只代表「曾经 tie 过」；生死要靠 lock() 判。
    {
        std::shared_ptr<void> guard = tie_.lock();
        // 【设计】关键一行。lock() 的语义：原子地检查控制块，如果强引用数 > 0 就 +1 并返回一个
        //        指向宿主的 shared_ptr<void>；否则返回空对象。返回的 guard 是一个「局部强引用」。
        //        为什么必须有 guard 这个变量（而不是写 if (tie_.lock()) ）：
        //        guard 的生命周期 = 它所在的花括号块 = handleEventWithGuard 的整个调用期。
        //        只要它活着，宿主的引用计数就至少为 1，任何线程都不可能在这段时间里把
        //        TcpConnection 真正释放 → 回调里那个被 bind 捕获的 this 必定有效。
        //        这就是「延长宿主存活期到回调结束」，是比「判空」更重要的一半作用。
        // 【易错】若写成 if (tie_.lock()) { handleEventWithGuard(...); } —— 提升出的临时
        //        shared_ptr 在 if 条件求值完立刻析构，计数又掉回原值，回调期间就没有任何保护了：
        //        判空确实判对了，但保护丢了。这一行必须有名有姓地接住。
        // 【线程】lock() 对控制块的增减是原子的（shared_ptr 的引用计数是线程安全的），
        //        所以「另一个线程正在释放最后一个强引用」这种竞态是能被 lock() 正确挡住的。
        // 【语法】shared_ptr<void> 是合法的：类型擦除后仍能正确析构宿主（删除器在构造时已被记住），
        //        只是不能 void* 解引用 —— 这里也不需要解引用，只要它「活着并计数」。
        if (guard)
        // 【设计】把「有没有提升成功」翻译成「宿主是否还在」。空 shared_ptr 转 bool 为 false。
        {
            handleEventWithGuard(receiveTime);
            // 【设计】被调用时 guard 已在手上，宿主被这次回调「钉」住了：
            //        即使用户回调里把 TcpServer 的 connections_ 清掉、把最后一个外部引用扔了，
            //        析构也被推迟到本函数返回之后 —— 回调里访问 this（TcpConnection 成员）是安全的。
            // 【流程】⑤⑦⑧⑩⑪ 的实际业务从这里开始。
        }
        // 如果提升失败了 就不做任何处理 说明Channel的TcpConnection对象已经不存在了
        // 【设计】「提升失败 → 什么都不做」是正确而非偷懒：宿主已死，事件应当被丢弃。
        //        这里不调 closeCallback_（宿主都没了，通知谁？）、也不 LOG_ERROR（正常竞态，不是错误）。
        // 【易错】注意 guard 在下面的 '}' 处才析构，也就是说即使提升失败，
        //        guard 这个空 shared_ptr 也仍然在作用域里（空对象析构是 no-op，无害）。
    }
    else
    // 【设计】没 tie 过的 channel 直接进分发：listenfd 的宿主是 Acceptor（Acceptor 的成员对象，
    //        与它同生共死，不会「先于 channel 死」）、wakeupfd 的宿主是 EventLoop（同理）。
    //        这是 tie 只在 TcpConnection 上用的原因：只有连接对象是被 shared_ptr 动态管理、
    //        可能在任意时刻归零析构的。
    {
        handleEventWithGuard(receiveTime);
        // 【易错】else 分支里 guard 根本不存在，若在这种 channel 的回调中把宿主析构了，
        //        本函数返回时的 this 就悬垂了 —— 所以约定：未 tie 的回调绝不能销毁自己的 channel。
    }
}

// 【流程】被 handleEvent 在 ownerLoop 线程调用（④ 之后）；四个分支分别通向
//        ⑪（close/error）、⑧（read）、⑩（write）。
// 【设计】「WithGuard」的名字就来自上一层：进到这个函数，就意味着宿主存活性已经被 guard 担保过了。
// 【设计】四个分支顺序：close → error → read → write。为什么是这个顺序？
//        ① 先处理「连接已经不行了」（HUP/ERR）：一旦触发 closeCallback_，连接会走 disableAll()
//           并从 Poller 除名，后面的 read/write 分支即使再进去也只会读到空 buffer 或立刻失败；
//        ② 错误排在读写前：EPOLLERR 之后 read/write 都可能直接报错，先通知上层比先搬数据更安全；
//        ③ 读在写前：读可能产生要回写的数据（echo），也可能直接发现 EOF 触发 handleClose，
//           而写分支需要的是「缓冲区空了」这个更弱的条件，放最后不会饿死；
//        ④ 四个分支都用 if 而不是 else if：一次 epoll_wait 完全可能同时置多个位
//           （EPOLLIN|EPOLLOUT|EPOLLHUP），必须都能被处理。
//        【对比原版】可核实：本项目代码里从不出现 EPOLLRDHUP（既没注册也没判位），
//           所以「对端关闭」只有两条路能发现：① EPOLLIN + read 返回 0（⑪ 的主路径），
//           ② EPOLLHUP 且没有 EPOLLIN（下面第一个分支）。原版是否用 EPOLLRDHUP 表达
//           「对端关闭」、read 分支掩码怎么写，本环境无法核实，以原版仓库为准。
//           两条路在业务侧的落点见 src/TcpConnection.cc 的 handleRead / handleClose 注释。
// 【线程】ownerLoop 线程（mainLoop 或 subLoop#N，取决于这个 fd 属于哪个 loop）。
void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    LOG_INFO("channel handleEvent revents:%d\n", revents_);
    // 【⚠问题】每个活跃事件都打一条 INFO：⑧ 高频 IO 下（每秒几万包）日志量与格式化开销巨大，
    //        而且日志写在分发最前面，即使下面一个分支都没命中也会打。
    //        生产环境应该用 LOG_DEBUG（本项目的 LOG_DEBUG 在未定义 MUDEBUG 时是空宏，
    //        编译期直接消失，零开销），或者至少在 revents_ 非零时才打。
    // 【易错】%d 打的是十进制，revents_=0x13 会显示成 19；读日志时记得换算成位（见 Channel.h 位表）。
    // 关闭
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) // 当TcpConnection对应Channel 通过shutdown 关闭写端 epoll触发EPOLLHUP
    // 【系统调用】EPOLLHUP(0x0010) 表示管道/连接挂断；EPOLLIN(0x0001) 表示仍有可读数据。
    // 【设计】为什么必须加 && !(revents_ & EPOLLIN)：这是【易错】也是高频面试题。
    //        对端关闭连接时，内核经常同时置 EPOLLIN 和 EPOLLHUP（0x0001|0x0010=0x0011=17）：
    //        前者代「接收缓冲区里还有未读数据」（包括让它 read 返回 0 这个 EOF 事件），
    //        后者代「已经挂断」。如果只判 EPOLLHUP 就走 closeCallback_，
    //        连接会被立刻拆掉，缓冲区里最后一段数据（对端 close 前发的最后一包）就永远丢了。
    //        加上「且不可读」之后：本轮先走下面的 read 分支把数据交给 readCallback_，
    //        数据读完/EOF 后下一轮（或同一次 revents 里没有 EPOLLIN 时）才会触发关闭。
    // 【易错】本项目的 connfd 根本没注册 EPOLLHUP（kReadEvent/kWriteEvent 里没有它），
    //        但内核无视注册、上报错误/挂断时永远会带上这两个位，所以这个分支并非死代码。
    // 【对比原版】通行描述：原版还会显式注册 EPOLLRDHUP 来区分「对端关闭写端」与「彻底挂断」，
    //        并保留同样的 (EPOLLHUP && !EPOLLIN) 判据 —— 本环境无原版源码，具体以原版仓库为准。
    //        本项目可核实的事实：connfd 的 events_ 里永远没有 EPOLLRDHUP 这一位。
    {
        if (closeCallback_)
        {
            closeCallback_();
            // 【流程】⑪ 的第一步：TcpConnection::handleClose（disableAll → connectionCallback_
            //        → closeCallback_=TcpServer::removeConnection）→ 线程跳转 2/3。
            // 【易错】这个调用可能一路把宿主 TcpConnection 析构掉（若 guard 是唯一强引用，
            //        则不会 —— guard 把它抬到了 1，析构会延后到 handleEvent 返回之后；
            //        这正是 guard 存在的又一个理由）。
            //        反过来说：如果本函数后面还有分支要碰宿主（比如接着走 readCallback_），
            //        而宿主已经被 closeCallback_ 拆了 —— 那就靠 guard 兜住。
        }
    }
    // 错误
    if (revents_ & EPOLLERR)
    // 【系统调用】EPOLLERR(0x0008)：连接出错（如收到 ICMP port unreachable、连接 reset）。
    //        同样不需要注册就会上报。错误排在读之前，因为错误后的 read 可能直接返回 -1。
    {
        if (errorCallback_)
        {
            errorCallback_();
            // 【流程】⑪ 的异常分支：TcpConnection::handleError（只是 getsockopt(SO_ERROR) 打日志）。
            // 【易错】handleError 并不关闭连接！真正的关闭仍然靠后续的 EPOLLHUP/read==0 触发。
            // 【⚠问题】注意它和 closeCallback_ 是两个独立分支，若同轮同时置 EPOLLERR|EPOLLHUP，
            //        会先 error 再 close，两个回调都会执行。
        }
    }
    // 读
    if (revents_ & (EPOLLIN | EPOLLPRI))
    // 【系统调用】掩码写成 (EPOLLIN|EPOLLPRI) = 0x0003，与 kReadEvent 同值，
    //        即「内核报的位里只要有任何一个是可读类，就去读」。
    //        这里用 revents_ & 掩码而不是 revents_ == kReadEvent，因为 revents_ 往往还带着
    //        EPOLLERR/EPOLLHUP 等没注册却上报的位，全等判断永远不成立（【易错】初学者常写错）。
    {
        if (readCallback_)
        {
            readCallback_(receiveTime);
            // 【流程】⑤ Acceptor::handleRead（accept4 得 connfd）/ ⑦⑧ TcpConnection::handleRead
            //        （Buffer::readFd(readv) → 用户 onMessage）。
            // 【线程】mainLoop（listenfd/wakeupfd）或 subLoop#N（connfd）。
            // 【易错】readCallback_ 可能同步地把这个连接拆掉（用户回调里 shutdown+erase），
            //        那正是需要 tie/guard 的场景；拆掉后本函数剩余的 write 分支再碰宿主
            //        就靠 guard 抬高的引用计数保命。
        }
    }
    // 写
    if (revents_ & EPOLLOUT)
    // 【系统调用】EPOLLOUT(0x0004)：发送缓冲区有空间。因为是水平触发(LT)，只要没关这个位，
    //        每轮 epoll_wait 都会重新报 —— 所以 ⑩ 写完必须 disableWriting()（见 Channel.h）。
    {
        if (writeCallback_)
        {
            writeCallback_();
            // 【流程】⑩ TcpConnection::handleWrite：把 outputBuffer_ 继续往 fd 写，
            //        写完 disableWriting() + writeCompleteCallback_；若 state_==kDisconnecting
            //        再 shutdownInLoop()。
        }
    }
}