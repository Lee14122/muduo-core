/* ============================================================================
 * 【文件】include/Callbacks.h
 * 【一句话】把“服务器在哪些时机要回头调用用户的函数”写成 6 个类型别名，让
 *          TcpServer / TcpConnection / 用户代码共用同一份契约，而不用互相 include。
 * 【Reactor 角色】基础设施（纯编译期契约层：没有一行可执行代码，也不产生 .o）
 * 【依赖】<memory>（std::shared_ptr 模板本体）、<functional>（std::function 模板本体）；
 *        对本项目的 Buffer / TcpConnection / Timestamp 只做 class X; 前向声明，一个 .h 都不 include。
 * 【谁用它】TcpConnection.h（5 个回调成员的类型）、TcpServer.h（4 个 setter 的形参类型）、
 *        testserver.cc（经 TcpServer.h 间接拿到 ConnectionCallback / MessageCallback）；
 *        Channel.h 不用它 —— Channel 只认 std::function<void()> 这种不带业务信息的哑回调。
 * 【先修知识】① 前向声明与“不完整类型 incomplete type”能做什么、不能做什么；
 *        ② std::shared_ptr / std::function 是模板，模板实参不必是完整类型；
 *        ③ 头文件循环包含为什么必然编译失败；④ TCP 是字节流而不是消息流。
 * 【难度】★★☆☆☆   【建议耗时】15 分钟
 * 【自检】1) 为什么这里一句 class TcpConnection; 就能写出 shared_ptr<TcpConnection>，
 *           却不能写出 new TcpConnection(...)？
 *        2) ConnectionCallback 与 CloseCallback 签名一模一样，为什么要分成两个别名？
 *        3) 六个回调里，哪个是 TcpConnection::handleClose 的最后一行？哪个只在
 *           “数据堆进 outputBuffer_ 并跨过 64MB” 时才可能触发？
 * ============================================================================ */
#pragma once

// 【语法】这里 include 的只是两个标准库头：<memory> 给 std::shared_ptr 模板本体，
//   <functional> 给 std::function 模板本体。需要“完整定义”的是这两个模板自己，
//   不是它们尖括号里的业务类型 —— 这正是本文件敢只写三行 class X; 的全部原因。
#include <memory>
#include <functional>

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 本文件的核心技巧：不完整类型(incomplete type) —— muduo 头文件分层的地基      │
// └────────────────────────────────────────────────────────────────────────────┘
// 【语法】class Buffer; 这一行叫“前向声明”。写完这行，Buffer 是不完整类型：
//   编译器只登记“存在一个叫 Buffer 的 class”，不知道它占多少字节、有哪些成员。
//   不完整类型【能】做：声明指针/引用、写函数原型（形参或返回值是它的指针/引用）、
//   用 using/typedef 起别名、声明 friend、把它当模板实参传给“指针型”成员。
//   不完整类型【不能】做：定义对象（算不出大小）、继承它、访问它的成员、sizeof 它、
//   new/delete 它、调用它的任何成员函数。
//   根因：指针和引用在 64 位机器上永远占 8 字节，与所指对象的大小和内容无关，
//   所以“我只存一个地址”这类用法不需要知道对象长什么样。
// 【语法】shared_ptr<TcpConnection> 为什么允许 TcpConnection 不完整：
//   <memory> 里 shared_ptr 的定义对 T 只做两件事 —— 存一个 T* 成员、提供 T* get() const，
//   全是“指针用法”。真正要求 T 完整的是三个时刻：
//   ① shared_ptr<TcpConnection>(new TcpConnection(...))：要调构造函数，还要把
//      ~TcpConnection 编进删除器；② conn->成员()：operator-> 拿到 T* 之后要查成员；
//   ③ *conn 解引用。这三件事都发生在能看到 TcpConnection.h 的翻译单元里（本项目是
//   src/TcpServer.cc、src/TcpConnection.cc 和用户的 .cc），本文件一件都不做。
// 【语法】Timestamp 是唯一“按值出现在签名里”的项目类型（MessageCallback 第三个参数）。
//   函数类型/原型带不完整类型的按值形参是合法的 —— 只有函数【定义】和【调用】那一刻
//   才要求类型完整。这里 std::function<void(..., Timestamp)> 只是被当成一个名字用，
//   没人构造它、没人调它，libstdc++ 不必实例化任何与 sizeof(Timestamp) 有关的代码，于是编过。
// 【易错】别把这招当通用技巧：一旦在本文件里加一句 Timestamp x; 或者真去调用那个
//   std::function，立刻编译失败。规则背死：本文件只允许出现“名字”，不允许出现“动作”。
// 【设计】为什么不直接 include TcpConnection.h？会形成环：
//   TcpConnection.h → Callbacks.h（它要用 ConnectionCallback）→ TcpConnection.h。
//   有 pragma once 之后第二次展开变成空文件，于是 TcpConnection.h 里
//   “ConnectionCallback connectionCallback_;” 找不到类型 → 编译错误。
//   解法：让 Callbacks.h 待在依赖最底层（谁都能 include 它，它谁都不 include），
//   用“前向声明 + using 别名”把双向硬依赖拆成单向。这就是它单独成一个头文件的理由。
// 【对比原版】原版 muduo 的 Callbacks.h 也是“前向声明 + 别名”的同构写法（boost 时代用 typedef，
//   具体措辞与类型以原版源码为准）；本仓库可核实：本项目把 typedef 换成 using、
//   把 boost 换成 std。思路来自“接口层与实现层分离”这一通用做法。
// 【面试】“muduo 怎么解决头文件循环依赖？”→ 契约层只前向声明 + using 别名，业务层 include
//   契约层，依赖图单向无环；shared_ptr / function 的模板实参不需要完整类型。
//   class Buffer;        // 字节流双端缓冲区；本文件只用它的指针类型（MessageCallback 第二个形参）
//   class TcpConnection; // 一条 TCP 连接一个对象；本文件只用 shared_ptr<TcpConnection>
//   class Timestamp;     // 收到数据的时刻；在 MessageCallback 里按值出现，见上面那段说明
class Buffer;
class TcpConnection;
class Timestamp;

// 【设计】全库对“连接对象”的唯一称呼：一律 shared_ptr，不用 TcpConnection*。
//   原因是这条连接没有单一主人：TcpServer::connections_（mainLoop 的表）持有它、
//   Channel::tie_ 用 weak_ptr 沾着它、每个 runInLoop/queueInLoop 的 functor 按值捕获它、
//   handleClose 的栈上还有一份局部拷贝 —— 谁最后放手，谁触发析构。
//   这种“多持有者 + 寿命不定”正是 shared_ptr 的用武之地；配套机制见 TcpConnection.h
//   的 enable_shared_from_this 说明与 TcpServer.cc 的 ~TcpServer 技巧。
// 【语法】下面几个别名的形参全部写成 const TcpConnectionPtr& 而不是按值 TcpConnectionPtr：
//   按值传一次 = 引用计数原子 ++，函数返回再原子 --（一对带 lock 前缀的指令）；
//   const& 只传 8 字节地址，零原子操作。真要把寿命延长到函数之外的地方才显式拷贝
//   （TcpConnection::handleClose 里的 TcpConnectionPtr connPtr 局部量、bind 进 functor 的实参）。
// 【易错】const 修饰的是“这个智能指针本身不能改指向”，不是所指对象：
//   回调里 conn->send(...)、conn->shutdown() 照调不误（TcpConnection 的成员函数
//   几乎都不是 const 成员函数）。别被 const 吓到而去做 const_cast。
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
// 【设计】“上线/下线”通知。同一个别名服务两个时机，靠 conn->connected() 区分：
//   上线 = TcpConnection::connectEstablished 的最后一行（流程 ⑦，subLoop 线程），
//          那时 state_ 已是 kConnected，connected() 为 true；
//   下线 = TcpConnection::handleClose 里 setState(kDisconnected) 之后（流程 ⑪，subLoop），
//          以及 TcpServer 拆连接时 TcpConnection::connectDestroyed 里的补通知。
// 【易错】用户回调里不判 connected() 就分不清上线和下线 —— 两者用的是同一个函数对象。
//   testserver.cc 的 onConnection 就是这个判断的标准写法。
// 【线程】subLoop#N（连接所属 IO 线程）；setThreadNum(0) 的单线程模式就是 mainLoop。
// 【面试】“muduo 怎么发现对端断开的？”→ read 返回 0 或 epoll 上报 EPOLLHUP，
//   两条路都汇到 TcpConnection::handleClose，由它调本回调（此时 connected() 为 false）。
using ConnectionCallback = std::function<void(const TcpConnectionPtr &)>;
// 【设计】签名和 ConnectionCallback 一模一样，但语义是两回事，所以必须分两个名字：
//   ConnectionCallback 给“用户”用；CloseCallback 是库内部的回收钩子，唯一设置者是
//   TcpServer::newConnection 里的 std::bind(&TcpServer::removeConnection, this, _1)。
//   分成两个别名后，类型名本身就在提醒读者“这里挂的是资源回收，不是业务逻辑”，
//   也让编译器挡住“把回收钩子当业务回调传进去”这类手滑。
// 【流程】只在 TcpConnection::handleClose 的【最后一行】触发，它是 ⑪ 中【线程跳转 2】的起点：
//   subLoop → TcpServer::removeConnection → mainLoop 里的 connections_.erase。
// 【易错】“必须是最后一行”是硬约束：closeCallback_ 一路 erase 下去，本 TcpConnection
//   的引用计数可能当场归零并析构，之后再碰 this 的任何成员就是 use-after-free。
// 【易错】“must be the last line”这句提醒在本仓库可见于 src/TcpConnection.cc 的 handleClose 同一行行尾
//   （原版 muduo 是否同措辞，以原版源码为准）。
using CloseCallback = std::function<void(const TcpConnectionPtr &)>;
// 【设计】“这批数据真的全进内核发送缓冲区了”的通知。两个触发点：
//   ① TcpConnection::sendInLoop 里一次 write 就发完（remaining==0，流程 ⑨）；
//   ② TcpConnection::handleWrite 里把 outputBuffer_ 排空、disableWriting 之后（流程 ⑩）。
//   两处都用 loop_->queueInLoop 把回调排到本轮事件处理之后再跑，而不是就地调用 ——
//   免得用户在 sendInLoop / handleWrite 的调用栈里重入 send()，把 outputBuffer_ 的状态搅乱。
// 【流程】典型用法：“这条响应发完就关连接”（在回调里 conn->shutdown()）；
//   大报文分块发送（回调里继续 send 下一块）；统计发送耗时。
// 【易错】小报文当场发完时根本不注册 EPOLLOUT，所以 ① 那条路不走 handleWrite；
//   而 outputBuffer_ 一直排不空（对端不收、自己还在猛发）时它又永远不触发。
//   写“发完再做 X”的逻辑必须同时想到这两种情形。
// 【线程】subLoop#N（在本 loop 的 doPendingFunctors 里执行，不在 epoll 回调栈上）。
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr &)>;
// 【设计】反压(backpressure)钩子：只有数据“堆进 outputBuffer_”才可能触发。
//   highWaterMark_ 在 TcpConnection 构造函数里默认 64*1024*1024，单位字节；
//   TcpConnection::sendInLoop 发现本次要缓存的字节让待写量从“阈值以下”跨过“阈值”，
//   才 queueInLoop 通知一次，第二个实参就是跨越后的待写总字节数（oldLen + remaining）。
//   用户收到它就该停止 send（等 WriteCompleteCallback 再继续），否则内存会被自己的
//   发送数据撑爆 —— 这是 muduo 唯一的反压手段，内核发送缓冲区那一层它管不着。
// 【易错】它是“边沿触发”：只在跨越那一次通知，不是每次 send 都通知，也不帮你限流。
//   另外一次 write 就发完的小报文根本不进 outputBuffer_，这个回调一次都不会跑。
// 【线程】subLoop#N。
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr &, size_t)>;

// 【设计】“有可读数据”通知，三个参数各有理由：
//   const TcpConnectionPtr& —— 让你能在回调里 conn->send() 把数据回过去；
//   Buffer* —— 指向该连接的成员 inputBuffer_。裸指针表示“不拥有”：缓冲区跟着连接生死；
//     若做成 shared_ptr<Buffer> 就会被外部（functor、用户全局变量）拖住寿命，
//     连接的内存何时回收不再由 loop 线程说了算；
//   Timestamp —— 按值传（就是一个微秒整数，拷贝比取地址更便宜，也绝无悬垂问题），
//     值是 EventLoop::loop() 里 poller_->poll 返回的 pollReturnTime_：同一批就绪事件
//     共用一个时间戳，省掉“每收一包就 clock_gettime 一次”的系统调用开销。
// 【易错】回调返回后数据不会自动消失 —— 它一直躺在 inputBuffer_ 里，直到你调用
//   retrieve / retrieveAllAsString 推进 readerIndex；只 peek 不 retrieve 会让缓冲区
//   无限膨胀。也绝不能把这个 Buffer* 存到别处跨回调使用（连接可能在别的线程被销毁）。
// 【面试】“一次 onMessage 正好收到一行/一个包吗？”不一定：TCP 是字节流，Buffer::readFd
//   用 readv 一次读到多少取决于内核此刻攒了多少字节，粘包/拆包要用户自己的协议
//   （定长、分隔符、长度前缀）解决。原版 muduo 另提供成系列的解码器（命名以原版源码为准）；本仓库没有。
using MessageCallback = std::function<void(const TcpConnectionPtr &,
                                           Buffer *,
                                           Timestamp)>;