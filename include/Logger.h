/* ============================================================================
 * 【文件】include/Logger.h
 * 【一句话】声明全库唯一的日志出口：4 个 LOG_XXX 宏 + Logger 单例类 + LogLevel 枚举。
 * 【Reactor 角色】基础设施（不是 Acceptor/Demultiplexer/Loop/Handler 中任何一个）。
 *                   它是排障时的眼睛：从 ① 构造 EventLoop 到 ⑫ 退出，每一步的输出都走这里。
 * 【依赖】<string>（log 的形参类型）；"noncopyable.h"（声明"这个单例不许被复制"）。
 * 【谁用它】src 下 9 个 .cc（Socket/Acceptor/EPollPoller/Channel/EventLoop/TcpServer/
 *           TcpConnection/EventLoopThreadPool/Logger 自己）+ example/testserver.cc，共 10 个文件；
 *           其余 8 个 .cc（Buffer、CurrentThread、DefaultPoller、EventLoopThread、
 *           InetAddress、Poller、Thread、Timestamp）没有直接 include 它（grep 'Logger.h' 可验证）。
 *           具体时机：bind/listen 失败、eventfd/epoll_create 失败、accept 失败、
 *           Channel 事件到达、EventLoop 起停、TcpConnection 构造析构与读写、新连接与删连接、
 *           用户回调 onConnection/onMessage 里自己打 —— 覆盖 ①→⑫。
 * 【关于"原版"】本环境不能联网（github/raw 域名均不可达），仓库里也没有原版副本，
 *             所以下文所有【对比原版】条目都写成"通行描述"，只用来对照设计思路，
 *             接口名与写法一律以原版仓库为准；凡是标了"实测 / grep / man"的结论，才是本机可复现的事实。
 * 【先修知识】1) 预处理器：#define 与行尾续行反斜杠 \；
 *             2) 可变参数宏 __VA_ARGS__；
 *             3) do { ... } while (0) 这个惯例在防什么；
 *             4) 无作用域 enum 的隐式取值；函数内 static 变量（实现在 src/Logger.cc）。
 * 【难度】★★☆☆☆   【建议耗时】25 分钟
 * 【自检】1) 把宏体直接写成 { 三条语句; } 而不用 do-while(0)，什么调用点会编译失败？报什么错？
 *         2) ##__VA_ARGS__ 的 ## 去掉，src/Socket.cc 里哪一行会编不过？
 *         3) 本项目里 LOG_DEBUG("x") 会输出吗？i++ 写在它的参数里会执行吗？
 *         4) logLevel_ 能拦住某一级日志不输出吗？（答：不能，它只挑前缀字符串）
 *         5) LOG_FATAL 打完这条之后进程还在吗？局部对象的析构函数跑了吗？
 * ========================================================================== */
#pragma once

#include <string>

#include "noncopyable.h"

// 【依赖】这个头文件只用到了三种外部设施：std::string（<string> 里）、
//         snprintf（<cstdio>/<stdio.h>）、exit（<cstdlib>/<stdlib.h>），
//         但后面两个都没有显式 include，靠 <string> 的传递包含（transitive include）侥幸拿到。
//         【易错】现在 GCC 的 libstdc++ 能编过（实测 #include "Logger.h" 的 TU 单独编译成功），
//         但换 libc++ 或收紧标准库头文件依赖时，snprintf/exit 会突然"未声明"。
//         规范做法是自己把 <cstdio>、<cstdlib> 写上 —— 头文件应当"自给自足"，
//         不能赌别人的头文件里带了什么。
//
// 【设计】下面 4 个宏是同一个模板抄了 4 遍，先把共性一次讲完（各宏上方只补各自特有的问题）。
//         注释统一写在 #define 上方，绝不写进宏体：宏体每行末尾都有续行反斜杠 \，
//         在 \ 之前插 // 注释会把这一行连同注释一起当成宏内容，插块注释同样会破坏 token 流，
//         而且 \ 必须是行内最后一个字符，注释里出现反斜杠还会引发下一行被"意外续上"。
//         —— 宏体内部不能插注释（续行反斜杠一断，整条宏就废了），这是语言规则本身决定的。
//
// 【语法】为什么要 do { ... } while (0)：
//         宏体是"多条语句"，而 if / else / for / while 语法上只能挂"一条语句"。
//         若宏写成 { a(); b(); c(); }，调用点
//             if (ok) LOG_INFO("a"); else LOG_INFO("b");
//         展开成 if (ok) { ... }; else { ... }; —— 那个多余的分号是一条空语句，
//         else 就找不到它的 if 了，编译器直接报 error: 'else' without a previous 'if'（实测复现）。
//         包成 do{...}while(0) 后整段是一个复合语句且只执行一次，调用点尾部的分号正好
//         被消化成 while(0) 之后的空语句，语法严丝合缝。
//         为什么不用别的招：单独 { } 有上面这个分号问题；({ a(); b(); }) 语句表达式是 GNU 扩展且吞不掉分号；
//         if (...) {...} else 会引入"宏体里的 else 吃掉调用者 else"的新悬垂问题。
//         do-while(0) 同时满足：①只执行一次 ②能吞尾部分号 ③内部可声明局部变量 ④宏里的 break 不会跳到外层循环。
//
// 【语法】##__VA_ARGS__ 里的 ## 是"token 粘贴"，GCC/Clang 扩展：当可变参数个数为 0 时，
//         它把紧跟在 ## 前的那个逗号一起删掉，于是 snprintf(buf, 1024, fmt, ) 变成 snprintf(buf, 1024, fmt)。
//         这不是可选的写法：src/Socket.cc 里 LOG_ERROR("shutdownWrite error") 就是一个可变参数都没有的调用，
//         去掉 ## 立刻编不过（error: expected expression before ')' token）。
//         C++20 的等价标准写法是 __VA_OPT__(,) __VA_ARGS__；本库用 GNU 扩展，MSVC 上不认（要在 CMakeLists.txt 里加 /Zc:preprocessor 或换写法）。
//
// 【语法】logmsgFormat 是必须存在的具名参数（所以宏至少要有 1 个实参），__VA_ARGS__ 代表"其余全部"。
//         好处：printf 风格在 -Wall 下由编译器检查格式与实参是否匹配
//         （实测 LOG_INFO("str=%s", x) 会给出 format '%s' expects argument of type 'char*' 警告），
//         （原版走 operator<< 流式路线、没有格式串，也就没有这类 -Wformat 检查；以原版源码为准）
//
// 【设计】三步固定套路：拿单例 → 设级别 → 格式化进栈上 char buf[1024] → 交给 log()。
//         buf 在"调用者的栈帧"上，1KB × 一次调用，不跨调用保留；
//         snprintf 保证不越界写，但超过 1023 字节会被静默截断（返回值是"想写的长度"，宏体没人看它）。
//         【易错】容量 1024 在宏体里写死了两处（数组定义和 snprintf 第二个参数），改一处就得改两处；
//         通行描述：原版把长度写成 sizeof(buf)，一处定义（以原版源码为准）。
//         【易错】4 份宏体是复制粘贴的，任何修改要同步 4 处；比如 exit(-1) 只出现在 LOG_FATAL 那一个宏里，
//         LOG_ERROR 再严重也不会退出进程 —— 想加"错误累积到 N 次自动退出"这类策略，这个结构会让你改很久。
// LOG_INFO("%s %d", arg1, arg2)
#define LOG_INFO(logmsgFormat, ...)                       \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(INFO);                         \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)

// 【设计】LOG_ERROR 的实现与 LOG_INFO 逐字相同，唯一区别是 setLogLevel(ERROR)：
//         级别只决定输出行开头的 [ERROR] / [INFO] 字样，完全不决定"打不打"。
//         【流程】④→⑪ 的运行期错误都用它：accept 返回 -1（src/Acceptor.cc）、
//         epoll_ctl del 失败与 poll 出错（src/EPollPoller.cc）、shutdownWrite 失败（src/Socket.cc）、
//         读写返回 -1 与 fd 已断（src/TcpConnection.cc）、eventfd 读写字节数不是 8（src/EventLoop.cc）。
//         【线程】mainLoop 与每个 subLoop#N 都会调用，还可能和用户回调线程叠加。
//         【易错】用 LOG_ERROR 而不是 LOG_FATAL 的地方，都是"这条连接/这次操作可以失败但进程要继续活"；
//         读代码时可以按"logLevel_ 是 1 还是 2"反推作者的容错设计意图。
#define LOG_ERROR(logmsgFormat, ...)                      \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(ERROR);                        \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)

// 【⚠问题】这个宏比前两个多一句 exit(-1)，它是整个日志系统最重的一处退化，逐条看清：
//         ① 栈回退不会发生：实测局部对象的析构函数一个都不跑（atexit 注册的函数会跑）。
//            在这个框架里意味着 EventLoop 析构、TcpConnection 引用计数递减、
//            Thread 的 join/detach、shared_ptr 的自定义删除器全部被跳过。
//         ② 不会生成 core 文件：exit 是"正常终止"路径，不是信号崩溃，
//            于是 gdb 里没有任何调用栈可看，只能靠这一行日志本身定位问题。
//         ③ -1 作为退出码传给 shell 时按低 8 位取，实测 shell 看到的是 255，
//            与"用户传参错误返回 1"这类约定混在一起，运维脚本无法区分 fatal 的种类。
//            惯例是 abort() → SIGABRT → 退出码 134（128+6）并留 core。
//         ④ 会不会丢这条日志？常见的说法是"exit 不刷 std::cout 缓冲所以丢日志"，
//            实测这条不成立：exit() 会经 ios_base::Init 的析构刷新 cout，
//            把 stdout 重定向到全缓冲的文件时这一行照样完整写出来了。
//            真正可能丢/坏的场景是：以后把日志后端换成自己管理的 FILE *、ofstream 或内存缓冲（标准库不保证帮你刷），
//            或者另一个线程正在同时写 cout —— 本文件没有任何锁，交错会毁掉这一行的可读性。
//         【对比原版】通行描述：原版在 Fatal 级别是刷完缓冲再 abort()（信号终止，配合 ulimit -c 能留 core 给 gdb），
//            并且提供过 fatal 回调一类钩子让用户在退出前打印调用栈；函数名与细节以原版仓库为准。
//         【流程/线程】本库的 LOG_FATAL 调用点：src/EventLoop.cc 的 eventfd 创建（①）、
//            src/Socket.cc 的 bind/listen（②③）、src/Acceptor.cc 的 socket 创建失败（②）、
//            src/EPollPoller.cc 的 epoll_create（②）—— 这些都在启动阶段，进程还没接受任何连接，破坏面小，
//            这是"看起来没事"的原因；但 src/EPollPoller.cc 的 epoll_ctl add/mod 失败（⑥ 建连接时）
//            和 src/TcpConnection.cc/TcpServer.cc 的 mainLoop 为空（⑥）发生在运行期、
//            可能在 subLoop#N 上，一旦触发就是把整个进程连同其它已建立的连接一起干掉，
//            而不是只断这一条连接 —— 这是本框架"单连接故障升级为全站故障"的隐患点。
//         【易错】调用者写 LOG_FATAL(...) 之后可以继续写代码，编译器不会提示"这函数不会返回"，
//            因为宏的"不会返回"这件事不体现在任何函数签名上（通行描述：原版的终止动作放在析构里，同样不让编译器知道 noreturn）。
#define LOG_FATAL(logmsgFormat, ...)                      \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(FATAL);                        \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
        exit(-1);                                         \
    } while (0)

// 【设计】DEBUG 走的是编译期裁剪（#ifdef），不是运行期开关，两种开关的差别要分清：
//         没定义 MUDEBUG 时走 #else 分支，LOG_DEBUG 展开成"空"，
//         连 Logger::instance()、char buf[1024]、snprintf 全都不存在，一条指令都不生成 → 零成本；
//         代价是发布版和调试版是两份不同的代码，出问题时无日志可查。
//         【⚠问题】实测本仓库三处 CMakeLists.txt（顶层/src/example）都没有 -DMUDEBUG，
//         grep 全仓库 MUDEBUG 只命中这一行 #ifdef，所以 LOG_DEBUG 在整个项目里恒等于空宏：
//         src/EventLoop.cc 的 "EventLoop created %p in thread %d" 与
//         src/EPollPoller.cc 的 "%s timeout!" 这两条目前一条都不会输出。
//         【对比原版】通行描述：原版把"编译期裁剪"留给 TRACE 一级（与 NDEBUG 绑定），
//         其余级别都是运行期先比较阈值再决定是否格式化，
//         所以线上可以临时把级别调低复现问题，不必重新编译（宏体写法与级别名以原版仓库为准）。
//         【对比原版】通行描述：原版的日志行自带文件名/行号/函数名（作为构造参数传进去，以原版源码为准），
//         本项目要打印位置只能像 src/Acceptor.cc 那样手写 __FILE__, __FUNCTION__, __LINE__ 当参数。
//         想打开本项目的 DEBUG：g++ ... -DMUDEBUG，或在 CMakeLists.txt 里
//         target_compile_definitions(muduo_core PRIVATE MUDEBUG)（实测加了这个开关本文件能正常编译）。
//         【易错】"用 if/else 单语句写 LOG_DEBUG" 是安全的（空宏 + 分号 = 空语句），
//         但反过来别指望它做点什么：LOG_DEBUG("%d", i++) 在未开 MUDEBUG 时 i++ 也不执行
//         （实测 i 保持 0）—— 任何有副作用的表达式都绝不能写进日志参数，
//         否则换一次编译选项程序行为就变了，这类 bug 极难定位。
//         【易错】LOG_DEBUG() 一个实参都不传，属于 GNU 扩展（-pedantic 会警告
//         ISO C++11 requires at least one argument for the '...' in a variadic macro），
//         因为具名参数 logmsgFormat 必须占一个；LOG_DEBUG("xxx") 才是合法写法。
#ifdef MUDEBUG
#define LOG_DEBUG(logmsgFormat, ...)                      \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(DEBUG);                        \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)
#else
#define LOG_DEBUG(logmsgFormat, ...)
#endif

// 【语法】这是无作用域枚举（unscoped enum），等价于在 Logger.h 的全局作用域里定义了
//         4 个整数常量：INFO=0、ERROR=1、FATAL=2、DEBUG=3（不写显式值就从前往后每次 +1）。
//         【易错】它们会泄漏到每个 #include "Logger.h" 的文件里：任何同名宏 DEBUG / ERROR
//         （很多第三方库都有）都会在这里打架，编译时报一串莫名其妙的错。
//         现代 C++ 的解法是 enum class LogLevel : uint8_t { ... }（有作用域、不做隐式转换），
//         本项目沿用 C 风格枚举是因为教学版想少讲一个概念。
//         【语法】enum → int 是隐式转换（所以宏里 setLogLevel(INFO) 能直接传），
//         int → enum 才需要显式转换；正因为 setLogLevel 收的是 int、logLevel_ 存的也是 int，
//         这条链路完全没有类型保护：setLogLevel(99) 合法，实测走到 switch 的 default，
//         输出行连 [XXX] 前缀都没有（前缀是空串），只有看日志格式不对才发现。
//         【设计】级别顺序按"常用程度"排（FATAL 排在 DEBUG 前面），不按严重程度排，
//         所以 logLevel_ 不能拿来做"≥ 某阈值才打印"的比较语义 —— 数值大小在这里没有意义。
//         【对比原版】通行描述：原版的级别枚举按严重程度递增排列，并留一个 NUM_LEVELS 当数组边界，
//         所以它的第一件事能写成"级别低于阈值就 return"，前缀字符串来自一张表而不是手写 case
//         （枚举成员名、阈值变量名以原版仓库为准）。
// 定义日志的级别 INFO ERROR FATAL DEBUG
enum LogLevel
{
    INFO,  // 普通信息
    ERROR, // 错误信息
    FATAL, // core dump信息
    DEBUG, // 调试信息
};

// 输出一个日志类

// 【设计】Logger 是全局单例：整个进程一个对象，唯一的成员就是那个 int。
//         【线程】它"不属于任何线程"—— 谁打日志就在谁的线程里执行（mainLoop、任意 subLoop#N、
//         用户自己的线程都可能），而类内部没有任何 mutex/atomic，
//         这份共享状态的安全性问题写在 src/Logger.cc 的 log() 上方。
//         【流程】本类的三个函数被 LOG_XXX 宏串起来，宏在 ①→⑫ 每一步都被调用。
// 【⚠问题】单例其实没有封住：这个类一个构造函数都没声明，于是编译器会补一个 **public** 的
//         默认构造函数（noncopyable 的 protected 构造对派生类的隐式构造是可访问的，
//         所以这条链路合法）。实测下面这几行能编译、能跑：
//             Logger bad;                       // 第二个 Logger 对象就这么出现了
//             bad.setLogLevel(ERROR);           // 改的是 bad 自己的 logLevel_
//             Logger::instance().log("x");      // 前缀仍然是 [INFO]，两者互不影响
//         后果：logLevel_ 可以有多份副本，"设级别"和"输出"可能作用在不同对象上，
//         调试时会出现"我明明设过级别了呀"的诡异现象。
//         修法（本文件不改，留给你动手）：把默认构造放 private（或 delete），
//         只留 static Logger& instance() 一个入口，需要更严的话再加 = delete 拷贝。
//         【对比原版】通行描述：原版没有"常驻单例 + 设级别 + 输出"这套组织方式：
//         Logger 是每条日志在调用点栈上构造的小对象（级别、文件名、行号都是构造参数），
//         析构时把这一行交给输出函数，因此不存在"两个对象抢一份可变状态"（细节以原版仓库为准）。
class Logger : noncopyable
{
public:
    // 【语法】返回引用而非指针：引用不能为空、不能重新绑定，调用方不用判 null，也不用写 ->。
    //         这里返回 static Logger& 的同时把构造函数留给编译器（见上面【⚠问题】）。
    //         【语法】static 成员函数意味着"没有 this 参数、通过 Logger::instance() 调用"，
    //         和 static 局部变量的"静态存储期"是同一个关键字的两件不其他文件的注释，初学者极易混。
    //         【流程/线程】每个 LOG_XXX 宏的第一步就是它，因此 ①→⑫ 全程、任意线程都会调用；
    //         实现见 src/Logger.cc（函数内 static 变量，C++11 保证并发初始化安全）。
    // 获取日志唯一的实例对象 单例
    static Logger &instance();
    // 【设计】为什么把"设级别"做成独立的公有成员函数，而不是 log(msg, level) 的第二个参数？
    //         因为宏只能写成三步直线：拿实例 → 设级别 → 输出，这样 4 个宏体可以完全复制粘贴。
    //         代价就是这一个"两步之间的窗口"：级别先写进共享成员，再由另一个函数读出来，
    //         中间没有任何同步 —— 详见 src/Logger.cc 里 setLogLevel 与 log 两处【⚠问题】。
    //         【对比原版】通行描述：原版把级别作为 Logger 对象的构造参数，一次构造就定死，没有"两步窗口"。
    // 【⚠问题】形参类型是 int 而不是 LogLevel：合法级别与非法级别在这里无法区分，
    //         也没有任何范围校验（实测 setLogLevel(99) 后输出行没有前缀）。
    // 设置日志级别
    void setLogLevel(int level);
    // 【⚠问题】std::string msg 按值传参（规范点名的退化）：
    //         宏里的调用是 logger.log(buf)，buf 是 char[1024]，
    //         于是先 strlen + malloc + memcpy 造出一个临时 std::string，
    //         再用它移动构造形参（C++11 传的是右值，走移动构造，缓冲区被"偷"过来，不再多分配一次）；
    //         如果调用方传的是左值字符串（例如 conn->name()），才会真的多拷一份。
    //         实测一条日志约 2~3 次堆分配：时间戳串 1 次 + "前缀+时间"拼接 1 次 + 长消息本体 1 次。
    //         正确做法：void log(const std::string& msg)，或 StringPiece（const char* + size_t，零分配）。
    //         【易错】按值传参唯一的"歪打正着"是形参是独立副本：即使调用方的缓冲区随后被改，
    //         函数内部也不会读到半新半旧的内容 —— 这也是 TcpConnection::send 跨线程时
    //         用 std::bind 按值拷贝 string 的同一个理由（见 src/TcpConnection.cc），
    //         但这里并没有异步转移，纯属白花一次分配。
    //         【对比原版】通行描述：原版不是"一条一条把字符串递出去"，而是在栈上的固定小缓冲区里
    //         攒字节、析构时一次性写出，热路径上基本不产生堆分配（缓冲区类型名以原版仓库为准）。
    // 写日志
    void log(std::string msg);

private:
    // 【⚠问题】logLevel_ 只做一件事：给输出行挑一个 [INFO]/[ERROR]/[FATAL]/[DEBUG] 前缀
    //         （它的唯一消费者是 src/Logger.cc 的 log() 里那个 switch）。
    //         它**不是阈值**：没有 minLogLevel、没有 if (level < xxx) return、没有"低于某级丢弃"，
    //         所以 LOG_INFO 和 LOG_ERROR 都会照样打印，日志总量只由"你写了多少条 LOG_XXX"决定。
    //         真要把某一级裁掉，本项目只有编译期那一个 #ifdef MUDEBUG 开关（且只影响 DEBUG）。
    //         【对比原版】通行描述：原版有一个全局级别阈值（默认 INFO 量级），
    //         每条日志第一步就比阈值、低于则立刻 return（连格式化都不做），
    //         所以线上可以从 INFO 临时降到 DEBUG 复现问题（阈值变量名与默认值以原版仓库为准）。
    //         【易错】它没有初始化列表，本文件里也不初始化；目前拿到 0（==INFO）是因为
    //         唯一实际存在的对象是 src/Logger.cc 里那个 static 局部变量，静态存储期对象先零初始化（实测）；
    //         一旦有人 Logger bad; 在栈上构造一个（见上面【⚠问题】），它的 logLevel_ 就是随机值，
    //         第一行日志的级别前缀不可预测 —— 这是"靠别的地盘侥幸兜住"的典型隐患。
    int logLevel_;
};