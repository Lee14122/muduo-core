/* ============================================================================
 * 【文件】src/DefaultPoller.cc
 * 【一句话】实现 Poller 的静态工厂 newDefaultPoller：读一个环境变量，决定这次 EventLoop 用哪套
 *          IO 复用实现（epoll 或 —— 名义上的 —— poll），并把新建对象的所有权交给调用方。
 * 【Reactor 角色】基础设施 / 简单工厂（工厂方法），不属于 Reactor 主循环的三段（Loop/Demux/Handler）。
 * 【依赖】<stdlib.h>（::getenv）、"Poller.h"（要实现的静态成员函数所属类）、"EPollPoller.h"（要 new 的具体类）。
 *        注意它 include 的是【具体实现】的头文件，而 EventLoop.cc 只 include "Poller.h" —— 这就是工厂的作用：
 *        把「依赖哪个实现」这件事集中到一个编译单元里。
 * 【谁用它】只在 EventLoop 的构造函数初始化列表里被调一次：poller_(Poller::newDefaultPoller(this))，
 *        时机是 ①（主线程建 baseLoop）与 ③（每个 IO 线程建 subLoop#N）。
 * 【先修知识】1) 静态成员函数可以在类的任意 .cc 里定义（本项目特意放到单独一个文件）
 *          2) 环境变量与进程环境块 3) 裸返回值 = 所有权转移
 * 【难度】★☆☆☆☆   【建议耗时】10 分钟（但【⚠问题】很重要，务必读）
 * 【自检】① 设了 MUDUO_USE_POLL 环境变量后，进程会在哪一行崩？为什么？
 *        ② getenv 的返回值有哪三种情况？空字符串算哪一种？
 *        ③ 为什么这个工厂单独占一个 .cc，而不写进 Poller.cc？
 * 【口径】本文件凡【对比原版】条目里涉及原版 muduo 具体写法的话，本环境无网络、无原版源码，
 *        一律「以原版仓库为准」，不当作已核实事实；凡写「可核实：…」的都是能在本仓库
 *        直接 grep / 读码验证的事实。本文件的隐患与要点已全部就地打了标签，用 grep 【⚠问题】/【易错】/【对比原版】 即可索引。
 * =========================================================================== */

#include <stdlib.h>
// 【语法】用 C 头 stdlib.h 而不是 <cstdlib>：两者都可用，前者把名字同时放进全局命名空间，
//        所以下面写 ::getenv 一定找得到（换成 <cstdlib> 时严格讲应写 std::getenv）。

#include "Poller.h"
#include "EPollPoller.h"
// 【设计】全库只有这个文件 + EPollPoller.cc 自己知道「默认实现是 epoll」。
//        EventLoop.cc 只 include "Poller.h"，换成 select/poll/devpoll 时上层零改动。

// 【设计】静态成员函数 + 返回基类指针 = 简单工厂：调用方（EventLoop）拿到的是 Poller*，
//        之后所有调用（poll/updateChannel/removeChannel）都靠虚表派发到具体实现。
//        为什么单独一个 .cc 而不调到 Poller.cc 里：① 让「选实现」这个决策可被单独替换/裁剪
//        （嵌入式裁剪时删掉这个文件换一个即可）；② Poller.cc 因此不必依赖 EPollPoller.h，
//        避免抽象层 include 具体层（否则 Poller 与 EPollPoller 的包含关系会绕起来）。
// 【线程】EventLoop 所在线程：mainLoop 在 main 线程（①），每个 subLoop#N 在自己线程里各调一次（③）
//        —— 所以每个 loop 有独立的一个 EPollPoller 实例（各自一个 epollfd_）。
// 【⚠问题】返回裸指针：所有权立即转移给调用方，EventLoop 用 std::unique_ptr<Poller> 接住；
//        如果哪天有人写 Poller* p = Poller::newDefaultPoller(loop); 而忘了接管，就是内存 + epollfd 泄漏。
// 【易错】这里的 loop 参数只是「存起来给子类用」，工厂本身不做任何注册动作。
// 【流程】①（主线程构造 baseLoop）与 ③（EventLoopThread 起 subLoop#N 时构造 EventLoop）各调一次；
//        除此之外全库没有别的调用点（可核实：grep newDefaultPoller 只命中 EventLoop 构造与本定义）。
Poller *Poller::newDefaultPoller(EventLoop *loop)
{
    if (::getenv("MUDUO_USE_POLL"))
    // 【系统调用】char *getenv(const char *name)：
    //   · 变量【不存在】→ 返回 NULL（空指针）—— 走 else，用 epoll。
    //   · 变量【存在】→ 返回指向进程环境块内部字符串的指针（不是拷贝！）：
    //     绝不能 free 它，也不要长期保存（一旦调用 setenv/putenv，之前拿到的指针可能失效）；
    //     在本项目里我们只把返回值当布尔用，立刻丢弃，安全。
    //   · 变量被设成【空字符串】（export MUDUO_USE_POLL=）→ 仍然返回非 NULL（指向一个 '\0'），
    //     于是 if 判真 —— 【易错】这是个真实的坑：以为「设成空就等于没设」的人会直接踩进下面那条 nullptr 分支。
    //   · 查找是线性遍历 environ，只在构造期调用一次，无性能问题；不阻塞、不失败、不设 errno。
    // 【设计】用环境变量而不是编译期宏来选 IO 复用：便于在测试里现场切换实现（本项目没实现切换目标，见下）。
    {
        return nullptr; // 生成poll的实例
        // 【⚠问题】★重点问题★ 原注释说「生成poll的实例」，但这里【返回 nullptr】——
        //   · 原因：本项目没有实现 PollPoller（poll(2) 版的 Poller），作者用 nullptr 占位，避免引入一半的代码。
        //   · 后果（可精确到调用链）：EventLoop 构造里 poller_(Poller::newDefaultPoller(this)) 会把这个
        //     nullptr 存进 std::unique_ptr<Poller>（unique_ptr 允许接空指针，构造不报错，也不走判空分支）；
        //     紧接着 EventLoop 构造函数体最后两句 wakeupChannel_->setReadCallback(...) / enableReading()
        //     会调 loop_->updateChannel(this) → poller_->updateChannel(channel) —— 对空 Poller* 调虚函数，
        //     虚表指针取自地址 0，段错误（SIGSEGV）在【启动瞬间】就发生，连一条日志都来不及打。
        //   · 正确做法：要么在这里 return new PollPoller(loop);（先实现它），
        //     要么直接 return nullptr 之前 LOG_FATAL 明确告诉用户「poll 版未实现，请勿设置该变量」，
        //     或者干脆去掉这个分支。现状是「静默把炸弹交给上层」。
        // 【易错】MUDUO_USE_POLL=0（想表达「关掉」）同样会命中这个分支 —— 环境变量只看存不存在，不看值。
        // 【对比原版】通行描述：原版这一支返回 new PollPoller(loop)。本环境无原版源码，以原版仓库为准；
        //        可核实的是本仓库【没有】任何 PollPoller 实现（include/ 与 src/ 下只有 Poller.h、
        //        EPollPoller.h/.cc、DefaultPoller.cc），也没有任何 #ifdef 平台分档 —— 所以这里只能返回 nullptr。
    }
    else
    {
        return new EPollPoller(loop); // 生成epoll的实例
        // 【设计】默认路径（没设环境变量）→ 每个 EventLoop 一个 EPollPoller，即一个 epoll 实例。
        // 【流程】①/③ 构造期一次性完成；本行内部会立刻 epoll_create1(EPOLL_CLOEXEC)，
        //        也就是说「进程一启动就占用 N+1 个 fd」（N 个 loop 各一个 epollfd）。
        //        连接数很大时要记得 RLIMIT_NOFILE 要留出这些 epollfd 的余量（【易错】常被忽略）。
        // 【语法】new 之后不在这里 delete：所有权随返回值转移给 unique_ptr<Poller>；
        //        若 EPollPoller 构造中途抛异常，new 出来的内存会被编译器自动回收（不会泄漏），
        //        但本项目的构造用 LOG_FATAL+exit 而不是抛异常，所以这条兜底路径实际走不到。
    }
}