/* ===========================================================================
 * 【文件】src/InetAddress.cc —— include/InetAddress.h 里三个成员函数的实现
 * 【一句话】实现"人写的 IP 串 + 数字端口" 与 "内核要的 16 字节 sockaddr_in"之间的
 *          双向翻译：构造时把端口 htons、把 IP 串 inet_addr；打印时把地址 inet_ntop
 *          并拼上 ":端口"。
 * 【Reactor 角色】基础设施 / OS 封装层。只做字节序与格式转换，不碰 fd、不碰事件循环。
 * 【依赖】InetAddress.h（本体）；string.h 的 memset/strlen；strings.h（历史包袱，见 include 说明）。
 * 【谁用它】①② main/TcpServer/Acceptor 构造监听地址；⑤ accept 填对端地址后被打印；
 *            ⑦⑪ 用户 onConnection 里 toIpPort() 打上下线日志。
 * 【先修知识】1) 大端/小端；2) 端口是 16 位无符号数（0~65535）；3) C 字符串以 NUL 结尾，
 *            strlen 不含 NUL；4) inet_addr 的"失败值"和 255.255.255.255 撞车这件事。
 * 【难度】★★★☆☆（代码很短，但字节序与内核约定是重点）  【建议耗时】25 分钟
 * 【自检】a) 端口 8080 忘记 htons，netstat 上会看到本机在监听哪个端口？画出两个字节。
 *        b) inet_addr("127.0.0.1") 的返回值是什么类型、什么字节序？写错格式时它给你什么？
 *        c) toIpPort 在同一个 buf 上先 inet_ntop 再 strlen 再 sprintf，为什么不会越界？
 *           这条隐式前提在什么情况下会崩？
 * ========================================================================= */

// 【易错】这里 include 的是 <strings.h>（老 BSD 头，放 strcasecmp / bcmp / index 这类
//          函数），不是 <string.h>（放 memset / strlen / memcpy）。本文件真正用到的是
//          memset 和 strlen，都来自下一个头。两个名字只差一个 s，glibc 下都能装进来，
//          编译不报错 —— 是读代码时的经典坑（本项目其它文件也反复出现这一对）。
#include <strings.h>
#include <string.h>
// 【依赖】memset：构造函数里把 addr_ 整块清零；strlen：toIpPort 里找 IP 串的结尾。
//          两者都是用户态函数，不进内核，不阻塞。

#include "InetAddress.h"

// ── 本函数干的事：把"人写的 port + 点分 IP"翻译成内核要的 16 字节 sockaddr_in ──────────
// 【流程】① main() 里 InetAddress addr(8080) 调它；② 造好的对象被 TcpServer/Acceptor
//          交给 Socket::bindAddress，最终喂给 ::bind。
// 【线程】mainLoop（主线程，启动阶段一次性）。
// 【设计】三步走：memset 清零 → 填 family → 填 port（转大端）→ 填 addr（inet_addr 已含大端）。
//          任何一步漏了都能编译，但内核看到的地址就是残的，报错点还离得很远（bind 的
//          EADDRNOTAVAIL / EAFNOSUPPORT / 连不上），这就是它需要逐行注释的原因。
// 【语法】定义里没有默认值（port、ip 都不写 = 0 / = "127.0.0.1"）：默认参数只能写在
//          声明里一次；定义再写一遍就是"重定义默认参数"，编译直接报错。
InetAddress::InetAddress(uint16_t port, std::string ip)
{
    // 【设计】16 字节全清零：sin_zero[8] 填充区必须为 0（内核/部分 libc 会校验整个结构），
    //            同时保证将来加字段时是确定的 0 而不是栈垃圾。
    // 【易错】顺序很重要：必须先 memset 再逐字段赋值（反过来会把已赋的值抹掉）。
    //            这段代码的三行语义上有先后依赖，不能重排。
    ::memset(&addr_, 0, sizeof(addr_));
    // 【设计】地址族：AF_INET 表示"后面那 14 字节按 IPv4 解释"。::bind 第一步就查它，
    //            不对就返回 EAFNOSUPPORT / EINVAL。AF_INET6 时是 10，AF_UNIX 时是 1。
    // 【易错】这个字段是 2 字节（sa_family_t = unsigned short），而 sin_port 紧跟着；
    //            如果谁忘了 memset 又把结构交给内核，family 是垃圾值时行为完全不可预测。
    addr_.sin_family = AF_INET;
    // ── 为什么要 htons：字节序（byte order）问题，本文件最重要的一个知识点 ──────────────
    // 【系统调用】htons = host to network short：把 16 位整数从主机字节序转成网络字节序。
    //            网络字节序规定是大端（最高有效字节放在最低地址）。x86/ARM（默认配置）是
    //            小端，所以这个函数真的会交换两个字节；在大端机器上它是空操作（编译成 mov）。
    //            配套：htonl 用于 32 位（IP 地址），ntohs/ntohl 是反方向（网络 变成 主机）。
    //            它们是 libc 里的宏或内联指令（bswap），不是系统调用，不进内核。
    // 【语法】前缀 :: 是"到全局命名空间去找"，防止哪天有人在某个 namespace 里写了同名的
    //            htons 而被 ADL/查找规则悄悄选中。对性能无影响，是调用 C API 的标注习惯。
    //
    // 【画出 8080 的字节序变化】8080 = 0x1F90，高位字节 1F，低位字节 90
    //   不转换（小端本机存法）：低地址 [ 90 ] 高地址 [ 1F ]     —— 数值仍是 8080
    //   htons 之后（大端网络序）：低地址 [ 1F ] 高地址 [ 90 ]     —— 这才是 sockaddr 要求的
    //   内核读 sin_port 时按大端解释，即 低地址字节 当 高位：
    //     忘记 htons   → 内核读到 0x901F = 36895 → 客户端连 8080 得到 ECONNREFUSED，
    //                    而你在 netstat -lntp 里看到本机监听的是 36895（面试常考的"连不上"排查）；
    //     多转一次     → 又换回小端读法 → 同样变 36895，症状一样，很难查；
    //     取出来比较   → 直接拿 sin_port 和 8080 比永远不等，必须先 ntohs（见 toPort）。
    // 【设计】结论一句话：端口和 IP 存进 sockaddr 时是网络序，取出来做数值运算/打印时
    //            先转回主机序。整个 muduo 里 sin_port 只在 bind/accept 的内核边界上出现，
    //            跨边界就转，绝不带着混合状态跑。
    // 【易错】参数是 uint16_t：传 70000 会在调用点被静默截断成 4644（70000 与 0xFFFF 取与），
    //            不报错不警告（本项目 example 没这么写，但面试会问）。
    addr_.sin_port = ::htons(port); // 本地字节序转为网络字节序
    // 【系统调用】inet_addr("点分十进制") 返回 in_addr_t（32 位无符号），返回值已经是
    //            网络字节序，所以这里直接塞给 s_addr，绝对不能再 htons（那是双重转换）。
    //            内部：解析四段 0~255 的十进制数，按大端拼成 32 位。
    //            失败返回 INADDR_NONE，也就是 0xFFFFFFFF，和 inet_addr("255.255.255.255")
    //            的正确结果完全相同 —— 所以它天生无法区分"格式错"和"广播地址"。
    // 【⚠问题】这里不判返回值：传 "abc"、"256.1.1.1"、"1.2.3"（老式简写会被接受成别的意思）
    //            都得到 0xFFFFFFFF，错误要到 ::bind 时才以 EADDRNOTAVAIL 的形式爆出来
    //            （"该 IP 不属于本机"），错误信息与真正起因隔了一层，排查成本高。
    // 【对比原版】现代正确写法是 inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr)：
    //            返回 1 成功 / 0 格式非法 / -1 不支持的地址族，能明确区分错误并且可写进
    //            sockaddr_storage；原版 muduo 更进一步用 getaddrinfo（InetAddress::resolve）
    //            以同时支持域名解析与 IPv6。
    // 【易错】"127.0.0.1" 与 "0.0.0.0" 的分别：0.0.0.0（INADDR_ANY）表示"本机所有网卡
    //            都监听"，127.0.0.1 只监听回环，别的机器根本连不上。本项目默认值是
    //            127.0.0.1，在容器里或远程机上想被外部访问必须显式传 "0.0.0.0"。
    // 【设计】s_addr 是 in_addr 唯一的成员（本质就是 struct in_addr { in_addr_t s_addr; }），
    //            所以"写 s_addr"就是"写这个 IPv4 地址"，没有别的字段。
    addr_.sin_addr.s_addr = ::inet_addr(ip.c_str());
}

// 【做什么】反向翻译：二进制地址 变成 点分十进制串。
// 【流程】诊断辅助，不在 ⑧⑨⑩ 数据通路上；② 启动日志与 ⑥⑦⑪ 连接日志里被间接使用
//          （toIpPort 内部也是靠同一个 inet_ntop）。
// 【线程】谁调用在谁那儿：mainLoop（②）或 subLoop#N（⑦⑪）；无状态、不加锁，可并发调。
std::string InetAddress::toIp() const
{
    // addr_
    // 【设计】64 字节是"最坏情况留足余量"：IPv4 点分十进制最长 15 个字符
    //            （255.255.255.255）加一个结尾 NUL 只要 16 字节；给到 64 是为了让
    //            toIpPort 再往后拼 ":65535"（最多 6 字符加 NUL）也绰绰有余。
    //            这是栈上数组，不分配堆，函数返回后 buf 销毁，所以必须 return std::string。
    // 【语法】= {0} 把整个数组清零：即使 inet_ntop 失败返回 nullptr 且没有写 buf，
    //            return buf 拿到的也是空串而不是栈垃圾 —— "永远返回合法字符串"的兜底。
    // 【系统调用】inet_ntop 不是系统调用，是 libc 里的纯用户态格式化：不进内核、不阻塞、
    //            不设 errno 分支（除 ENOSPC/EAFNOSUPPORT），失败只返回 nullptr。
    //            入参四个：地址族 AF_INET、源（指向 4 字节 s_addr 的指针，这里传
    //            &addr_.sin_addr 而不是 &addr_.sin_addr.s_addr，二者地址相同但类型不同，
    //            inet_ntop 要求的是"该族的二进制地址指针"）、目的缓冲区、缓冲区容量。
    //            返回 const char*（就是目的 buf 的地址），本项目忽略返回值 —— 靠 buf 已被
    //            清零来保证失败时也不崩。IPv6 时容量要给 INET6_ADDRSTRLEN（46）。
    // 【易错】第 4 个参数是"容量"：给小了 inet_ntop 直接失败返回 nullptr 并把 errno 设成
    //            ENOSPC，它绝不会截断写出一个不合法的串。本项目传 sizeof buf = 64，永远够。
    // 【对比原版】原版用 buf[kMaxIpLen] 加 inet_ntop(AF_INET6,...) 同时支持两种族，
    //            并用 std::string(buf) 显式构造，让"这里在造字符串"这件事写在脸上。
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    // 【语法】return buf：数组名在表达式里退化成 char*，std::string 的构造函数按 NUL
    //            求长度（等价 std::string(buf)），所以这里隐式造了一个 std::string 再返回，
    //            并且它是有拷贝/移动开销的那一步（一次堆分配）。
    return buf;
}

// 【做什么】"IP:端口"串，例如 "127.0.0.1:8080"。端口必须先 ::ntohs 还原成主机数值，
//            否则 %u 打出来的是大端裸值（8080 变 36895）。
// 【流程】⑦ connectEstablished 调 connectionCallback_（用户 onConnection）打印对端上线；
//          ⑪ handleClose/connectDestroyed 打印下线；② TcpServer 构造日志打印监听地址。
// 【线程】②/⑥ 在 mainLoop，⑦/⑪ 的连接日志在 subLoop#N。
// 【面试】"怎么在日志里区分不同客户端？" —— 靠对端的临时端口：同一台机器发来的多条连接
//          IP 相同，但 127.0.0.1:52344、127.0.0.1:52345 不同，这正是四元组唯一性的体现。
std::string InetAddress::toIpPort() const
{
    // 本函数前半段和 toIp 完全一样（栈 buf + inet_ntop），重点在后三段拼接。
    // ip:port
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    // ── 这段"在同一块 buf 上 写IP → strlen → 再往后 sprintf"为什么安全 ────────────────
    // 【⚠问题/易错】看着像自我覆盖，实际成立靠三个前提，缺一不可：
    //            1) inet_ntop 写完 buf 会自动带结尾 NUL，所以 strlen(buf) 恰好停在 IP 串的
    //               末尾（即那个 NUL 的下标），buf+end 正是"应该接着写"的位置；
    //            2) 追加的部分最长是 ":65535"，6 个字符加 1 个 NUL = 7 字节；IP 前缀最长
    //               15 字节，15 + 7 = 22，小于 buf 的 64，永远碰不到数组尾巴，不越界；
    //            3) 读的方向只有 strlen 一次，它读的是刚写好的前缀，NUL 也在 buf 内，
    //               不存在读越界或悬垂。
    //            另外注意：sprintf 是先覆写那个 NUL 再往后写，所以两段内容天然连续，
    //            拼完仍然是合法 C 串，return buf 一次就能取到全串。
    // 【易错】这个"安全"是算出来的，不是类型保证的。两种改动会让它立刻变成栈溢出：
    //            a) 有人把 buf 缩小到 22 字节以内（或者照抄一份代码时手抖改了大小）；
    //            b) 支持 IPv6：地址串最长 45 字符，加 ":65535" 需要 53 字节，
    //               64 虽然还勉强够，但如果同时忘了把 strlen 的返回值当作"已写字节数"
    //               就会重复计算错误。正确姿势是用 snprintf 让边界由函数参数兜住。
    // 【对比原版】原版 muduo 用 std::string 拼接（toIp() + ":" + to_string(toPort())）或
    //            一次 snprintf(buf, sizeof buf, "%s:%u", ...)，长度上限显式出现在调用里，
    //            不依赖"我算过够用"这种注释级安全。
    // 【设计】end 用 size_t（strlen 的返回类型），不要写成 int：省掉一次有符号转换警告。
    size_t end = ::strlen(buf);
    // 【系统调用】ntohs 把 sockaddr 里存的大端端口转回主机数值，这样 %u 打出来才是 8080。
    // 【易错】返回值先被提升为 int（uint16_t 到 int），再赋回 uint16_t，这里没有精度损失；
    //            但如果是 32 位字段就要用 ntohl，写成 ntohs 就只转了低半个字。
    uint16_t port = ::ntohs(addr_.sin_port);
    // 【系统调用】sprintf 无长度上限地往 buf+end 写，安全性完全依赖上面那条"15+7 小于 64"
    //            的算术，所以 sprintf(buf+end, ...) 这一句是全文件最脆的地方（本项目不改，只提示）。
    // 【对比原版】snprintf(buf + end, sizeof buf - end, ":%u", port) 才是自我约束的写法：
    //            第二个参数把"还能写多少"显式交给库，越界风险归零。
    // 【语法】%u 配 uint16_t：可变参数会被提升（uint16_t 变 int），%u 按 unsigned int 读，
    //            实践中值非负所以正确；严格写法是 %hu（unsigned short）或先转 unsigned int。
    // 【设计】这里不用 std::string 拼接，是为了省一次堆分配和一次拷贝：整个函数只用一块栈 buf。
    // 【易错】没有加 :: 前缀（sprintf/strlen 在本文件里混用 :: 与不加 ::），风格不统一
    //            但语义一致；见到混用不要以为有什么特殊含义。
    sprintf(buf+end, ":%u", port);
    return buf;
}

// 【做什么】端口号（主机序）。三行代码里唯一的信息量就是"记得 ntohs"。
// 【线程】调用者线程；【流程】诊断辅助，不参与 ⑧⑨⑩ 的数据通路。
// 【设计】返回 uint16_t 而不是 int：把"值域只有 0~65535"这件事写进签名，
//            调用方拿它比较或格式化都不会遭遇符号扩展问题。
uint16_t InetAddress::toPort() const
{
    // 【易错】写成 return addr_.sin_port 就返回大端裸值：8080 变 36895，
    //            而且这个 bug 只在"打印出来看"或"和常量比较"时才暴露。
    return ::ntohs(addr_.sin_port);
}

// 【说明】下面 #if 0 到 #endif 之间是一个"手工验证小程序"：作者当年写完 InetAddress 用它
//          确认 8080 到底被转成了什么、toIpPort() 的串长什么样，验证完把它关掉而不是删掉，
//          留作"怎么手动测这个类"的示例。因为预处理器整块丢弃，它不参与编译，所以文件里
//          出现 main 也不会和 example/testserver.cc 的 main 冲突。
//          规范铁律要求原样保留，本注释只写在块外。想看效果可以临时把 #if 0 改成 #if 1
//          单独编译一份（不要提交，会和其他 .cc 里的 main 撞车）。
// 【先修知识】#if 0 ... #endif 是 C/C++ 里"注释掉一大段代码"的标准手法，比逐行加 // 好在
//          块内可以含 // 与 /* */，也方便一句改回。
#if 0
#include <iostream>
int main()
{
    InetAddress addr(8080);
    std::cout << addr.toIpPort() << std::endl;
}
#endif