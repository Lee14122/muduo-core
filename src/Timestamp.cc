/* ============================================================================
 * 【文件】src/Timestamp.cc
 * 【一句话】实现 Timestamp 的三个成员函数：默认构造、按数值构造、取当前时刻、把时刻格式化成字符串。
 * 【Reactor 角色】基础设施 —— 全库唯一的时间来源，日志的时间戳和"事件到达时间"都从这里出。
 * 【依赖】<time.h> 提供 time() / localtime() / tm / time_t；
 *         "Timestamp.h" 提供类声明（类外定义必须先有声明，而且这里定义的是头文件里声明的那三个函数）。
 * 【谁用它】1) src/Logger.cc：每条日志的输出行都要 now().toString() 打时间（①→⑫ 每一步都可能触发）；
 *           2) src/EPollPoller.cc 的 poll()：用 now() 生成"这一批活跃事件的到达时间"，作为返回值
 *              交给 EventLoop::loop()，再经 Channel::handleEvent(receiveTime) 一路透传到用户的 onMessage（④→⑧）；
 *           3) include/EventLoop.h 的 pollRetureTime_ 成员、include/Channel.h 与 include/TcpConnection.h
 *              的 handleEvent/handleRead 形参，用的都是这个类型。
 * 【关于"原版"】本环境不能联网（github/raw 域名均不可达），仓库里也没有原版副本，
 *             所以下文所有【对比原版】条目都写成"通行描述"，只用来对照设计思路，
 *             接口名与写法一律以原版仓库为准；凡是标了"实测 / grep / man"的结论，才是本机可复现的事实。
 * 【先修知识】1) 类外定义成员函数为什么要写 Timestamp:: 前缀；
 *             2) time_t / struct tm / 为什么 tm_year 要 +1900；
 *             3) char 数组与 std::string 的区别（数组会退化成指针）；
 *             4) 返回值是"值"意味着什么（拷贝/移动/返回值优化）。
 * 【难度】★☆☆☆☆   【建议耗时】15 分钟
 * 【自检】1) now() 存进成员里的到底是秒还是微秒？谁"将错就错"地把它当秒用了？
 *         2) 多线程同时打日志时 localtime() 为什么会互相踩？正确替代是哪个函数？
 *         3) 若把 now() 改成真正的微秒而不改 toString()，日志会打印出什么样的年份？（实测：五千六百多万年）
 * ========================================================================== */
#include <time.h>

#include "Timestamp.h"
// 【语法】尖括号 <> 表示"去标准库/系统 include 路径找"，引号 "" 表示"先在当前目录找，再走 -I 路径"；
//         两者都只是查找策略差别，编译出来的东西一样。
//         这里 <time.h> 在前、自己的 "Timestamp.h" 在后，是本仓库的书写习惯（原版顺序以原版源码为准）；
//         另一种更严格的做法是把自己的头放第一个，这样能立刻暴露"头文件不自包含"的问题
//         （即 Timestamp.h 离开 <time.h> 就编译不过）。本项目的 Timestamp.h 只用了 int64_t/std::string，
//         并不依赖 <time.h>，所以两种顺序都编得过。


// 【设计】默认构造把时间戳置 0，约定 0 表示"无效/未赋值的时间"。
//         它是给"先声明后赋值"的场景用的：例如 include/EventLoop.h 里的
//         Timestamp pollRetureTime_; 成员在构造 EventLoop 时先取默认值（①），
//         等 EventLoop::loop() 里第一次 poll() 返回才写入真实时间（④）。
//         【对比原版】通行描述：原版的 Timestamp 接口更完整（另有 invalid()/swap()/addTime() 与
//         toStringMilli()/toStringMicro() 之类，名字以原版仓库为准）。
//         本仓库可核实的事实：include/Timestamp.h 里除两个构造外只声明了 now() 与 toString()，
//         所以这里没有 INVALID() 之类的工具，判"无效时间"只能自己拿数值跟 0 比。
Timestamp::Timestamp() : microSecondsSinceEpoch_(0)
{
}
// 【语法】这个构造在 Timestamp.h 里被标了 explicit，所以在 src 内部写
//         return Timestamp(time(NULL)); 这种"直接构造返回值"是合法的，
//         但外部代码不能写 Timestamp t = 1794392406; 这种隐式转换（单位会被猜错）。
//         【易错】形参名字与成员名字相同（microSecondsSinceEpoch），靠初始化列表
//         microSecondsSinceEpoch_(microSecondsSinceEpoch) 区分：括号里是形参，括号外是成员。
//         如果换成函数体里写 microSecondsSinceEpoch = microSecondsSinceEpoch;
//         就变成"形参自己赋值给自己"，成员根本没被初始化，且不报错（只有 -Wshadow 类警告）。
//         【流程】构造时机有两处：④ EPollPoller::poll() 里 Timestamp now(Timestamp::now())；
//                 以及任何 LOG_XXX 宏内部间接调用 now() 时。【线程】mainLoop / subLoop#N 都可能。

Timestamp::Timestamp(int64_t microSecondsSinceEpoch)
    : microSecondsSinceEpoch_(microSecondsSinceEpoch)
{
}
// 【系统调用】time(NULL) 语义：返回"从 1970-01-01 00:00:00 UTC 到现在的秒数"，类型是 time_t；
//         参数非空时还会把同一个值写到那个指针指向的对象里，传 NULL 就是只要返回值。
//         失败返回 (time_t)-1 并设置 errno（现实里几乎不会失败）。
//         它只到"秒"，一次系统调用（Linux 上通常走 vDSO，不需要真正陷入内核，但仍比读用户态缓存贵）。
// 【⚠问题】单位错位（本项目最典型的一处退化）：这里把**秒**直接塞进了名为 microSeconds 的微秒字段，
//         于是 Timestamp 内部数值是"秒"，而 include/Timestamp.h 的变量名承诺的是"微秒"；
//         toString() 又将错就错地把同一个值当秒交给 localtime()，所以整条日志时间线精度只到 1 秒，
//         高并发下大量日志会挤成同一秒、无法排序，也做不了"某次 read 到 write 用了多少微秒"这类测量。
//         【对比原版】通行描述：原版用 gettimeofday(&tv, NULL)（或 CLOCK_REALTIME）一次拿到秒与微秒，
//         换算成微秒存进成员，格式化时再除回 10^6 交给 localtime —— 进出两处必须配套改（以原版源码为准）。
//         【易错】这两处是一个"错误互相掩盖"的组合：只把 now() 改成微秒、不改 toString()，
//         日志时间立刻变成 56864001 年这种荒谬值（实测：localtime 并不报错，只是算出天文数字年份），
//         反而比现在更难查。

Timestamp Timestamp::now()
{
    return Timestamp(time(NULL));
}
// 【一句话】把内部那个整数当"Unix 秒"解析，输出 19 个字符的 "年/月/日 时:分:秒"（实测本机输出形如 2026/11/11 18:20:06）。
// 【流程】两个调用来源：① src/Logger.cc 每条日志打时间前缀（①→⑫ 全程、mainLoop 与每个 subLoop#N 都会走到）；
//         ② src/EPollPoller.cc poll() 生成事件到达时间（④，作为返回值往外传）。
// 【语法】成员函数后面的 const 修饰 this，承诺不改成员；正因为不改，
//         include/Channel.h、include/TcpConnection.h 里那些 Timestamp receiveTime（值传递，非 const 引用）
//         才能在回调链路上随手调用它。若去掉 const，const Timestamp& 就拿不到字符串了。
std::string Timestamp::toString() const
{
    // 【语法】= {0} 把整个数组清零，避免没写完的部分留下栈上的垃圾字节（snprintf 自己会补 '\0'，
    //         所以这里的清零主要是习惯与防御）。128 字节对 19 字符的输出来说非常宽裕。
    //         【易错】容量写死在两处：这里的 char buf[128] 和下面 snprintf 的第二个参数 128。
    //         改一处不改另一处，将来格式串变长就会被静默截断成半个时间串（snprintf 保证不越界写，
    //         但返回的是"它想写的长度"，本项目没人看返回值，也就没人发现截断）。
    //         更稳的写法是长度处写 sizeof(buf)，让数组与长度绑成一个来源（通行描述：原版即如此，以原版源码为准）。
    char buf[128] = {0};
    tm *tm_time = localtime(&microSecondsSinceEpoch_);
    // 【⚠问题】localtime() 不是线程安全的：它返回的是指向"进程内静态 tm 缓冲"的指针，
    //         POSIX 明确说两次调用（或两个线程）会互相覆盖同一块内存。
    //         【线程】本框架里 mainLoop 和每个 subLoop#N 都可能同时打日志（见 src/Logger.cc），
    //         于是 A 线程刚拿到 tm*、还没读完 6 个字段，B 线程已经把那块 tm 换成自己的时间，
    //         轻则打印出一个"月 0 日 40 点"的错乱时间，重则读到不一致的年月组合。
    //         【易错】还有第二个坑：tm* 是静态缓冲区的指针，所以它不是"用完即废"的临时对象，
    //         把 tm* 存起来晚点再用，同样会被下一次调用踩掉；要留就立刻把 6 个整数拷出来。
    //         【系统调用】线程安全的可重入版本是 localtime_r(&t, &tm_buf)（结果写进调用者给的 tm）。
    //         【系统调用】本机 man 3 localtime 可核实：RETURN VALUE 段写明 localtime() 返回的指针
    //         "指向静态分配的 struct tm，可能被之后任意日期时间函数的调用覆盖"；
    //         ATTRIBUTES 表里 gmtime()/localtime() 标注 MT-Unsafe race:tmbuf env locale，
    //         而 localtime_r() 与 ctime_r()/gmtime_r()/mktime() 同组标注 MT-Safe —— 这就是并发打日志时间会串的权威出处。
    //         也就是说把 struct tm 放到调用者自己的栈上，每线程各一份，日志时间就不会互相踩。
    //         【对比原版】通行描述：原版的日志格式化用的正是 localtime_r（以原版源码为准）。
    //         与 int64_t 同一个类型，所以可以把成员地址直接递过去；
    //         换到 32 位平台（int64_t 是 long long，time_t 是 long）就是类型不匹配的编译错误 ——
    //         这个函数能跑，一半靠运气一半靠 64 位 LP64 数据模型。
    //         【⚠问题】另外代码没有判 localtime 返回 NULL，也不做 time_t 溢出检查。
    snprintf(buf, 128, "%4d/%02d/%02d %02d:%02d:%02d",
             tm_time->tm_year + 1900,
             tm_time->tm_mon + 1,
             tm_time->tm_mday,
             tm_time->tm_hour,
             tm_time->tm_min,
             tm_time->tm_sec);
             // 【语法】六个实参的顺序必须和格式串里 %d 的顺序一一对应，写错顺序就是"月日互换"这类
             //         肉眼难发现的 bug（本项目所有 .cc 里 snprintf/printf 都靠人肉对齐）。
             //         tm_year 是"距 1900 年的年数"（历史包袱），tm_mon 是 0~11（1 月是 0），
             //         所以要 +1900 和 +1；%02d 表示不足两位补 0，%4d 表示至少 4 位宽。
             //         【易错】这三个"+1"式的换算全部写死在实现里，Timestamp 的调用方看不到，
             //         这就是为什么"库内部可以乱，接口层必须干净"。
    return buf;
    // 【语法】return buf 这里发生了一次隐式转换：char 数组退化成 char*，std::string 的
    //         非 explicit 构造函数被调用，于是 strlen + 一次堆分配（19 字节 > SSO 的 15 字节上限）。
    //         写成 return std::string(buf); 语义完全一样但意图更明确。
    //         【对比原版】通行描述：原版这个函数的缓冲区更小（32 字节量级），并且提供过
    //         "格式化到内部线程缓冲、零分配"这类热路径接口（名字与细节以原版仓库为准）。

}

// 下面这几行是作者留下的手工自测代码（写完后注释掉了），这里保持原样、不改不删。
// 想复现它的效果，别在库里留 main（会和 example/testserver.cc 的 main 冲突），
// 把这几行抄到 example/ 下单独一个 .cc 里，或者直接在命令行验证：
//     g++ -std=c++17 -Iinclude src/Timestamp.cc /tmp/mytest.cc -o /tmp/mytest && /tmp/mytest
// #include <iostream>
// int main() {
//     std::cout << Timestamp::now().toString() << std::endl;
//     return 0;
// }