/* ============================================================================
 * 【文件】src/Poller.cc
 * 【一句话】实现 IO 复用抽象基类的唯一一点公共代码：把 ownerLoop 记下来；
 *          另外保留一份【被注释掉的】hasChannel 实现，作为接口与实现分离的活教材。
 * 【Reactor 角色】Demultiplexer 基类的骨架（几乎空 —— 真正的活全在 EPollPoller.cc）。
 * 【依赖】"Poller.h"（本类）、"Channel.h"（只为下面被注释掉的 hasChannel 里 channel->fd() 而 include）。
 * 【谁用它】构造由 EPollPoller(loop) 的初始化列表里 Poller(loop) 触发（① / ③ 创建 EventLoop 时）。
 * 【先修知识】1) 纯虚基类仍可有自己的非虚成员函数与构造实现 2) 声明有、定义无 → 链接错误
 * 【难度】★☆☆☆☆   【建议耗时】5 分钟
 * 【自检】① 现在有人调用 Poller::hasChannel 会在哪一阶段失败？报什么？
 *        ② 本文件里为什么还要 include Channel.h（一个似乎没用的依赖）？
 * 【口径】本文件凡【对比原版】条目里涉及原版 muduo 具体写法的话，本环境无网络、无原版源码，
 *        一律「以原版仓库为准」，不当作已核实事实；凡写「可核实：…」的都是能在本仓库
 *        直接 grep / 读码验证的事实。本文件的隐患与要点已全部就地打了标签，用 grep 【⚠问题】/【易错】/【对比原版】 即可索引。
 * =========================================================================== */

#include "Poller.h"
#include "Channel.h"
// 【设计】注意：当前生效的代码（只有构造函数）根本不需要 Channel 的完整类型 ——
//        这个 include 是为下面那段被注释掉的 hasChannel 里 channel->fd() 留着的。
// 【易错】看代码时容易误以为 Poller 依赖 Channel 的实现细节；抽象接口层只依赖 Channel*。

// 【流程】① / ③：EventLoop 构造 → newDefaultPoller → new EPollPoller(loop) → 先执行本构造（基类优先）。
// 【线程】EventLoop 所在线程：mainLoop 在 main 线程，subLoop#N 在对应 IO 线程。
Poller::Poller(EventLoop *loop)
    : ownerLoop_(loop)
    // 【语法】基类成员必须在派生类的初始化列表里通过基类构造传进去：
    //        EPollPoller 写的是 Poller(loop) 而不是 ownerLoop_(loop)（私有成员不可直接初始化）。
    // 【⚠问题】存了但全库没有任何地方读它（详见 Poller.h 里的【⚠问题】）：线程归属检查缺失。
{
}

// ==========================================================================================
// 【⚠问题】以下 5 行是【被注释掉的代码】，以下保持原样不动，只在此块外面说明：
//   · Poller.h 里的声明 bool hasChannel(Channel*) const; 仍然存在，但这里没有定义。
//   · 后果：一旦有代码调用它，编译通过、链接期报 undefined reference to Poller::hasChannel
//     （注意不是「未定义行为」也不是运行期错误，是 ld 阶段的硬错误，但只有真的调用才暴露）。
//   · EventLoop.h 里的 bool hasChannel(Channel*) 定义同样被注释掉，两处是一对：
//     EventLoop::hasChannel 的实现体是 return poller_->hasChannel(channel); —— 两层都是空声明。
//   · 为什么会这样：本仓库自称是教学精简版，推测精简时把「当前没人用的校验」整块注掉，
//     正确做法应该是声明也一起删掉；留着声明是隐患。
//   · 想恢复它：把下面 5 行的注释符去掉即可，实现是正确的（按 fd 查表 + 比对指针同一性）。
// ==========================================================================================
// 【设计】顺带读一下这段被注释的实现本身：it->second == channel 这个「指针相等」判断不可省 ——
//        只 find 成功不代表查到的是同一个对象：fd 号会被内核回收复用（close 后 open 常拿到同一个数），
//        若旧 channel 已 remove、新 channel 用同一个 fd 注册，find 会命中新值；
//        比指针可同时拦住「fd 在但对象不同」的情况（这是【易错】的高频考点）。
// 【对比原版】通行描述：原版用它配合 assert 做一致性检查；本环境无法核实原版源码，以原版仓库为准。

// bool Poller::hasChannel(Channel *channel) const
// {
//     auto it = channels_.find(channel->fd());
//     return it != channels_.end() && it->second == channel;
// }