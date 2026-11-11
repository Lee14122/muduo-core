/* ============================================================================
 * 【文件】include/Poller.h
 * 【一句话】定义「IO 复用」这一层的抽象接口：poll() 取回本轮就绪的 channel 列表、
 *          updateChannel()/removeChannel() 维护注册表；并声明「默认用哪种实现」的静态工厂。
 * 【Reactor 角色】Demultiplexer（多路解复用器）的基类 + 注册表，是 Reactor 的心脏外壳。
 * 【依赖】<vector>（ChannelList 出参）、<unordered_map>（ChannelMap 注册表）、
 *        noncopyable.h、Timestamp.h（poll 的返回值）。对 Channel / EventLoop 只用前向声明。
 * 【谁用它】EventLoop 通过 std::unique_ptr<Poller> poller_ 持有一个具体实现（① 构造时由
 *        newDefaultPoller 工厂造出来，④ loop 每轮调 poll，Channel 的 update/remove 转发到 updateChannel/removeChannel）。
 * 【先修知识】1) 抽象基类 / 纯虚函数 / 虚析构 2) 面向接口编程（换 epoll 不改 EventLoop）
 *          3) 裸指针作为「观察引用」的非所有权语义 4) 工厂方法
 * 【难度】★★★☆☆   【建议耗时】20 分钟
 * 【自检】① Poller 为什么不 include Channel.h 而只写 class Channel; ？
 *        ② channels_ 用 fd 当 key、用裸 Channel* 当 value，这两件事各自的风险是什么？
 *        ③ hasChannel() 现在能调用吗？调了会发生什么？
 * 【口径】本文件凡【对比原版】条目里涉及原版 muduo 具体写法的话，本环境无网络、无原版源码，
 *        一律「以原版仓库为准」，不当作已核实事实；凡写「可核实：…」的都是能在本仓库
 *        直接 grep / 读码验证的事实。本文件的隐患与要点已全部就地打了标签，用 grep 【⚠问题】/【易错】/【对比原版】 即可索引。
 * =========================================================================== */

#pragma once

#include <vector>
#include <unordered_map>

#include "noncopyable.h"
#include "Timestamp.h"

class Channel;
// 【设计】同 Channel.h 对 EventLoop 的处理：这里只需要「Channel* 这个指针类型」就能声明
//        ChannelList/ChannelMap/成员函数签名，不需要它的完整定义 —— 因此 Poller 与 Channel
//        之间没有编译期循环依赖（Channel.cc 反过来 include EventLoop.h → EventLoop.h include Poller.h，
//        如果 Poller.h 又 include Channel.h，就形成 EventLoop→Poller→Channel→EventLoop 的包含环）。
class EventLoop;
// 【设计】构造参数与 ownerLoop_ 都要 EventLoop 指针，同样只需前向声明。
//        Poller 也从不解引用 loop_（只在构造时存下来），所以本头文件完全不必知道 EventLoop 长什么样。

// muduo库中多路事件分发器的核心IO复用模块
// 【设计】本类是纯抽象接口：三个 =0 的纯虚函数让它不能实例化，任何具体实现（EPollPoller、
//        假想中的 PollPoller/SelectPoller）只要继承并实现这三个函数，EventLoop 一行都不用改。
//        【Reactor 角色】分工：epoll_wait 是机制（怎么等），Poller 是策略（等谁、等完给谁）。
//        这就是「面向接口便于换 epoll/poll」的落点：库的其它部分只认 Timestamp poll(int, ChannelList*)。
// 【语法】本类没有继承 noncopyable（虽然 include 了它）：因为 Poller 是通过 unique_ptr<Poller>
//        使用的抽象基类，拷贝一个 Poller 语义上就很荒谬（epollfd_ 会复制出两个 owner），
//        【⚠问题】可核实：本文件 include 了 noncopyable.h，但 class Poller 后面【没有】继承它
//        （同目录下 Channel / EventLoop / Socket / TcpConnection 都继承了）。后果是编译器会给
//        EPollPoller 合成一份拷贝构造与拷贝赋值，两个副本共享同一个 epollfd_ 与同一张 channels_：
//        任一份析构都会 ::close(epollfd_)，另一份析构时就是 double close —— 可能把别的线程刚
//        拿到的同号 fd 误关掉。通行描述：原版继承 noncopyable 禁掉了这条路，本环境无法核实原版，
//        以原版仓库为准；但「这里少了禁拷贝」在本仓库是可读码验证的事实。
class Poller
{
public:
    using ChannelList = std::vector<Channel *>;
    // 【设计】用 vector<Channel*> 而不是 set/list：① poll 只关心「有哪些就绪」，不要求有序；
    //        ② vector 连续内存、push_back 均摊 O(1)，EventLoop 每轮 clear() 后复用同一块容量
    //        （见 EventLoop::loop 里 activeChannels_.clear() 不会释放内存）—— 稳态下零分配。
    //        ③ 存指针不存对象：Channel 由业务对象持有，这里只是「本轮就绪名单」。
    // 【易错】EventLoop.h 里还有一个同名 ChannelList（各自 private 作用域的别名），类型相同但不是同一个别名。

    Poller(EventLoop *loop);
    // 【线程】在 EventLoop 的构造线程里被调用：mainLoop→main 线程（①），subLoop#N→该 IO 线程。
    // 【设计】唯一职责是把 ownerLoop_ 记下来（EPollPoller 的构造会先调 Poller(loop) 再开 epollfd）。
    virtual ~Poller() = default;
    // 【语法】基类析构必须 virtual：EventLoop 用 unique_ptr<Poller>（静态类型是基类）持有派生对象，
    //        若析构非虚，delete 基类指针是未定义行为 —— 派生类析构不跑，EPollPoller 的
    //        ::close(epollfd_) 就不会执行，直接漏一个 fd。
    // 【语法】= default 而不是写空体 {}：显式告知编译器「什么都不用做」，仍是 inline 且
    //        不阻碍移动构造/移动赋值的合成。
    // 【设计】protected 还是 public？这里 public，因此 unique_ptr<Poller> 正常销毁。

    // 给所有IO复用保留统一的接口
    // 【设计】以下三个纯虚函数就是「IO 复用的最小完备接口」：等事件、改注册、删注册。
    //        注释里的「保留」二字是关键：EventLoop 只依赖这三个签名，实现可插拔。
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;
    // 【流程】④ 的心脏：EventLoop::loop() 每轮第一件事就是它，整个线程绝大部分时间阻塞在这里。
    // 【线程】ownerLoop 线程（mainLoop 阻塞在 mainLoop 的 poll，subLoop#N 阻塞在自己的 poll）。
    // 【设计】出参用指针而非返回 vector：调用方（EventLoop）传自己的 activeChannels_ 进来复用容量；
    //        返回值只留一个 Timestamp（本轮就绪的统一时间戳），避免「时间+列表」打包返回。
    // 【易错】timeoutMs 单位是毫秒；EventLoop 传的是 kPollTimeMs=10000（10 秒），
    //        它同时是「quit() 最坏延迟」与「doPendingFunctors 的兜底唤醒周期」——见 A4 的【⚠问题】。
    //        另外 poll 必须把就绪 channel 的 revents_ 填好再放进 activeChannels（见 fillActiveChannels）。
    virtual void updateChannel(Channel *channel) = 0;
    // 【流程】Channel::update() → EventLoop::updateChannel() → 这里（第三层）。
    //        ③（listen 后 enableReading）、⑦（connectEstablished 里 enableReading）、
    //        ⑨（enableWriting）、⑩（disableWriting）、⑪（disableAll）都会打到这里。
    // 【线程】ownerLoop 线程。【设计】语义是「让注册表/内核与 channel->events() 保持一致」，
    //        至于是 ADD/MOD/DEL 由实现自己按 index_ 决定 —— 接口层不规定 epoll 细节。
    virtual void removeChannel(Channel *channel) = 0;
    // 【流程】Channel::remove() → EventLoop::removeChannel() → 这里；⑪ connectDestroyed 的最后一步。
    // 【设计】与 updateChannel 分成两个接口（而不是让 updateChannel 看 isNoneEvent 自己判断），
    //        是为了区分「我不关心事件了」(DEL 但保留登记) 与「这个对象要没了」(erase 彻底除名)。

    // 判断参数channel是否在当前的Poller当中
    // 【⚠问题】下面这个 hasChannel 的【定义】在 src/Poller.cc 里被整段注释掉了，声明还留在这里：
    //        → 现在只要有任何代码写 poller->hasChannel(ch)，编译能过（声明在、类型全），
    //          链接期报 undefined reference to Poller::hasChannel，是个「定时炸弹」式接口。
    //        EventLoop.h 里的 bool hasChannel(Channel*) 也是同样情况（定义同样被注掉，见 A4）。
    //        通行描述：原版这两个函数都有实现并被 assert 使用；本环境无法核实原版源码，以原版仓库为准。
    //        可核实的是：本仓库 Poller.cc 与 EventLoop.cc 里这两处定义都被注释掉了，只剩声明。
    bool hasChannel(Channel *channel) const;
    // 【语法】const 成员函数：只读 channels_，不改 Poller 状态。
    // 【易错】它「查的是注册表」，不是「查内核 interest list」—— kDeleted 状态的 channel
    //        仍能在 channels_ 里查到，但那时它已经不在 epoll 的 interest list 里了。

    // EventLoop可以通过该接口获取默认的IO复用的具体实现
    // 【设计】静态工厂（工厂方法模式）：把「选哪种 IO 复用」这个决策从 EventLoop 里剥出去。
    //        EventLoop 构造里只写 Poller::newDefaultPoller(this)，不需要 include EPollPoller.h。
    static Poller *newDefaultPoller(EventLoop *loop);
    // 【流程】① / ③ 里 EventLoop 构造函数的成员初始化列表唯一调用点。
    // 【线程】EventLoop 所在线程（main 线程或某 subLoop 线程）。
    // 【设计】定义不在本类的 .cc（Poller.cc）而在 src/DefaultPoller.cc —— 让「默认策略」独立成
    //        一个可替换的编译单元；也避免 Poller.cc 依赖 EPollPoller.h。
    // 【⚠问题】返回裸指针：所有权立刻交给 EventLoop 的 unique_ptr<Poller>。中间若有人接不住就泄漏；
    //        工厂函数还可能返回 nullptr（DefaultPoller 里设了 MUDUO_USE_POLL 环境变量那条分支），
    //        EventLoop 没有判空 → 之后第一次 poll() 就是空指针解引用。
    // 【对比原版】通行描述：原版会按平台宏挑实现、poll 分支返回 PollPoller，本环境无原版源码，以原版为准；
    //        可核实的是：本仓库根本没有 PollPoller 这个文件（include/ 与 src/ 里只有 Poller.h、
    //        EPollPoller.h/.cc、DefaultPoller.cc），所以该分支只能返回 nullptr（见 DefaultPoller.cc）。

protected:
    // 【设计】protected（不是 private）：让派生类 EPollPoller 直接读写 channels_，
    //        同时又对外隐藏 —— 「注册表是 IO 复用实现的内部事，业务不可见」这条边界正好落在类层次上。
    // map的key:sockfd value:sockfd所属的channel通道类型
    // 【设计】为什么以 fd（int）为 key：
    //        ① epoll_ctl 的入参就是 fd，从内核事件里拿到的 data.ptr 也是 Channel*，
    //           真正需要「按 fd 找人」的场合是 removeChannel/查重/调试；
    //        ② int 作 key 哈希成本低、无内存分配，比 hash<Channel*> 更稳（指针哈希易受地址分布影响）；
    //        ③ 语义上 fd 才是「操作系统层面的唯一身份」，一个 fd 在同一 Poller 里只能有一个 channel。
    // 【设计】为什么 value 是裸 Channel* 而不持有所有权：
    //        Channel 由创建它的业务对象用 unique_ptr/成员持有（Acceptor、TcpConnection、EventLoop）。
    //        Poller 只是「登记处」：它要能在 Channel 析构前被通知（removeChannel），
    //        但它若持 shared_ptr 就会把 Channel 的生命周期无限延长（fd 关了、对象还在表里），
    //        并且与 TcpConnection↔Channel 的所有权链打架。这里是典型的「引用不拥有」的裸指针用法。
    // 【易错】因此 channels_ 里存的是「可能变脏」的观察指针：任何绕过 removeChannel 的 Channel
    //        析构都会让表里留下悬垂指针，下一次 poll 用 data.ptr 之外再查这张表就会踩到。
    // 【对比原版】通行描述：原版曾用 std::map、后来换成 unordered_map；本环境无法核实，以原版仓库为准。
    //        选型理由可独立成立：这里只需要「按 fd 精确查/删」，从不需要有序遍历，也从不需要区间
    //        查询，所以 unordered_map 的 O(1) 严格优于 map 的 O(log n)，且没有顺序依赖可以松开。
    using ChannelMap = std::unordered_map<int, Channel *>;
    ChannelMap channels_;
    // 【设计】生命周期：EPollPoller::updateChannel 在 kNew 时插入，removeChannel 时 erase；
    //        kDeleted 状态下不 erase（这是 updateChannel 那个空 else 分支能成立的前提）。

private:
    EventLoop *ownerLoop_; // 定义Poller所属的事件循环EventLoop
    // 【设计】记录归属，语义是「本 Poller 只能被这个 loop 线程使用」。
    // 【对比原版】通行描述：原版提供 ownerLoop() 访问器并配合 assertInLoop 做线程校验，本环境无法核实，
    //        以原版仓库为准。可核实：本文件既没有该访问器，也没有任何一处读取 ownerLoop_。
    // 【⚠问题】本项目既没有访问器，也没有任何一处读取 ownerLoop_（只写不读）：
    //        也就是说「跨线程操作 channel/Poller」这道防线在 Poller 层是空的。
    //        它唯一的作用是在构造里把参数存住 —— 留着是教学用意的「本该有的样子」。
};