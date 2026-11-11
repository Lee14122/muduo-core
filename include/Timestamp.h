/* ============================================================================
 * 【文件】include/Timestamp.h
 * 【一句话】给「时刻」做一个只有 8 字节的数据类型：内部就一个 int64_t，其余全是语法示范。
 * 【Reactor 角色】基础设施。不参与事件分发，但 EPollPoller::poll 的返回值、
 *   TcpConnection::handleRead 传给用户的 receiveTime、Logger 的日志前缀，都用它。
 * 【依赖】只依赖标准库（<string> 用于 toString 返回值）。整个库最底层的叶子文件之一。
 * 【谁用它】src/Logger.cc（拼时间前缀）、include/Poller.h 与 src/EPollPoller.cc（poll 的返回时刻）、
 *   include/Channel.h（handleEvent 的入参）、include/EventLoop.h（pollRetureTime_ 成员）、
 *   include/TcpConnection.h 与 include/Callbacks.h（MessageCallback 的第三个参数）、src/Timestamp.cc（实现）。
 * 【先修知识】int64_t 定长整型；static 成员函数（属于类，不属于对象）；const 成员函数；explicit 关键字。
 * 【难度】★☆☆☆☆   【建议耗时】10 分钟
 * 【自检】1) 为什么 now() 要 static，而 toString() 要 const？去掉各自会怎样？
 *        2) 去掉 explicit 后，代码里能写出什么危险语句？为什么单位错位是网络编程里最难查的 bug 之一？
 *        3) 这个类为什么不继承 noncopyable？（它是纯值类型，拷贝就是复制一个整数，没有资源冲突）
 *        4) 为什么头文件里没有 <sys/time.h>？（因为实现退化成了 time(NULL)，见下面【⚠问题】）
 * ========================================================================== */
#pragma once

// 【⚠问题】<iostream> 在本头文件里其实用不到：类声明里没有任何 std::cout/cerr，
//   toString 返回的是 std::string（由下一行的 <string> 提供）。
//   后果：每个 include 它的翻译单元都要背上 iostream 的初始化与模板实例开销，
//   还会让「谁真正需要流」这件事看不出来。原版这里放的是 <string> 与 <inttypes.h>（拿 int64_t）。
//   【易错】别因为编译器没报错就认为 include 是干净的：多余的 include 只会拖慢编译、埋依赖，
//   等到哪天你把这个头文件的实现挪走，全项目的传递包含就一起崩了。
#include <iostream>
#include <string>

// 时刻类型：表示「从 Unix 纪元（1970-01-01 00:00:00 UTC）到某一刻」的累计量。
// 【设计】它是值类型（value type）：放栈上、当成员、按值传参都安全，没有析构、没有资源。
//   与 Socket（管 fd，必须 noncopyable）形成对照 —— 同一个库里两种封装取舍，读代码时值得对比。
// 【对比原版】通行描述（以原版仓库为准）：原版 Timestamp 还提供 isValid()/invalidTime()/
//   addTime()/compare() 以及毫秒、微秒互转的自由函数，并内联实现大部分逻辑；
//   本教学版只留「构造 + now + toString」三件套，够读懂链路即可。
class Timestamp
{
public:
    // 默认构造：时间置 0，语义上是「无效时刻」。
    // 【设计】有了它，Timestamp 才能作为别的类的成员、才能放进容器而不必先显式赋值；
    //   真正要取时间请一律用 now()。
    // 【易错】0 不等于「1970 年」这个业务含义，它只是「没赋值」。原版用 isValid() 区分，
    //   本项目没这个接口，所以别拿 default 构造出来的对象去做时间差。
    Timestamp();
    // 【语法】explicit 禁止「隐式转换」。不加它，编译器会接受 Timestamp t = 1234567;
    //   而 1234567 是秒、毫秒还是微秒没人说得清 —— 单位错位会导致日志/超时差出几个数量级，
    //   且**不会崩溃**，只会「偶尔不对」，是最难查的一类 bug。
    //   加了 explicit，就必须写 Timestamp t(1234567); 明确表达「我在手动构造一个时刻」。
    // 【⚠问题】参数名承诺单位是「微秒」，但本项目的 now() 塞进来的实际是「秒」，见 src/Timestamp.cc。
    //   这个接口的形状是好的（定长 64 位微秒计数），坏的是实现没跟上。
    explicit Timestamp(int64_t microSecondsSinceEpoch);
    // 【语法】static 成员函数：不属于任何对象，用类名调用 Timestamp::now()。
    //   为什么必须 static？「取当前时刻」这件事不需要任何已有状态，硬要做成成员函数就得先有一个
    //   Timestamp 对象才能问时间，逻辑上是循环依赖。
    // 【流程】Logger::log（打日志前缀）与 EPollPoller::poll（返回事件到达时刻）各调它一次。
    // 【线程】无共享状态，任何线程可调、无需加锁（它调用的 time() 也是线程安全的）。
    static Timestamp now();
    // 【语法】签名末尾的 const 修饰 this，承诺不改成员；好处是 const Timestamp 对象也能调用。
    //   如果去掉 const，那么像「const Timestamp& t」这种引用上就调不动它了。
    // 转成「2024/09/04 20:50:19」这种人类可读串，仅供日志用。
    // 【易错】它按值返回 std::string，调用点会拿到一份新字符串（一次堆分配）；
    //   所以别在热路径（比如每条日志、每个包）上反复调它 —— 本项目的 Logger::log 恰好每条日志调一次。
    // 【对比原版】通行描述（以原版仓库为准）：原版把格式化实现成内部缓冲区 + 手工拼微秒小数位，
    //   并用线程安全的 localtime_r；本项目的实现见 src/Timestamp.cc 的 toString。
    std::string toString() const;

private:
    // 从 Unix 纪元算起的累计值。命名里的 microSeconds 是「本类的单位约定」。
    // 【⚠问题】约定与实现不一致（三处连坐，动手修见 docs/experiments.md 练习 1）：
    //   1) src/Timestamp.cc 的 now() 用 time(NULL) 取值，那是「秒」，却被存进了名为微秒的字段；
    //   2) toString() 又把同一个值当秒交给 localtime() 解析（所以打印出来的日期是对的，精度只到秒）；
    //   3) 于是「字段名说微秒、实际是秒」。哪天你把 now() 换成真微秒，
    //      toString() 不改就会打印出五万年后 —— 这就是单位错位 bug 的典型连锁。
    //   通行描述（以原版仓库为准）：原版用 gettimeofday()/clock_gettime 取真正的微秒，
    //   并且只在需要的时候才做秒↔微秒换算，字段名与实现一致。
    // 【设计】用 int64_t 而不是 time_t/unsigned：定长、可表示负数（用于「无效」或时间差），
    //   且微秒计数到 2262 年才溢出，够任何服务端用。
    int64_t microSecondsSinceEpoch_;
};
