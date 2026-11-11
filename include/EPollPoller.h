/* ============================================================================
 * 【文件】include/EPollPoller.h
 * 【一句话】把 epoll 的三个系统调用（epoll_create1 / epoll_ctl / epoll_wait）包成 Poller 接口
 *          的具体实现，并持有「内核就绪数组」events_ 这张缓冲区。
 * 【Reactor 角色】Demultiplexer 的 epoll 实现（全库唯一实现）。
 * 【依赖】"Poller.h"（继承抽象接口、用它的 ChannelList/ChannelMap）、<sys/epoll.h>（epoll_event 类型）、
 *        <vector>（EventList）、"Timestamp.h"（poll 返回值）。对 Channel 只前向声明。
 * 【谁用它】EventLoop 通过 unique_ptr<Poller> 指向它（① / ③ 由 Poller::newDefaultPoller 造，
 *        见 src/DefaultPoller.cc）；④ 每轮 loop 调 poll，③⑦⑨⑩⑪ 任何开关事件都落到 updateChannel。
 * 【先修知识】1) epoll 的 interest list（内核里那棵红黑树）与 ready list（就绪双链表）
 *          2) 水平触发 LT vs 边沿触发 ET 3) 联合体 union 的内存共享 4) 虚函数 override
 * 【难度】★★★★☆   【建议耗时】25 分钟（配合 src/EPollPoller.cc 一起读，那边更细）
 * 【自检】① epoll 用 API 一共三步，分别对应本类的哪三个成员函数/变量？
 *        ② events_ 为什么必须是成员变量而不是 poll() 里的局部数组？
 *        ③ kInitEventListSize=16 定小了会怎样、定大了会怎样？
 * 【口径】本文件凡【对比原版】条目里涉及原版 muduo 具体写法的话，本环境无网络、无原版源码，
 *        一律「以原版仓库为准」，不当作已核实事实；凡写「可核实：…」的都是能在本仓库
 *        直接 grep / 读码验证的事实。本文件的隐患与要点已全部就地打了标签，用 grep 【⚠问题】/【易错】/【对比原版】 即可索引。
 * =========================================================================== */

#pragma once

#include <vector>
#include <sys/epoll.h>
// 【设计】本文件是全库唯一在【头文件】里 include <sys/epoll.h> 的地方 ——
//        因为成员 EventList = std::vector<epoll_event> 需要 epoll_event 的完整定义。
//        这恰好反衬出 Channel.h 为什么坚持不 include：Channel 只需 int 存位值，不需要内核类型。

#include "Poller.h"
// 【设计】继承关系：EPollPoller : public Poller（公有继承，is-a）。EventLoop 只认 Poller*，
//        这里换成 EPollPoller 对上层完全透明 —— 策略可替换的落点。
#include "Timestamp.h"

// 【系统调用】epoll 家族三步（本类的分工对照）：
//   1) int epollfd = epoll_create1(flags)        → 构造里做一次，存进 epollfd_
//        创建一个 epoll 实例 = 内核里一个 eventpoll 对象（含 interest 红黑树 + ready 链表 + 等待队列）
//   2) epoll_ctl(epollfd, op, fd, &event)        → update(operation, channel) 里做，op 三选一
//        EPOLL_CTL_ADD(1) 登记 / EPOLL_CTL_DEL(2) 摘掉 / EPOLL_CTL_MOD(3) 改感兴趣的事件位
//   3) int n = epoll_wait(epollfd, events, maxevents, timeoutMs) → poll() 里做，每轮一次
//        把 ready list 里的事件【拷贝】到用户数组 events，返回个数 n（0=超时，-1=出错）
//   4) 收尾：::close(epollfd)                     → 析构里做一次
/**
 * epoll的使用:
 * 1. epoll_create
 * 2. epoll_ctl (add, mod, del)
//        ↑ 注意本类里 add/mod/del 三个动作全部走同一个私有函数 update(int operation, Channel*)，
//          「选哪个 operation」的决策在 updateChannel 的 index_ 状态机里。
 * 3. epoll_wait
 **/

class Channel;
// 【设计】只前向声明：本头文件里 Channel 只以指针出现在函数参数上，不需要完整类型。

class EPollPoller : public Poller
// 【设计】本类自己没有成员函数需要「查 fd → 找 Channel」：因为 data.ptr 里直接存了 Channel*
//        （见 EPollPoller::update 里 event.data.ptr = channel），这正是 epoll 高效用法的关键。
{
public:
    EPollPoller(EventLoop *loop);
    // 【流程】① / ③：EventLoop 构造 → Poller::newDefaultPoller(this) → new EPollPoller(loop)。
    // 【线程】EventLoop 所在线程（mainLoop→main 线程，subLoop#N→该 IO 线程），
    //        因此每个 loop 有自己独立的 epollfd_：一个进程有几个 loop 就有几个 epoll 实例。
    ~EPollPoller() override;
    // 【语法】override 在析构函数上合法且有用：如果忘了写虚析构（基类 Poller 已写 virtual），
    //        override 会立刻编译报错，把「通过基类指针 delete 派生类」这件生死攸关的事钉在编译期。

    // 重写基类Poller的抽象方法
    // 【设计】三个 = 0 的纯虚函数在这里全部落地，EPollPoller 因此成为可实例化的具体类。
    Timestamp poll(int timeoutMs, ChannelList *activeChannels) override;
    // 【流程】④ 每轮一次（EventLoop::loop 的 while 循环体第一句），是「等待」发生的地方。
    // 【线程】ownerLoop 线程。【设计】override 而非 virtual：表达「我在重写基类虚函数」，
    //        签名写错（少个 const、参数类型变了）会当场编译失败而不是悄悄变成新函数。
    void updateChannel(Channel *channel) override;
    // 【流程】Channel::update → EventLoop::updateChannel → 这里；用 index_ 状态机决定 ADD/MOD/DEL。
    void removeChannel(Channel *channel) override;
    // 【流程】Channel::remove → EventLoop::removeChannel → 这里；从 channels_ 除名 + 必要时 DEL。

private:
    // 【设计】epoll_wait 的 maxevents 初值。选 16 的理由：绝大多数时刻「同一瞬间真正就绪」的
    //        fd 很少（即使有几万连接，同一微秒活跃的也就几十个），16 个 * 12 字节 = 192 字节，
    //        起步成本可忽略；真不够时 poll() 里发现 numEvents == size 就原地翻倍扩容。
    //        这样「常见情况省内存、突发情况自动涨、且永不缩」，是典型的几何增长策略。
    // 【语法】这里是【类内直接给值】的 static const int，和 Channel.h 里那三个「只声明、值在 .cc」
    //        的写法不同，原因是 16 是个纯教学常量、不依赖任何内核宏；而且它只被用作编译期常量
    //        （在 .cc 里作为 vector 构造实参，属于可用常量表达式替代的场景），不需要 .cc 里再定义。
    static const int kInitEventListSize = 16;
    // 【易错】16 不是上限，只是初值：poll() 里会自动 resize 翻倍；也不要拿它去推测连接数上限。

    // 填写活跃的连接
    // 【设计】poll 与「就绪名单」之间的胶水：把内核数组里的结果翻译成 Channel* 列表，
    //        顺带把发生的事件位塞回 channel->revents_（关键副作用）。声明成 const 是诚实的：
    //        它不改 Poller 自己的任何成员，只读 events_ 并写「别人的」对象。
    void fillActiveChannels(int numEvents, ChannelList *activeChannels) const;
    // 【流程】④ 内部，poll() 在 numEvents>0 时调用。【线程】ownerLoop 线程。
    // 更新channel通道 其实就是调用epoll_ctl
    // 【设计】私有、不重载、带 operation 参数：ADD/MOD/DEL 三种语义共用一份「填 epoll_event + 调用」的代码，
    //        避免三份拷贝走偏；「该用哪个 operation」的判断留给 updateChannel（策略与执行分离）。
    void update(int operation, Channel *channel);
    // 【流程】EPollPoller::updateChannel 与 removeChannel 里调用；唯一真正调 epoll_ctl 的地方。
    // 【线程】ownerLoop 线程。【⚠问题】本类没有对外暴露 update，但它和基类的 updateChannel 名字只差
    //        一点，读代码时要分清：update 是「执行一次 epoll_ctl」，updateChannel 是「状态机决策」。

    using EventList = std::vector<epoll_event>; // C++中可以省略struct 直接写epoll_event即可
    // 【语法】C 语言里 typedef 了 struct epoll_event 的别名，所以 C++ 可以只写 epoll_event；
    //        若写成 struct epoll_event 也仍然合法。
    // 【设计】为什么必须是成员变量（而不是 poll() 里的局部数组）：
    //        ① 每轮 loop 都要用同一块缓冲区，局部数组要么 VLA（非标准）要么每次分配/清零，
    //           高频路径上开销可观；② 只有做成成员，才能「装满了再翻倍扩容」并把容量长期保留，
    //           稳态下再也不产生内存分配。这是 muduo 「一个 loop 一个 Poller 一块就绪缓冲区」的设计。
    // 【易错】vector<epoll_event> 必须是「连续内存」才能直接交给内核写 —— 它确实是；
    //        但 resize() 扩容可能搬家，所以绝不能把 &events_[0] 缓存成长期指针（每次 poll 现取）。

    int epollfd_;      // epoll_create创建返回的fd保存在epollfd_中
    // 【设计】本 epoll 实例的句柄。一个进程里它「管辖」哪些 fd，完全由 epoll_ctl 决定。
    // 【易错】它自己也是个 fd，会占用进程的 fd 配额（RLIMIT_NOFILE）；且注意它与被监听的 connfd 是两回事。
    EventList events_; // 用于存放epoll_wait返回的所有发生的事件的文件描述符事件集
    // 【设计】内核拷贝出来的就绪事件数组。元素结构（<sys/epoll.h>，共 12 字节）：
    //        struct epoll_event { uint32_t events; epoll_data_t data; }
    //        union epoll_data  { void *ptr; int fd; uint32_t u32; uint64_t u64; }  ← 8 字节，四选一
    //        muduo 用 data.ptr 存 Channel*，所以一次拷贝就带回「谁 + 发生了什么」，零查找。
};