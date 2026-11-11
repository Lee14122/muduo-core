/* ============================================================================
 * 【文件】include/noncopyable.h
 * 【一句话】一个不装任何数据的"标记基类"：谁继承它，谁的拷贝构造和赋值运算符就编译不过，
 *           于是全库的对象只能靠引用 / 指针 / shared_ptr 传递，不会被悄悄复制一份。
 * 【Reactor 角色】基础设施。它不是 Acceptor / Demultiplexer / Loop / Handler 中的任何一个；
 *                   真正继承它的是 9 个类：Acceptor、Channel、Socket、Logger、EventLoop、
 *                   Thread、EventLoopThread、EventLoopThreadPool、TcpConnection（都靠它声明"我不许被复制"）。
 * 【依赖】不依赖本项目任何其他头文件，也不依赖标准库；没有对应的 .cc（全部在类内声明完）。
 * 【谁用它】11 个头文件 include 它：上面 9 个类的头，外加 include/Poller.h 与 include/TcpServer.h。
 *           【易错】后两个只 include、没继承（看 class Poller 与 class TcpServer 的冒号后面是空的）：
 *           - Poller 是抽象基类，而派生类 EPollPoller 也没继承 noncopyable，
 *             成员全是 int + vector + map（都可拷贝），所以"EPollPoller 居然能被复制"在当前代码里是真的。
 *             没人这样写，所以不会出事，但"忘了继承"在本库确实存在。
 *           - TcpServer 侥幸安全：它有一个 std::unique_ptr<Acceptor> acceptor_ 成员，
 *             拷贝构造因此被"隐式定义为 deleted"。效果相同，但这条约束来自成员而不是设计意图 ——
 *             将来有人把 unique_ptr 换成 shared_ptr 或裸指针，TcpServer 就悄悄变成可拷贝的了。
 * 【流程】只在编译期生效，不参与 ①→⑫ 任何一步，也就没有"在哪个线程"的问题：
 *         它的效果是"让某行拷贝代码编译失败"，运行期一条指令都不产生。
 * 【关于"原版"】本环境不能联网（github/raw 域名均不可达），仓库里也没有原版副本，
 *             所以下文所有【对比原版】条目都写成"通行描述"，只用来对照设计思路，
 *             接口名与写法一律以原版仓库为准；凡是标了"实测 / grep / man"的结论，才是本机可复现的事实。
 * 【先修知识】1) 拷贝构造函数 T(const T&) 与赋值运算符 operator= 的区别；
 *             2) C++11 的 = delete / = default；
 *             3) public / protected / private 三种继承与访问控制；
 *             4) 头文件被多次 #include 会发生什么。
 * 【难度】★☆☆☆☆（全库最短、最适合第一个读的文件，读懂它后面每个头文件的 class X : noncopyable 就不用再问）
 * 【建议耗时】10 分钟
 * 【自检】1) 只 delete 拷贝构造、不 delete operator=，会出现哪种"还是被复制了"的写法？
 *         2) 构造和析构放 protected 而不是 public，各挡住了什么危险代码？
 *         3) 这个类的析构函数不是 virtual，为什么不算 bug？（提示：看它被继承的方式）
 * ========================================================================== */
#pragma once // 防止头文件重复包含
// 【语法】#pragma once 让同一个翻译单元里这份内容只被纳入一次，等价于
//         #ifndef MUDUO_NONCOPYABLE_H / #define MUDUO_NONCOPYABLE_H / ... / #endif 三件套。
//         尖括号 <> 是"到系统/标准 include 路径找"，引号 "" 是"先当前目录再到 -I 路径找"；
//         本仓库所有头文件统一用 #pragma once（grep -L 'pragma once' include/*.h 结果为空）。
//         【易错】#pragma once 只按"文件的物理身份"去重：同一个文件被符号链接成两个路径时
//         个别编译器会认为它们是两份内容 —— 传统宏卫士没有这个问题，这是它唯一的代价。


/**
 * noncopyable被继承后 派生类对象可正常构造和析构 但派生类对象无法进行拷贝构造和赋值构造
 **/
// 【设计】"标记基类"(marker base class)：不存数据、不提供函数，存在的唯一价值就是被继承。
//         因为 = delete 只能作用于"当前这个类"的成员，想把同一条限制贴到 11 个类上，
//         就抽一个空基类出来，避免 11 处复制粘贴，也让读代码的人一眼看出这个类的约束。
// 【语法】继承它通常不会让对象变大（空基类优化 EBO）：sizeof(EventLoop) 里看不到这 1 字节，
//         编译器会把空基类的地址与其它成员重叠。所以"加约束不加成本"是它敢当基类的底气。
// 【易错】class Logger : noncopyable 没写继承关键字，对 class 来说默认是 private 继承。
//         照样能禁住拷贝，原因不是"继承方式是 private"，而是：编译器为 Logger 隐式生成的
//         拷贝构造必须调用基类 noncopyable 的拷贝构造，那个函数是 delete 的，
//         于是 Logger 的拷贝构造被"隐式定义为 deleted"。private 继承只是额外使得
//         Logger* 不能隐式转成 noncopyable*（本来也没人这么写）。
class noncopyable
{
public:
    // 【语法】= delete 的三条好处（对比 C++03 的"声明了但不实现"老写法）：
    //         ① 报错在编译期直接落在"函数名 + 调用点"，而不是链接期一句 undefined reference；
    //         ② 被 delete 的函数仍然参与重载决议，能顺手挡住"通过隐式转换绕开"的调用；
    //         ③ 意图写在声明上，别人不会误以为是漏实现而去"帮忙补一个"。
    //         【易错】去掉这两行会怎样：TcpConnection/Socket 这类持有 fd 的对象就能被拷贝，
    //         同一个 fd 出现两份"我拥有它"的所有权 → 两个副本各自 close() 一次 →
    //         第二次 close 的可能是别的线程刚刚打开的 fd（这是网络编程里最典型的事故之一）。
    //         这也正是本项目 TcpServer 用 shared_ptr<TcpConnection>、Channel 只被 EventLoop 持有的原因。
    // 【线程】无：这两行是编译期约束，任何线程都看不到它们的存在。
    noncopyable(const noncopyable &) = delete;
    noncopyable &operator=(const noncopyable &) = delete;
    // void operator=(const noncopyable &) = delete;    // muduo将返回值变为void 这其实无可厚非
    // 【对比原版】上面被注释掉的那行写的是 void operator=(...) = delete，注释也说"无可厚非"，确实如此：
    //         函数一旦被 delete，就永远不会真的被调用，返回类型写成 noncopyable& 还是 void 都不影响结果，
    //         两种写法在这里没有功能差别。写 noncopyable& 的好处只是"符合惯例"：
    //         将来若有人改成可复制的实现，返回引用才支持 a = b = c 这种链式赋值。
    // 【易错】两条必须一起 delete，缺一不可：
    //         只 delete 拷贝构造，还能写 s2 = s1;（赋值运算符还没被声明过，编译器会隐式生成一个）；
    //         只 delete operator=，还能写 Socket s2 = s1; 或函数按值传参 Socket byvalue(Socket s) —— 走的是拷贝构造。
    //         两个都 delete 之后，"复制一个已有对象"的所有入口（显式构造、赋值、按值传参、按值返回）才全断掉。
protected:
    // 【设计】构造/析构放 protected 是"一石二鸟"：
    //         ① 挡住 noncopyable n; 这种单独实例化基类的写法 —— 它没有任何数据，实例化出来毫无意义；
    //            派生类的成员函数（包括编译器为 Logger 生成的隐式默认构造）可以访问 protected 基类成员，
    //            所以"子类能构造，外人不能构造"这个效果正好达成。
    //         ② 挡住 delete 基类指针：noncopyable* p = new SomeLoop(); delete p; ——
    //            delete 需要访问 ~noncopyable()，而它是 protected，编译器直接报错，
    //            于是不会出现"通过没有 virtual 析构的基类指针 delete 派生类"这种未定义行为。
    //            【易错】这条保护只在"有人 public 继承 noncopyable 并拿基类指针删"时才用得上；
    //            本库全是默认 private 继承，转换本来就不合法，所以这里是"双保险"而不是唯一防线。
    //         这就是本库大量类（EventLoop、Channel、Socket、Thread）析构不写 virtual 却不算 bug 的前提：
    //         没有人通过 noncopyable* 去 delete 它们，对象都在自己类型的指针/引用上析构。
    noncopyable() = default;
    ~noncopyable() = default;
    // 【语法】= default 的意思是"请编译器按默认语义生成实现"，而不是"什么都不做"。
    //         为什么这里要显式写？因为访问级别要靠这两行落到 protected：
    //         如果整段省略，编译器隐式生成的构造/析构会是 public 的，"外面不许单独实例化 noncopyable"这条就失效了。
    //         默认构造/析构本身是 trivial 的（类里没有任何成员），生成出来是零成本，
    //         不产生任何可执行代码，所以全库的 noncopyable 继承在运行期一点开销都没有。
    // 【对比原版】通行描述（本环境无法核实原版源码，细节以原版仓库为准）：
    //         原版 noncopyable.h 与本文件几乎同形 —— 同样 public: = delete 两行 + protected: = default 两行，
    //         差别只在 operator= 的返回类型写作 void（正是本文件里被注释掉的那行）；
    //         C++03 时代常见的老写法是 private: 只声明不给定义，靠链接期报错，报错来得晚、也更难懂。
};