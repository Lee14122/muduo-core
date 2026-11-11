/* ===========================================================================
 * 【文件】src/Buffer.cc
 * 【一句话】用一次 readv 把 socket 上的字节读进缓冲区（放不下就溢出到栈上 64KB 再搬回来），
 *          用一次 write 把缓冲区里的字节交给 socket —— 收发两条数据通路各只留一个系统调用。
 * 【Reactor 角色】Handler 的数据面实现。Buffer.h 管"下标与内存布局"，这里管"和内核换字节"。
 * 【依赖】sys/uio.h(::readv 与 struct iovec)、unistd.h(::write)、errno.h(errno)、Buffer.h 本体。
 * 【谁用它】⑧ TcpConnection::handleRead 调 inputBuffer_.readFd(channel_->fd(), &savedErrno)；
 *          ⑩ TcpConnection::handleWrite 调 outputBuffer_.writeFd(channel_->fd(), &savedErrno)，
 *          发出去多少由调用方 retrieve 多少。
 * 【先修知识】1) 非阻塞 fd 上 read/write 的三种返回（正数、0、负数加 EAGAIN）；
 *            2) TCP 是字节流，一次 read 拿到的是"当前内核缓冲区里有的"，不是"一条消息"；
 *            3) 内核 socket 接收/发送缓冲区的存在；4) 栈帧与线程栈大小。
 * 【难度】★★★★☆（readv 双缓冲区是全库最巧的一段）  【建议耗时】40 分钟
 * 【自检】a) 画出 vec[0] 与 vec[1] 各自指向哪里、长度是多少，说明内核填它们的顺序。
 *        b) 一次 readFd 最多能读多少字节？把 128K-1 推出来。
 *        c) n 的三种返回值分别代表什么？为什么第三种要多一次 memcpy，而不是"白丢数据"？
 * ========================================================================= */

// 【依赖】errno：readv 失败时把错误码存进调用给的 saveErrno，靠的是先读到这里的全局 errno。
//          【易错】errno 实际是宏（线程局部变量，*__errno_location()），所以"给 errno 赋值"
//          这种写法在线程下也安全；但 errno 只在不返回成功的调用里才会被设置，
//          成功调用不会把 errno 清 0 —— 判错只看返回值，不要看 errno 是否非 0。
#include <errno.h>
#include <sys/uio.h>
// 【依赖】sys/uio.h：struct iovec 与 ::readv/::writev —— 本文件全部技巧都在这个头给的
//          "scatter/gather 一次性读写多段内存" 能力上。
#include <unistd.h>
// 【依赖】unistd.h：::write（writeFd 唯一的系统调用）。

#include "Buffer.h"

/**
 * 从fd上读取数据 Poller工作在LT模式
 * Buffer缓冲区是有大小的！ 但是从fd上读取数据的时候 却不知道tcp数据的最终大小
 *
 * @description: 从socket读到缓冲区的方法是使用readv先读至buffer_，
 * Buffer_空间如果不够会读入到栈上65536个字节大小的空间，然后以append的
 * 方式追加入buffer_。既考虑了避免系统调用带来开销，又不影响数据的接收。
 **/
// ── 本函数要解决的核心矛盾（原注释已经点出来了，这里展开）──────────────────────
// 【设计】矛盾在于：Buffer 的容量是确定的（比如 1024 字节），但 TCP 一次能到达多少字节
//          完全不由我们决定（对端一口气发 200KB，内核接收缓冲区里堆了多少就读多少）。
//          两个极端解法都不好：a) 只读 writable 个字节 => 剩下的要靠下一次 EPOLLIN 才收，
//          在 LT 下没问题但要多轮事件，ET 下直接丢数据；b) 每次先把缓冲区扩到很大 =>
//          绝大多数小包都白占内存。折中就是"先往缓冲区读，缓冲区满了再多读一块到临时区"，
//          那块临时区放在栈上（几乎零成本），只有真的溢出时才付出一次 memcpy。
// 【流程】⑧ 的第一步：客户端数据到达 → subLoop 的 poll 返回 → Channel::handleEvent
//          → Channel::handleEventWithGuard → TcpConnection::handleRead → 这里。
//          下游是 messageCallback_(conn, &inputBuffer_, receiveTime)，也就是用户的 onMessage。
// 【线程】connfd 所属的 subLoop#N（绝不能跨线程调：inputBuffer_ 只属于那个 loop）。
// 【设计】"Poller 工作在 LT 模式"这句原注释的分量：LT 下这次没读完的数据不会丢，
//            内核会在下一次 epoll_wait 继续报 EPOLLIN，所以本函数可以"少读一点"。
//            如果换成 ET（边缘触发），必须 while 循环读到 EAGAIN 才算读完，
//            本函数的写法就要变（但 muduo 的 readFd 语义仍可复用，只是外面要套循环）。
// 【面试】"muduo 一次读多少？" —— 一句话答案：最多 min(writable, 内核里已到数据) 加
//            必要时一段 64KB 的栈空间，单次上限 128K-1 字节；不是一条消息。
ssize_t Buffer::readFd(int fd, int *saveErrno)
{
    // 栈额外空间，用于从套接字往出读时，当buffer_暂时不够用时暂存数据，待buffer_重新分配足够空间后，在把数据交换给buffer_。
    // ── 为什么是 64KB、为什么在栈上、代价是什么 ──────────────────────────────────────
    // 【设计】65536 这个数不是拍脑袋：它等于 64KB，和常见套接字接收缓冲区
    //            （/proc/sys/net/ipv4/tcp_rmem 中位数、默认 max 约 4MB、初始约 64KB）同量级，
    //            所以"一次 readv 把内核里已到达的数据基本收干"是大概率事件。
    // 【设计】为什么用栈而不是 malloc：
    //            1) 成本：栈上就是移动一下 rsp，没有分配器加锁、没有 free；malloc 64KB
    //               会落在 glibc 的 mmap 阈值（默认 128KB）附近，大块要走 mmap/munmap，
    //               每次收发都多两三个系统调用，比 readv 本身还贵；
    //            2) 缺页：线程栈（默认 8MB）早已映射好，这块地址反复被用，页表项和 TLB
    //               都是热的；新 mmap 的内存第一次写入必触发 page fault；
    //            3) 生命周期正好：extrabuf 只在本次 readFd 内有效，函数返回自动消失，
    //               不需要任何所有权管理，也绝不会泄漏。
    // 【⚠问题】= {0} 会把这 64KB 全部清零（编译器只能生成一次 memset，它无法证明
    //            readv 会覆盖全部字节）。也就是说每次 readFd 都要白写 64KB 内存，
    //            量级大约几十微秒内的带宽开销，在高频小包场景下相当可观。
    //            原版 muduo 这里写作 char extrabuf[kDefaultExtraBufferSize]; 不初始化 ——
    //            因为我们只会读"内核真正写进来"的那 n-writable 个字节，其余内容根本不重要。
    //            这段代码按铁律保持原样，但要认得出这是一处可优化的退化。
    // 【易错】风险也来自"栈"：线程栈默认 8MB，64KB 看着不多，但如果
    //            a) 有人用 pthread_attr_setstacksize 把栈调到几百 KB（嵌入式、协程化改造），
    //            b) 或者 messageCallback_ 里再递归调用会走 readFd/sendFileInLoop 的路径，
    //            那 64KB 一块的栈帧就会变成栈溢出（直接 SIGSEGV，且 core 里看着像随机崩溃）。
    //            所以"把大缓冲区放栈上"这个决定的前提是"我们的线程栈足够深"。
    // 【对比原版】原版把这个尺寸做成常量 kDefaultExtraBufferSize（默认 65536），
    //            并允许上层调整；本教学版写死字面量，读的时候要知道它是可配置的点。
    char extrabuf[65536] = {0}; // 栈上内存空间 65536/1024 = 64KB

    /*
    struct iovec {
        ptr_t iov_base; // iov_base指向的缓冲区存放的是readv所接收的数据或是writev将要发送的数据
        size_t iov_len; // iov_len在各种情况下分别确定了接收的最大长度以及实际写入的长度
    };
    */

    // 使用iovec分配两个连续的缓冲区
    // ── iovec：一段"地址 加 长度"，readv/writev 拿它描述多段内存 ──────────────────
    // 【系统调用】struct iovec 两个成员：iov_base（这块内存的起始地址）、iov_len（这块
    //            内存的字节数）。readv(fd, vec, iovcnt) 的语义：内核按 vec[0]、vec[1]……
    //            的顺序把读到的字节依次灌进去，第一段填满才写第二段；返回值是所有段
    //            实际收到的总字节数（ssize_t）。
    //            writev 反过来：按顺序从各段取数据发出去。
    //            为什么用它而不是两次 read：一次系统调用就能覆盖两段内存。两次 read 就是
    //            两次陷入内核（每次约 1~2 微秒），而且中间对端可能又发来数据，
    //            读到的总量不确定；readv 的语义还额外保证"字节顺序与流顺序完全一致"
    //            （POSIX 规定 readv 是原子的：不会插入别的线程读到的数据 —— 对本项目
    //            单线程独占一个 fd 的场景只是锦上添花）。
    //            read(fd, buf, n) 就是 readv(fd, 一个 iovec) 的特例，性能基本相同。
    struct iovec vec[2];
    // 【设计】为什么要先把 writable 存成局部 const 变量，而不是每次直接调函数：
    //            下面 vec[0].iov_len、iovcnt 的判断、以及三条返回分支里的比较都要用它，
    //            而一旦走进 append（会 resize 并改变 buffer_.size()），
    //            writableBytes() 的数值就变了 —— 缓存住"进系统调用之前的可写量"是必须的，
    //            否则 n <= writable 这个判断就不成立了。这是本函数正确性的隐式前提。
    // 【易错】const size_t 只是防止本函数内误改，编译器不会替你证明"值没变"，
    //            靠的是"缓存时机正确"。
    const size_t writable = writableBytes(); // 这是Buffer底层缓冲区剩余的可写空间大小 不一定能完全存储从fd读出的数据

    // 第一块缓冲区，指向可写空间
    // ── 双缓冲区布局（本函数最该记住的一张图）──────────────────────────────────
    //   vec[0] → buffer_ 的可写区（就是 beginWrite() 指向的地方）
    //            低地址                                            高地址
    //   [ kCheapPrepend | 已读垃圾 |      可读区       | ← vec[0] 覆盖这里 → ]
    //                                                   writerIndex_    size()
    //   vec[1] → extrabuf（本函数栈上，64KB，与 buffer_ 完全无关）
    //            [ ← vec[1] 覆盖这里：最多 65536 字节 → ]
    //
    // 【设计】内核把数据直接写进 buffer_ 的可写区：这就是"零拷贝收包"的落点 ——
    //            数据从内核态到用户态只走这一次 memcpy，没有"先读到临时数组再 append"
    //            的第二次拷贝。对比一下朴素写法：char tmp[4096]; read(fd,tmp,..);
    //            buf.append(tmp,..) —— 每包多一次拷贝。
    // 【语法】这里用 begin() + writerIndex_ 而不是 beginWrite()：两者完全等价
    //            （beginWrite 就是这个表达式），写开是为了让"iov_base 是可写区起点"更醒目。
    vec[0].iov_base = begin() + writerIndex_;
    // 【设计】iov_len 是"这块内存最多能被写多少"，内核绝不会越界（它是安全边界，
    //            不是提示）。这里给的是缓冲区真实剩余量，所以 vec[0] 永远不会写爆 buffer_。
    vec[0].iov_len = writable;
    // 第二块缓冲区，指向栈空间
    // 【设计】第二段的地址指向栈：栈内存对内核同样可写（内核用 copy_to_user 那套
    //            机制访问用户地址空间的任意合法地址，不区分堆栈）。
    // 【易错】iov_base 的类型是 void*，这里 char* 隐式转换合法；但绝不要传
    //            std::string 的 &s[0] 之类"可能被 COW 或重分配"的地址 —— 本函数里
    //            buffer_ 与 extrabuf 在 readv 期间都不会动，所以安全。
    vec[1].iov_base = extrabuf;
    // 【设计】给满 65536：这一段的作用就是"兜底能兜多少兜多少"。
    vec[1].iov_len = sizeof(extrabuf);

    // when there is enough space in this buffer, don't read into extrabuf.
    // when extrabuf is used, we read 128k-1 bytes at most.
    // 这里之所以说最多128k-1字节，是因为若writable为64k-1，那么需要两个缓冲区 第一个64k-1 第二个64k 所以做多128k-1
    // 如果第一个缓冲区>=64k 那就只采用一个缓冲区 而不使用栈空间extrabuf[65536]的内容
    // ── iovcnt 为什么只有 1 或 2 两种取值（这里有个很值得推一遍的上限）────────────
    // 【设计】writable < 64KB  => 主缓冲区兜不住，把栈上的 extrabuf 也挂上去，iovcnt=2；
    //            writable >= 64KB => 主缓冲区本身已经比兜底段还大，只挂 vec[0]，iovcnt=1。
    // 【为什么这样设计而不是永远传 2】readv 会把所有段的长度加起来当"本次最多能读多少"，
    //            传 2 就意味着允许内核一次多塞 64KB 到栈上，而那 64KB 之后必须再 memcpy
    //            回缓冲区（第三分支）。当缓冲区已经够大时，这么做纯属自找麻烦。
    //            另外iov_cnt 传 0 就是读 0 字节返回 0，传负数是 EINVAL —— 只可能 1 或 2。
    // 【画出 128K-1 的推导】
    //            最坏情况：writable 刚好小于 65536，即 writable 最大取 65535（= 64K-1），
    //            此时 iovcnt=2，本次 readv 能接收的总长度上限 = writable + sizeof(extrabuf)
    //            = 65535 + 65536 = 131071 字节 = 128K-1。
    //            若 writable >= 64K（走 iovcnt=1），上限就是 writable 本身（可能远大于 128K）。
    //            结论："最多 128K-1"说的是"用到兜底段时"的上限，不是这个函数的绝对上限。
    // 【易错】这个数字不是 TCP 的限制，也不是内核缓冲区的限制，纯粹是本函数的设计选择。
    //            数据比这更多时靠 LT 的下一轮 EPOLLIN 继续读（或 ET 下外面套循环），
    //            所以永远不会丢数据，只是分成多次进入缓冲区。
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    // 【系统调用】::readv(fd, iov, iovcnt)：
    //            返回值三种情况：n > 0 实际读到的总字节数；n == 0 表示对端已经正常关闭
    //            （读到 EOF，收到 FIN 且接收队列已空）；n == -1 出错并设 errno。
    //            常见 errno：EAGAIN/EWOULDBLOCK（非阻塞且当前没数据）、EINTR（被信号打断，
    //            本项目不重试）、EBADF（fd 无效或已关，多半是 ⑪ 之后还有人调它）、
    //            EFAULT（iov_base 指向非法内存 —— 本函数不可能，除非 buffer_ 已经被析构）、
    //            EINVAL（iovcnt 越界或iov_len 溢出）、ECONNRESET（对端 RST，或
    //            开了 SO_RCVTIMEO？不，这里没有）、EPERM/EDQUOT 之类不相关。
    //            是否阻塞：fd 非阻塞则不阻塞（⑤ accept4 已经保证 connfd 非阻塞）。
    // 【易错】n 是"两段合计"的长度，所以我们必须自己判断有多少落在 buffer_、
    //            多少落在 extrabuf —— 这正是下面三条分支要做的事。
    // 【设计】为什么不在这里判断 EINTR 然后重试：本项目整体都不处理 EINTR（没有装信号
    //            处理器，实际上不会被普通信号打断），教学版接受这个简化，但要能指出来。
    const ssize_t n = ::readv(fd, vec, iovcnt);

    // ── 三条返回值分支，逐条看含义 ──────────────────────────────────────────────
    //   分支一 n < 0 ：出错。不改任何下标，把 errno 交给调用方（数据一个字节都没进来）。
    //   分支二 n <= writable：全部落在 buffer_ 可写区里，extrabuf 一个字节都没用到。
    //            [ 8 | 垃圾 | 可读 |  n 字节  | 剩余可写 ]   extrabuf 未使用
    //   分支三 n > writable：buffer_ 被塞满，溢出的 n-writable 字节在栈上。
    //            [ 8 | 垃圾 | 可读 |  可写区被填满 ]  加上  [ extrabuf: n-writable 字节 ]
    // 【易错】分支二的条件是 <= 而不是 <：n 恰好等于 writable 时，内核刚好把主区填满、
    //            没碰 extrabuf，走"只移动 writer"这条便宜路径才是对的；写成 < 会让
    //            这个边界情况走成"writerIndex_=size 再 append(extrabuf, 0)"，结果也对
    //            但白跑一趟扩容判断。
    if (n < 0)
    {
        // 【设计】只保存 errno 就返回（n 原样返回 -1），不改下标 —— 缓冲区完全没变，
        //            调用方拿到 n == -1 会去走 handleError（打印 SO_ERROR）。
        // 【易错】本函数只在出错时写 saveErrno。如果调用方初始化成 0 就没事
        //            （TcpConnection::handleRead 里正是 int savedErrno = 0;）；
        //            若传进来的是未初始化的栈变量，n >= 0 时它会保持垃圾值 ——
        //            调用方只要不在成功路径读它也没事，但这是很容易中招的隐式契约。
        // 【⚠问题】n == 0（对端关闭）时也走不到这里，所以"对端关闭"要靠调用方
        //            判 n == 0 来处理（⑪ 的 handleClose 正是这么触发的），别把它当错误。
        *saveErrno = errno;
    }
    // 【设计】绝大多数小包都走这条路径：唯一做的事是移动 writerIndex_，
    //            没有任何内存拷贝、没有扩容、没有系统调用之外的开销。
    //            这就是"readv 直接写进缓冲区"设计的收益所在。
    // 【数字演算】writable=1024，读到 n=100：writerIndex_ 从 8 变成 108，
    //            readable 变 100，writable 变 924，reader 不动（还是 8）。
    else if (n <= writable) // Buffer的可写缓冲区已经够存储读出来的数据了
    {
        // 【语法】n 是 ssize_t（有符号），writable 是 size_t（无符号）：
        //            writerIndex_ 加 n 时 n 会转成 size_t。因为这条分支已保证 n >= 0，
        //            转换安全。反例是把 n 用在减法或和有符号数比较上就会踩符号坑
        //            —— 这也是本函数写成 n <= writable 而不是 writable - n 之类的原因。
        writerIndex_ += n;
    }
    // 【设计】走到这里说明：内核把 buffer_ 的可写区完全填满了（writer 已顶到 size），
    //            还多出 n-writable 个字节躺在栈上的 extrabuf 里。
    //            数据顺序天然是"先 buffer_ 段、再 extrabuf 段"，与 TCP 流的顺序一致，
    //            所以下面只要把 extrabuf 的开头 n-writable 字节原样追加到 buffer_ 尾部，
    //            缓冲区里的字节序就完全正确 —— 这也是用 readv 而不是"读两次"的原因之一。
    // 【易错】绝不能"丢掉多出来的部分"：TCP 是字节流，被丢掉的那段永远不会再被内核送
    //            （已经从接收队列里取走了）。数据会莫名其妙少一截，协议直接错位。
    // 【数字演算】writable=24，读到 n=1000：buffer_ 里落 24 字节（writer 顶到 1032=size），
    //            extrabuf 里落 976 字节；下面 writerIndex_ 保持 1032，
    //            append(extrabuf,976) 触发 ensureWritableBytes(976) => makeSpace：
    //            此刻 writable=0，prepend=reader（比如 8）=> 左边 0+8=8，右边 976+8=984，
    //            8 < 984 成立 => 走 if 分支 resize(writer + 976 = 1032 + 976 = 2008)。
    //            搬完 writer=2008，readable = 2008-8 = 2000（原来的可读加上这次 1000）。
    else // extrabuf里面也写入了n-writable长度的数据
    {
        // 【设计】为什么是"等于 size"而不是"加 writable"：readv 已经把 [writer, size)
        //            整段填满了，此刻"数据终点"就是缓冲区末尾，两者相等。
        //            这一步的意义是让 append 看到 writableBytes()==0，从而必然去 makeSpace。
        // 【易错】这句必须在 append 之前：append 会按当前 writer 定位目的并再加 len，
        //            如果先 append 再改 writer，数据就会覆盖可读区。
        writerIndex_ = buffer_.size();
        // 【设计】这里复用 append 而不是手写 std::copy：ensureWritableBytes 加 copy
        //            加 移动 writer 这套顺序已经被验证过，重复使用它就不会在第二处
        //            忘了做扩容（"自己写一份"最容易漏的就是这一步）。
        // 【易错】源指针是 extrabuf 的开头：内核是从 iov_base 起始处开始写 vec[1] 的，
        //            长度恰好 n-writable，所以取"extrabuf 起 n-writable 字节"绝对正确，
        //            不需要任何偏移。
        // 【设计】append 会 memcpy 一次（O(n-writable)），这是"用 64KB 栈空间换
        //            不丢数据"的代价 —— 只有真的溢出才付，且付的是一次连续拷贝。
        // 【易错】append 内部可能重新分配 buffer_ 的内存，所以任何在这之前从
        //            peek()/begin() 拿到的指针在这之后都失效。extrabuf 在栈上不受影响。
        // 【流程】⑧：这一步做完，onMessage 拿到的 inputBuffer_ 就已经含全部数据了，
        //            用户完全感知不到"曾经溢出到栈上"这回事 —— 这就是 Buffer 抽象的价值。
        append(extrabuf, n - writable); // 对buffer_扩容 并将extrabuf存储的另一部分数据追加至buffer_
    }
    // 【设计】返回的是"从 fd 读到的总字节数"（含落到栈上的那部分），不是"缓冲区里
    //            可读字节数的增量"。两者在本函数里恰好相等（溢出部分最终也进了缓冲区），
    //            但概念要分清。调用方 handleRead 只用它判断 大于0 / 等于0 / 小于0 三路。
    // 【易错】n 可能大于调用方以为的"缓冲区容量"：所以上层不要拿 n 去做任何
    //            有符号/无符号混合的算术。
    return n;
}

// inputBuffer_.readFd表示将对端数据读到inputBuffer_中，移动writerIndex_指针
// outputBuffer_.writeFd标示将数据写入到outputBuffer_中，从readerIndex_开始，可以写readableBytes()个字节
    // 【做什么】把可读区交给 ::write，能塞进内核发送缓冲区多少就塞多少。
    // 【谁调用】⑩ TcpConnection::handleWrite 里的 outputBuffer_.writeFd(channel_->fd(), &savedErrno)
    //            —— 触发条件是 EPOLLOUT（内核发送缓冲区有空位）。
    // 【线程】connfd 所属的 subLoop#N。
    // 【流程】⑩ 的第一步；紧接着调用方 retrieve(n) 把发走的部分消费掉，
    //            readableBytes()==0 时 disableWriting 并触发 writeCompleteCallback_。
    // 【设计】它只写不改下标（把"实际收走多少"回报给调用方）：
    //            1) 出错或短写时不丢数据 —— 数据还在 outputBuffer_ 里等下一次 EPOLLOUT；
    //            2) "消费进度"的语义归 TcpConnection 管（retrieve），Buffer 只管内存布局，
    //               职责切得很干净；readFd 之所以自己动 writer，是因为"读到的字节
    //               已经落进缓冲区里"，不动就没人知道边界在哪。
ssize_t Buffer::writeFd(int fd, int *saveErrno)
{
    // 【系统调用】::write(fd, buf, count)：对 socket 而言是"把数据拷进内核发送缓冲区"，
    //            不是"等对端收到"。返回值为：
    //              n > 0  内核接受了多少字节（可能远小于 count，叫"短写"）；
    //              n == 0 通常表示 count 为 0（本项目就是 readableBytes()==0 的情况）；
    //              n == -1 出错：EAGAIN/EWOULDBLOCK（缓冲区满且非阻塞，最常规）、
    //                    EPIPE / ECONNRESET（对端已关，SIGPIPE 风险见 writeFd 的【⚠问题】段）、
    //                    ENOTCONN、EBADF（⑪ 之后还有人写）、ENOBUFS、EINTR。
    // 【设计】为什么这里"直接一个 write 就够，不用循环也不用 writev"：
    //            1) 非阻塞 —— 因为 ⑤ accept4 时带了 SOCK_NONBLOCK，connfd 是非阻塞的，
    //               所以 write 绝不会挂起 loop 线程：塞不下就返回 EAGAIN，
    //               由 epoll 用 EPOLLOUT 再叫我们一次。这就是"⑨ 写完还要 enableWriting"
    //               这条链路存在的根本原因，也是"非阻塞是 Reactor 命脉"在发送侧的体现。
    //            2) 短写不是错误 —— 一次没发完的部分留在缓冲区里，下次 EPOLLOUT 再来，
    //               所以本函数一次不需要循环（原版 muduo 的 write 也这样，
    //               陈硕在书里说明"EPOLLOUT 会再来"）。
    //            3) 数据源是一段连续内存（可读区），不需要 scatter/gather。
    //               什么时候才需要 writev：想"一次发多段"（例如协议头 加 body 而不合并），
    //               或发送带边界的整帧（MSG more 之类）。原版 muduo 提供
    //               writev 与 WriteVec 走多段发送，本教学版没这个需求，所以用 write。
    // 【易错】源指针是 peek()（可读区起点，不是缓冲区起点）：retrieve 之后起点会变，
    //            所以每次都要重新算，不能缓存。长度是 readableBytes()，绝不能用
    //            buffer_.size()（那会把已读垃圾也发出去）。
    // 【⚠问题】SIGPIPE：向"已被对端 RST 或已关闭"的 socket 写数据，内核默认发 SIGPIPE，
    //            而它的默认动作是终止进程。本仓库没有任何一处真的调用 signal/sigaction 去
    //            忽略它（能搜到的只是注释里的说明文字，不是调用）
    //            （原版 muduo 会忽略 SIGPIPE），所以理论上存在"一次写就把服务打死"的风险。
    //            为什么本项目实际很少踩到：handleWrite 只在 EPOLLOUT 上被触发，
    //            而对端 RST 时 epoll 报的是 EPOLLHUP/EPOLLERR 加 EPOLLIN（走 handleClose）。
    //            但 sendfile 路径或竞态下仍可能撞上。正确做法两条：库初始化时
    //            signal(SIGPIPE, SIG_IGN)，或发送时用 send(fd, buf, len, MSG_NOSIGNAL)
    //            让它老老实实返回 EPIPE。这是"面试问起 muduo 为什么忽略 SIGPIPE"的答案。
    ssize_t n = ::write(fd, peek(), readableBytes());
    // 【设计】出错时和 readFd 一样只存 errno，不改任何下标：数据仍在 outputBuffer_ 里。
    // 【易错】EAGAIN 在这里是"完全正常"的结果（内核发送缓冲区暂时满了），不是错误；
    //            但调用方 handleWrite 现在把所有 n 小于 0 都当错误打 LOG_ERROR，
    //            所以压测时这条日志会刷屏 —— src/TcpConnection.cc 里 handleWrite 的语义问题，
    //            源头就是 writeFd 的返回值约定本身。
    if (n < 0)
    {
    // 【对比】与 readFd 的对应分支同一套约定：n 小于 0 才有 errno，其余情况不写。
        *saveErrno = errno;
    }
    // 【设计】返回值语义（和 readFd 对齐着记）：
    //              大于 0：内核收走了这么多，调用方 retrieve(n)；
    //              等于 0：可读区本来就空（readableBytes()==0），或 fd 特殊状态；
    //              小于 0：失败，errno 已存进 *saveErrno。
    // 【流程】⑩ 的判据：handleWrite 靠"是否发完（readableBytes 归零）"决定
    //            disableWriting、writeCompleteCallback_ 与 kDisconnecting 时的 shutdownInLoop。
    // 【易错】本函数不判断 SIGPIPE 相关的 EPIPE，也不重试 EINTR —— 与全库风格一致，
    //            交给上层按"错误"处理。读代码时记住：Buffer 只提供机制，不提供策略。
    return n;
}