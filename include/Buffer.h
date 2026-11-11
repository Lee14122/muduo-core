/* ===========================================================================
 * 【文件】include/Buffer.h
 * 【一句话】用一块连续的 vector<char> 加两个下标（readerIndex_ / writerIndex_），
 *          实现"能读能写、能腾空间、能扩容"的字节缓冲区，充当 TCP 收发的中转站。
 * 【Reactor 角色】Handler 的数据面 —— TcpConnection 的两个成员 inputBuffer_/outputBuffer_
 *          就是它的两个实例；Acceptor / Poller / EventLoop 都不碰它。
 * 【依赖】vector（底层内存）、string（retrieveAsString）、algorithm（std::copy）、
 *          stddef.h（size_t）；实现文件 Buffer.cc 再依赖 sys/uio.h 的 readv 与 unistd.h 的 write。
 * 【谁用它】⑧ TcpConnection::handleRead 用 inputBuffer_.readFd 往里填，然后
 *          messageCallback_(conn, &inputBuffer_, time) 把控制权交给用户，用户调
 *          retrieveAllAsString() 或自己 peek/retrieve 消费；⑨ 用户 conn->send → sendInLoop →
 *          outputBuffer_.append 往里写；⑩ handleWrite → outputBuffer_.writeFd 往外倒，
 *          内核收走多少就 retrieve 多少。
 * 【先修知识】1) 迭代器解引用再取地址（&*it）；2) vector 扩容会搬走整块内存（旧指针失效）；
 *            3) std::copy 与"区间重叠"的坑；4) TCP 是字节流，不保留消息边界。
 * 【难度】★★★★☆（全库最精妙的小类之一：三个下标不变量串起所有函数）【建议耗时】45 分钟
 * 【自检】a) 画出 buffer_ 的四段布局，说清 kCheapPrepend 到 readerIndex_ 之间是什么。
 *        b) 为什么 append 必须"先 ensureWritableBytes，再 std::copy 到 beginWrite()"？
 *           反过来写会出什么事？
 *        c) makeSpace 的两条分支各在什么时候走？用 len=800 与 len=1010 各演算一遍。
 * ========================================================================= */

#pragma once

// 【依赖】为什么选 vector<char> 而不是 string / 裸 new char[]：
//          1) 语义正确 —— 这里是"字节序列"，可能含 NUL、含二进制，不是文本；
//          2) 能自动 resize 并把旧数据整块搬走（这就是 makeSpace 里"扩容"的全部实现）；
//          3) 保证连续存储且容量可查（size()/capacity()），裸指针可以安全交给
//             ::write / ::readv / std::copy；
//          4) 析构自动 free，所以 Buffer 不写析构、也不需 noncopyable
//             （拷贝一个 Buffer 是合法且有意义的：深拷贝数据，没有 fd 这类共享资源）。
//          【易错】别换成 vector<bool>：它是位压缩特化，&*begin() 直接编译不过
//             （operator* 返回代理对象而非 bool 引用的地址）。这是本文件"为什么这么写"
//             的经典反面教材。
#include <vector>
#include <string>
// 【依赖】string：retrieveAsString 的返回值。std::string 能装 NUL 和二进制数据，
//          所以"把可读区转成 string"不会丢内容 —— 前提是长度要显式给出（见那个函数）。
#include <algorithm>
// 【依赖】algorithm：std::copy —— append 拷入新数据、makeSpace 搬移可读区都靠它。
//          【易错】std::copy 要求源区间与目的区间不重叠；makeSpace 的搬移实际上是重叠的
//          （细节写在 makeSpace 旁边），严格合法的写法是 std::memmove。
#include <stddef.h>
// 【依赖】stddef.h 提供 size_t（下标与长度的类型）。
//          【易错】size_t 是无符号类型，这一点直接关系到下面三个长度函数的减法会不会
//          "下溢成天文数字"，是本类唯一致命的风险来源。

// ── 内存布局：一块连续数组，用两个下标切成四段 ──────────────────────────────────
//
//   字节偏移 →
//   +---------------+-----------------+-------------------+---------------------+
//   | kCheapPrepend | 已读垃圾区       | 可读区             | 可写区               |
//   |    8 字节      | [8, reader)     | [reader, writer)  | [writer, size())     |
//   +---------------+-----------------+-------------------+---------------------+
//   0            kCheapPrepend     readerIndex_      writerIndex_      buffer_.size()
//
//   最左边那 8 字节（0 到 7）是"永远预留、当前空着"的前插区，作用是让
//   prependableBytes() 恒大于 0（接口自洽），并让数据起点落在 8 字节对齐位置，
//   std::copy/memcpy 走对齐访问更快。
//   "已读垃圾区"是 retrieve 之后留下的旧数据：不清零、不搬走，只要 reader 前移就算消费掉了
//   —— 这就是"用下标代替 memmove"的核心技巧。
//
// 【三个长度，单位都是字节，不是消息条数】
//   readableBytes()    = writerIndex_ - readerIndex_      上层能取走多少
//   writableBytes()    = buffer_.size() - writerIndex_    尾部还能塞多少
//   prependableBytes() = readerIndex_                     头部能倒回去多少
// 【不变量】kCheapPrepend <= readerIndex_ <= writerIndex_ <= buffer_.size()
//   任何一条被破坏，上面三个减法都会因为 size_t 无符号而下溢，随后 std::copy / ::write
//   立刻越界 —— 所以本类所有函数都在"移动下标"而不是"移动内存"。
// 【设计】为什么不用环形缓冲区：环要处理"跨过末尾"的两段式读写（readv 能但代码复杂），
//   还要处理"写追上读"的满判定。muduo 选"连续 加 整体搬迁或扩容"，换来的是
//   "可读区永远是一整段连续内存"，于是 ::write、std::copy、以及 messageCallback 里的
//   const char* 全都简单直接。代价是偶尔多一次 memmove（makeSpace 的 else 分支）。
// 【易错】Buffer 不保留 TCP 消息边界：一次 readFd 可能读到 1.5 条消息，也可能只有半条。
//   "取一条完整消息"是应用层的责任（定长头 / 分隔符 / 状态机）。本项目 example 直接
//   retrieveAllAsString 全量读走，所以它只能演示 echo，不能演示协议解析。
// 【设计】Buffer 完全不感知线程：它属于某一条连接的 inputBuffer_ 或 outputBuffer_，
//   只有该连接所在 loop 的线程会碰它 —— 所以整个类没有任何锁、没有 atomic。
//   这是架构约定（one loop per thread）给的自由，不是这个类天生线程安全。
// 网络库底层的缓冲区类型定义
class Buffer
{
public:
    // 【设计】名字的意思是"便宜的前插区"：想要 prependable 空间不用 malloc、不用 memmove，
    //          一开始就留 8 字节 —— 这就是 cheap 的含义。
    //          用途：TCP 的长度头/魔数常常要在"已经收到的数据前面"补写（比如先收到 body
    //          再决定 4 字节长度头），有这段预留就能原地往前写而不用整体搬迁。
    //          8 的选值：一个 size_t/指针的天然对齐宽度，也刚好够放"4 字节长度 + 4 字节魔数"。
    // 【语法】static const size_t 允许类内直接初始化（size_t 是整型）。C++17 起 static
    //          数据成员隐式 inline，不需要在 .cc 里再定义一份。
    //          【易错】在 C++11/14 里，把它按引用传给 std::min 或 const T& 参数（ODR-use）
    //          会造成 undefined reference 链接错误，老代码常靠补一句类外定义解决。
    // 【⚠问题】本项目的 Buffer 没有 prepend() / retrieveUntil / 整数读写系列接口，
    //          所以这块预留区实际只被"三个长度计算 加 makeSpace 的搬移目标位置"用到，
    //          从来没被真正"前插"过，prependableBytes() 只剩形式意义。
    //          原版 muduo 有 prepend(data,len)、prepend(len, char)（补一个重复字符），
    //          并配合 kPrependIndicator / hasReadOffset 之类的约定防止误把前插标记当数据读。
    static const size_t kCheapPrepend = 8;//初始预留的prependabel空间大小
    // 【设计】默认容量 1024 字节：够装几个普通小包，又不至于每条连接都占大块内存。
    //          算账用的就是它：1 万条连接 * 2 个 Buffer * 1KB ≈ 20MB 起步（还没算 vector 开销）。
    //          注意它是"数据区初始大小"，实际分配还要再加 kCheapPrepend（见构造函数）。
    static const size_t kInitialSize = 1024;

    // 【做什么】分配 kCheapPrepend 加 initalSize 字节，两个下标都放到 8（数据区空）。
    // 【谁调用】⑥ TcpConnection 构造时作为成员 inputBuffer_/outputBuffer_ 一起被造出来 ——
    //            也就是说每条连接一建立就有两个 1KB 缓冲区。
    // 【线程】subLoop#N（连接属于哪个 loop 就在哪被构造；单线程模式下就是 mainLoop）。
    // 【流程】⑥ TcpConnection 构造（每条连接一建立就有 inputBuffer_ 与 outputBuffer_ 两个），
    //            是 ⑧⑨⑩ 数据通路的场地；它不参与 ①~⑤。
    // 【语法】explicit：禁止 Buffer b = 1024; 这种"凭空造一个临时缓冲区"的写法。
    // 【语法】buffer_(kCheapPrepend + initalSize) 走 vector 的"n 个元素"构造，
    //            会把每个 char 值初始化成 0，所以这里隐含一次 (8+1024) 字节的清零写入。
    // 【易错】initalSize 是原作者的拼写笔误（少一个 i，应为 initialSize）。它只是参数名，
    //            不影响行为，但你按 initialSize 全局搜索会搜不到这个类的这段。
    // 【设计】先记住一组初始数字，后面每个例子都从这里出发：
    //            size=1032，reader=writer=8，readable=0，writable=1024，prepend=8。
    explicit Buffer(size_t initalSize = kInitialSize)
        // 【语法】成员初始化列表按"类中声明顺序"执行，不按列表书写顺序。这里三个初始化
        //          彼此不依赖所以看不出问题，但这是 C++ 的通用陷阱：如果写成
        //          writerIndex_(readerIndex_)，而 readerIndex_ 声明在 buffer_ 之后、
        //          writerIndex_ 之前，取到的才是已初始化的值；顺序反了就是垃圾值。
        : buffer_(kCheapPrepend + initalSize)
        // 【设计】reader 与 writer 都从 8 开始：此刻可读区长度 0（空缓冲），
        //          可写区从 8 一直延伸到末尾，前插区是 [0,8)。
        , readerIndex_(kCheapPrepend)
        , writerIndex_(kCheapPrepend)
    {
    }

    // 【做什么】可读区字节数 = writer 减 reader；返回 0 表示"没数据可取"。
    // 【谁调用】⑨ sendInLoop（判 outputBuffer_ 是否空）、⑩ handleWrite（判是否发完）、
    //            ⑧ 用户 retrieveAllAsString、makeSpace 内部也算它。
    // 【线程】缓冲区所属的 subLoop#N。
    // 【流程】它是 ⑨/⑩ 两个方向的"是否有活要干"的判据，属于链路里被调用最多的函数之一。
    // 【易错】它假设 reader <= writer（不变量）。哪天有人写出"readerIndex_ += 超量"，
    //            size_t 减法下溢会返回接近 2^64 的巨值，随后 copy/write 直接越界崩溃，
    //            现场看起来像"随机 core dump"，根因却是下标被改坏 —— 这类 bug 的排查思路。
    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    // 【做什么】尾部可写字节数。减的是 buffer_.size()（当前已分配总字节数），
    //            不是 capacity() —— 本类把 size() 当"分配的总容量"用，靠 resize 变大，
    //            全程不读 capacity()，所以"writable"永远精确等于"还能写多少而不越界"。
    // 【谁调用】ensureWritableBytes（决定要不要 makeSpace）、readFd（定 vec[0].iov_len）、
    //            makeSpace 的分支判断。
    // 【易错】返回 0 不是"写不了了"，只是"要写得先腾地方或扩容"。
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    // 【做什么】头部能倒回去的字节数 = readerIndex_（0 号下标就是数组起点，所以二者相等）。
    //            它包含"预留的 8 字节"加"已读垃圾区"，所以随 retrieve 推进会越来越大。
    // 【谁调用】只有 makeSpace 的分支条件用到（"搬数据最多能腾出多少"）。
    // 【⚠问题】因为本类没有 prepend 接口，这个值永远不会被用来真的往前写数据
    //            （原版 muduo 用它实现 prepend，并在空间不足时 assert）。
    // 【易错】别以为"prependable 很大 就还有很多有效数据在前面"：[kCheapPrepend, reader)
    //            是垃圾，语义上禁止读，只有类内部的搬移会跨过它。
    size_t prependableBytes() const { return readerIndex_; }

    // 【做什么】返回可读区第一个字节的裸指针 —— peek 就是"看一眼但不取走"，下标不动。
    // 【谁调用】⑧ 用户 onMessage 里直接读 buf->peek()；retrieveAsString 用它构造 string；
    //            ⑩ writeFd 用它当 ::write 的数据源。
    // 【线程】缓冲区所属的 subLoop#N。
    // 【流程】⑧ 的"数据交付"入口。让用户在原地解析而不复制，这正是 muduo 把
    //            messageCallback 的参数定成 Buffer* 而不是 std::string 的理由：省一次拷贝。
    // 【易错】返回的裸指针会被这些操作弄失效：append、ensureWritableBytes、makeSpace
    //            （其中扩容会 realloc，旧地址彻底作废）。所以"先取 peek() 存着，再往里 append"
    //            是错的；每次要用都得重新调 peek()。
    // 【易错】peek() 指向的合法长度只有 readableBytes()。跨过 writerIndex_ 就是
    //            未初始化数据（在 vector 里是 0，在扩容后的新内存里也是 0，但语义上是垃圾）。
    // 返回缓冲区中可读数据的起始地址
    const char *peek() const { return begin() + readerIndex_; }
    // 【做什么】"我已经消费掉前 len 字节" —— 只把 reader 前移，不清零、不搬内存。
    // 【谁调用】⑩ TcpConnection::handleWrite 里 ::write 成功之后 outputBuffer_.retrieve(n)
    //            （内核收走多少就丢多少）；retrieveAsString 内部也调它。
    // 【线程】缓冲区所属的 subLoop#N。
    // 【流程】⑩ 的收尾动作，配合"readableBytes 是否为 0"决定 disableWriting。
    // 【设计】关键在"只动下标"：垃圾留在原地。retrieve 时立刻 memmove 会让每次消费
    //            都变成 O(n)，那就退化成了队列。把 O(n) 的代价推到"真的需要空间时"
    //            （makeSpace）才付，这是本类最重要的一个设计决定。
    // 【易错】len 大于 readableBytes() 时不会报错，而是掉进 else 调 retrieveAll()
    //            把整个缓冲区清空 —— "多消费一点"被静默升级成"全部丢弃"。
    //            原版 muduo 这里是 assert(len <= readableBytes())，越界立刻崩给你看；
    //            本项目没有断言（而且 NDEBUG 下 assert 本来也会消失），调用方得自己保证。
    void retrieve(size_t len)
    {
        // 【设计】用严格小于：len 恰好等于 readableBytes() 时会走 else，效果与"全读走"一致，
        //            而且 else 里把 writer 也归零，语义更干净（避免 reader==writer 但都不在 8
        //            的"空但偏移"状态 —— 那样下次 append 还是从老位置写，垃圾区一直不回收）。
        if (len < readableBytes())
        {
            // 【数字演算】从初始态（size=1032，reader=writer=8）出发：
            //            先 append 1000 字节 => reader=8，writer=1008，readable=1000；
            //            再 retrieve(300)   => reader=308，writer=1008；
            //            此时 readable=700，writable=1032-1008=24，prepend=308。
            //            [8,308) 这 300 字节成了"已读垃圾"，仍躺在内存里，没人管它。
            // 【易错】注意 writable 只有 24：消费数据并不会让"尾部可写空间"变大，
            //            要让空间变大得等 append 触发 makeSpace 去搬。
            readerIndex_ += len; // 说明应用只读取了可读缓冲区数据的一部分，就是len长度 还剩下readerIndex+=len到writerIndex_的数据未读
        }
        // 【易错】注释写的是 "len == readableBytes()"，真正的条件是 len >= readableBytes()：
        //            多出来的 len 被吞掉。读注释要自动修正成"只要不是严格小于就全清"。
        else // len == readableBytes()
        {
            retrieveAll();
        }
    }
    // 【做什么】两个下标一起回到 8：可读区清零、垃圾区回收成可写区，"看起来像刚构造一样空"。
    // 【谁调用】retrieve 的 else 分支；上层"这条消息我不要了/全读完了"时也可直接调。
    // 【线程】subLoop#N。【流程】⑧ 用户全量消费 与 ⑩ 发送完毕后的等价状态。
    // 【设计】重要认知：retrieveAll 不释放任何内存！size 仍是 1032（或历史最大值），
    //            下次 append 照旧写在这块内存上 —— "空缓冲区"与"小缓冲区"是两回事。
    // 【易错】writerIndex_ 被拉回 kCheapPrepend，writableBytes() 一下变回整个容量；
    //            如果外面还缓存着之前从 peek()/begin() 拿的指针，那块内容物理上还在，
    //            但逻辑上已经不属于可读区了（下次 append 会覆盖它）。
    void retrieveAll()
    {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    // 【做什么】把当前可读区一次性转成 std::string 并清空缓冲区（拷贝一份数据出去）。
    // 【谁调用】example/testserver.cc 的 onMessage（⑧）—— echo 语义"全读全发"就靠它。
    // 【线程】subLoop#N（在用户回调栈上）。【流程】⑧。
    // 【易错】它是"一次性全拿"：不知道协议边界时最省事，但它会连同上一条消息的尾巴
    //            一起给你；用它做协议解析必须配合定长头或分隔符自己判长度。
    // 【设计】返回 string 意味着一次堆分配 加 一次 memcpy；高频协议解析应改用
    //            peek() 加 retrieve() 原地处理，才是 muduo 推荐的零拷贝姿势。
    // 把onMessage函数上报的Buffer数据 转成string类型的数据返回
    // 【语法】一行里串两个成员调用，等价于 return retrieveAsString(this->readableBytes());
    // 【易错】readableBytes() 是在调用那一刻算的，之后 retrieveAsString 里又算一次
    //            peek/retrieve —— 单线程内没有竞争所以一致；如果哪天有人在两个 loop 里
    //            共享一个 Buffer（违反架构约定），这里就是一个 TOCTOU 竞态点。
    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }
    // 【做什么】"取 len 字节变成 string" 加 "retrieve(len) 消费掉它们"，两步捆成一个函数。
    // 【谁调用】retrieveAllAsString（传全部可读长度）；上层知道消息长度时传精确值更好。
    // 【线程】subLoop#N。【流程】⑧。
    // 【设计】顺序绝不能反：先构造 result（从 peek() 拷 len 字节），再 retrieve(len)
    //            移动下标。反过来写的话 result 拷到的就是已经被"跳过"的数据。
    // 【易错】这里用的是 std::string(const char*, size_t) 构造，而不是 std::string(peek())：
    //            后者会在第一个 NUL 处停下，网络数据里的 0 字节会直接把消息截断 ——
    //            二进制协议必错。这是"两个看起来一样的构造"的实质差别。
    // 【易错】len 大于可读长度时：先拷贝（越界读！）再 retrieveAll 清空 —— 原版有
    //            assert(len <= readableBytes()) 兜这条，本项目没有，调用方要保证。
    std::string retrieveAsString(size_t len)
    {
        std::string result(peek(), len);
        // 【设计】"复位"的准确含义：reader 前移 len，让这 len 字节变成垃圾区，
        //            下次 append 才能覆盖或跨过它。不复位的话同一段数据会被反复投递。
        retrieve(len); // 上面一句把缓冲区中可读的数据已经读取出来 这里肯定要对缓冲区进行复位操作
        return result;
    }

    // 【做什么】保证"尾部至少还能写 len 字节"，不够就交给 makeSpace 腾地方。
    // 【谁调用】append（每次写入前）、readFd 的溢出分支（经由 append 间接调用）、
    //            上层想先占一段空间再原地填数据时（配合 beginWrite）。
    // 【线程】subLoop#N。【流程】⑧ 的溢出路径与 ⑨ 的 sendInLoop 都可能走到。
    // 【设计】"检查 与 干活"分成两个函数是刻意的：热路径（空间够）一次比较就走，
    //            零额外开销；冷路径才付搬移/扩容的代价。
    // 【易错】它是 Buffer 里唯一会让内存块换地址的公开入口（经由 makeSpace 的 resize）。
    //            任何"先前取出的裸指针"在这次调用之后都不可信。
    // buffer_.size - writerIndex_
    void ensureWritableBytes(size_t len)
    {
        // 【设计】严格小于才腾空间：恰好等于时不折腾，省掉一次无谓的搬移。
        // 【数字演算】size=1032，reader=8，writer=1008（readable=1000，writable=24）：
        //            ensureWritableBytes(24) => 24 < 24 为假 => 什么都不做，正好填满；
        //            ensureWritableBytes(25) => 25 > 24 => 进 makeSpace，走哪条分支见那边。
        if (writableBytes() < len)
        {
            // 【易错】原注释只写"扩容"，其实 makeSpace 有两条分支：另一条根本不扩，
            //            只是把可读区前移到偏移 8，把已读垃圾变成可写空间。
            makeSpace(len); // 扩容
        }
    }

    // 【做什么】把外部 len 字节追加进可写区，然后 writer 前移 —— Buffer 唯一的写入入口。
    // 【谁调用】⑨ TcpConnection::sendInLoop 里 outputBuffer_.append((char*)data + nwrote,
    //            remaining)（把一次没发完的剩余数据放进缓冲区）；⑧ readFd 的溢出分支
    //            append(extrabuf, n - writable)；上层自定义协议写数据也用它。
    // 【线程】subLoop#N。【流程】⑨ 的"写入发送缓冲区"，以及 ⑧ 的"把栈上溢出数据搬回缓冲区"。
    // 【设计】这三行的顺序是本类最要紧的知识点：先 ensureWritableBytes，再 std::copy，
    //            最后才动 writerIndex_。为什么必须先 ensure，见 append 函数体内
            //            ensureWritableBytes(len) 调用点上方的那条【设计】注释。
    // 【易错】data 不能指向本 Buffer 的可读区（std::copy 不处理重叠）。要"原地加工"
    //            自己的数据，得另开缓冲区或显式用 memmove。
    // 把[data, data+len]内存上的数据添加到writable缓冲区当中
    void append(const char *data, size_t len)
    {
        // 【设计】为什么必须在 std::copy 之前：ensureWritableBytes 可能触发 makeSpace 里的
        //            buffer_.resize；vector 一旦重新分配，整块内存搬到新地址、旧地址被 free。
        //            如果先算好目的指针 beginWrite() 再 ensure，拿到的就是指向已释放内存的
        //            野指针，std::copy 会写进已被回收的堆块 —— 表现为偶发堆破坏、
        //            随后几条语句里崩溃，或很久之后崩在别处，而且 release 版更容易出现、更难复现。
        //            现在的写法天然安全：beginWrite() 是作为 std::copy 的第三个实参求值的，
        //            求值时刻在 ensure 之后，拿到的指针必然属于新的内存块。
        // 【易错】反过来说："ensure 之后、copy 之前"也不能插入任何可能扩容的调用。
        //            这就是为什么 append 要写成三行紧凑的形式，而不是留给调用方去拆。
        ensureWritableBytes(len);
        // 【语法】std::copy(源首, 源尾后, 目的首)：三个指针、两个区间；目的端不需要预先
        //            构造（char 是平凡类型，直接覆盖内存），等价于 memcpy(目的, 源, len)，
        //            编译器会把它优化成向量化 memcpy，性能与 memcpy 同级。
        //            为什么用 std::copy 而不是 memcpy：语义上"目的区间容量已经保证"，
        //            且不写 sizeof(char)，类型安全 —— 这是"零成本抽象"的正例。
        // 【易错】data+len 是"尾后指针"（C++ 的半开区间约定 [data, data+len)），
        //            它不是有效元素，但必须能构造出来（指针加到数组末尾是合法的）。
        std::copy(data, data+len, beginWrite());
        // 【设计】必须放在 std::copy 之后：数据先落地，下标再"承认"。
        //            反过来的话，中间一旦有异常/提前返回，可读区里就混进了未初始化数据，
        //            上层 peek 到的就是垃圾。
        // 【易错】len 已由 ensureWritableBytes 保证不会越过 buffer_.size()，所以这一行
        //            永不越界；但如果有人绕过 append 直接改 writerIndex_，就得自己重新
        //            证明这件事 —— 这是"把不变量集中在一处维护"的实际收益。
        writerIndex_ += len;
    }
    // 【做什么】可写区起点。此刻 writerIndex_ 还没动，所以返回的正是"第一个可写字节"。
    // 【设计】为什么要把裸指针接口暴露出来（虽然是 public，但主要给类内和 readFd 用）：
    //            readFd 需要 vec[0].iov_base 直接指向可写区，让内核把数据原地写进缓冲区，
    //            省掉"先读进临时区再 append"的第二次拷贝 —— 这就是 muduo 收发路径
    //            "小报文零额外拷贝"的落点。上层若已知要写多大，也可以先 ensure 再拿这个
    //            指针原地填（比如自己拼协议头），填完自己加 writerIndex_（本项目没给这个
    //            接口，所以要走 append）。
    // 【语法】非 const 与 const 两个 beginWrite 重载成对出现，和 begin() 的两个重载配套：
    //            非 const 对象拿到"可写指针"，const 对象只能拿到"只读指针"，由编译器挑版本。
    // 【流程】⑧ readFd 用它定 vec[0].iov_base（内核原地写入）；⑨ append 用它定拷贝目的地。
    // 【线程】subLoop#N。
    char *beginWrite() { return begin() + writerIndex_; }
    // 【语法】const 版本给"只读场景"用（对 const Buffer& 也能取到可写区起点看内容）。
    //            本项目实际调用点都在非 const 路径上，保留它是为了重载对称。
    // 【流程】只读场景（例如诊断打印可写区内容）；不在 ⑧⑨⑩ 的写路径上。
    // 【线程】subLoop#N。
    const char *beginWrite() const { return begin() + writerIndex_; }

    // 从fd上读取数据
    // 【做什么】一次 ::readv 把 fd 上可读的数据取进缓冲区（放不下时溢出到栈上临时区）。
    // 【谁调用】⑧ TcpConnection::handleRead 里的 inputBuffer_.readFd(channel_->fd(), &savedErrno)。
    // 【线程】connfd 所属的 subLoop#N。【流程】⑧ 的第一步，也是"数据进入用户视野"的入口。
    // 【设计】返回值用 ssize_t（可负）而不是 size_t：负数表示出错，0 表示对端关闭
    //            （FIN 被读到），正数是实际读到的字节数。上层 handleRead 正是按这三路分派的
    //            （>0 调 messageCallback_，==0 调 handleClose，<0 记 errno 调 handleError）。
    // 【设计】为什么错误要用"出参 saveErrno"而不是直接读 errno：readv 之后可能又有别的
    //            调用（比如 return n 路上的构造/日志）改写 errno，先存下来才不会被污染。
    ssize_t readFd(int fd, int *saveErrno);
    // 通过fd发送数据
    // 【做什么】一次 ::write 把可读区能塞进内核的部分全塞出去。
    // 【谁调用】⑩ TcpConnection::handleWrite 里的 outputBuffer_.writeFd(channel_->fd(), &savedErrno)。
    // 【线程】subLoop#N。【流程】⑩ 的第一步。
    // 【设计】为什么它不需要"腾空间/扩容"这类操作：写方向只移动 reader，不动内存布局，
    //            也不向缓冲区塞任何东西，所以接口面小得可怜（实现见 Buffer.cc，只有 8 行）。
    ssize_t writeFd(int fd, int *saveErrno);

private:
    // ── private 的两个 begin() 与 makeSpace：把"能拿到缓冲区基址"这件事藏起来 ──────────
    // 【设计】外部只能通过 peek / beginWrite / append / retrieve 这些"带下标语义"的接口
    //            接触内存：拿到裸指针也只能读，改不了 size，也改不了两个下标 ——
    //            不变量由类自己在唯一几处维护。这才是"封装"在本文件的实际价值，
    //            不是形式上的 private。
    // 【做什么】返回底层数组首字节地址；类内所有 begin() 加下标 的写法都以它为基准。
    // 【语法】为什么写 &*buffer_.begin() 而不是 &buffer_[0] 或 buffer_.data()：
    //            这里第一个字符 & 是"取地址"，紧跟着的 * 是"迭代器解引用"（operator*），
    //            它返回首元素的引用；引用再取地址就是数组首地址。
    //            读作"取 operator* 结果的地址"，绝不是"取 begin 函数本身的地址"。
    //            1) &buffer_[0]：operator[] 不做边界检查，对空 vector 传 0 是未定义行为；
    //               对非空 vector 与 &*begin() 完全等价。
    //            2) buffer_.data()：C++11 才有，语义最清楚（标准明确保证
    //               [data(), data()+size()) 是一段连续内存），现代写法应该用它。
    //            3) 历史原因：这段代码沿用 muduo 早期写法，那时 vector 还没有 data()，
    //               &*begin() 是唯一可移植的表达；它和 std::string 的 &*s.begin() 长得一样，
    //               所以两种容器读起来看不出差别 —— 但本项目这里是 vector<char>，
    //               不是 string，别把两个类型混为一谈。
    // 【易错】buffer_ 永远非空（构造就分配 8 加 1024 字节），所以三种写法今天都没问题；
    //            但若哪天有人加一个"默认构造不分配内存"的 Buffer，&*begin() 立刻变成 UB。
    //            这也是"把它写成私有函数、而不是在十处地方裸写"的意义：要改只改一处。
    // 【易错】两个重载返回的都是"整个数组的首地址"，不是可读区/可写区地址；
    //            所以 peek() 要加 readerIndex_，beginWrite() 要加 writerIndex_。
    // vector底层数组首元素的地址 也就是数组的起始地址
    // 【语法】非 const 版：返回可写的 char*，append/makeSpace/readFd 用它做 memcpy 端点。
    char *begin() { return &*buffer_.begin(); }
    // 【语法】const 版：给 const Buffer 用（例如把 Buffer 当 const& 传给某个只读函数时）。
    //            两个版本函数体一模一样，只差返回类型 —— 这是"const 重载"的标准套路。
    // 【流程】被 peek() 与两个 beginWrite() 以及 makeSpace 的搬移共用，
    //            因此 ⑧⑨⑩ 三条路径都在用它，只是自己不感知方向。
    // 【线程】缓冲区所属的 subLoop#N。
    const char *begin() const { return &*buffer_.begin(); }

    // 【做什么】"保证尾部能连续写下 len 字节"这件事的唯一实现，两条分支：
    //            分支 A（if）：垃圾全回收也不够 => resize 向堆多要内存（只增不减）；
    //            分支 B（else）：把可读区整体前移到偏移 8 => 一次 memmove，不花新内存。
    // 【谁调用】只被 ensureWritableBytes 调用，因此入口是 append（⑨ 发送、⑧ 溢出）。
    // 【线程】subLoop#N。【流程】⑧ readFd 溢出分支 与 ⑨ sendInLoop 大报文都会走到；
    //            它是 Buffer 里唯一会"搬家 或 换内存块"的函数，也是所有指针失效的根源。
    // 【设计】分支条件的推导（把它当不等式看一遍就记住了）：
    //            搬完之后能给出的最大连续可写空间
    //              = buffer_.size() - kCheapPrepend - readable
    //              = size - 8 - (writer - reader)
    //              = (size - writer) + reader - 8
    //              = writableBytes() + prependableBytes() - kCheapPrepend
    //            要求它 >= len，移项就得到 makeSpace 里 if 的判断式（把 >= 取反成 <）。
    //            所以 if 的读法是："连把垃圾全回收都不够 => 只能向堆多要内存"。
    void makeSpace(size_t len)
    {
        /**
         * | kCheapPrepend |xxx| reader | writer |                     // xxx标示reader中已读的部分
         * | kCheapPrepend | reader ｜          len          |
         **/
        // 【说明】上面原作者的块注释画的就是这两条分支的布局图，其中 xxx 指"已读垃圾区"：
        //            分支 A 把 len 直接加在 writer 后面（不搬）；
        //            分支 B 把 reader 挪回 8，让垃圾区并入可写区。
        // 【数字演算】统一从初始 size=1032 出发，两种局面：
        //   局面一：缓冲区从没被消费过（reader=writer=8，垃圾区为 0），要写 len=1030
        //     左边 = writable(1032-8=1024) + prepend(8) = 1032
        //     右边 = len + kCheapPrepend = 1030 + 8 = 1038
        //     1032 < 1038 成立 => 走 if：resize(writer + len = 8 + 1030 = 1038)。
        //     扩容后 writer=8、size=1038 => writable = 1030，正好够；垃圾区本来就没有，
        //     搬也腾不出额外空间，所以"只能扩"是最省的解。
        //   局面二：消费过一半（reader=508，writer=532，readable=24），要写 len=800
        //     左边 = writable(1032-532=500) + prepend(508) = 1008
        //     右边 = 800 + 8 = 808      => 1008 < 808 不成立 => 走 else：
        //     把 24 字节搬到偏移 8 => reader=8，writer=32，writable=1000 >= 800，完全不扩内存。
        //   同一局面要写 len=1010：右边 = 1010 + 8 = 1018 > 左边 1008 => 走 if，
        //     resize(532 + 1010 = 1542)。
        // 【易错】右边那一项 "+ kCheapPrepend" 最容易被看漏：它的作用就是把 [0,8) 这段
        //            预留从"可回收空间"里扣掉。少写它，else 分支就会把 reader 搬到 0，
        //            破坏"prependable 恒不小于 8"的约定，将来 prepend 类接口就无处可写。
        // 【易错】这个判断只看"够不够"，不看"浪不浪费"：局面二里明明只有 24 字节有效数据，
        //            但只要 else 能腾开就绝不扩 —— 这就是它比"直接 resize 到 size 加 len"
        //            聪明的地方（内存占用不会因为一次大 append 就永久变大，除非真没办法）。
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) // 也就是说 len > xxx前面剩余的空间 + writer的部分
        {
            // 【设计】目标为什么定成 writerIndex_ + len（而不是"翻倍"或"size 加 len"）：
            //            这样扩容后 writableBytes() 恰好等于 len，一步到位不浪费；
            //            而且 writer 之前的数据一个字节都不用动（连搬都省了）。
            //            注意它不是"size + len"：那会把已读垃圾也算进新容量里。
            // 【易错】vector 的 resize 到更大值时，底层 capacity 策略仍可能多要一些
            //            （按 2 倍增长），所以"新 size"不等于"新 capacity"；
            //            本类只认 size，所以逻辑完全确定，不会因为 capacity 猜错而出 bug。
            // 【系统调用】resize 变大会：分配新块、把旧数据整块 memcpy 过去、把新增部分
            //            值初始化（写 0）、释放旧块 —— 一次 O(size) 的开销 加 可能一次 mmap。
            //            这是本类最贵的一条语句，所以 ensureWritableBytes 的"一次比较"
            //            热路径才那么重要。
            // 【⚠问题】只增不减（本条是全库最需要知道的退化之一）：
            //            1) 这个分支只会把 size 变大（走到这里说明 len 超过当前能腾出的空间，
            //               所以 writer + len 必然大于旧 size）；
            //            2) 全类没有任何一处把 size 缩回去：retrieveAll 不动 size，
            //               只有析构才 free。
            //            后果：一条长连接偶然收到一个 8MB 的报文（或用户一口气 send 了 8MB），
            //            这 8MB 就一直挂在它身上，直到连接关闭；1 万条这种连接就是 80GB 级的
            //            "已经没用了但不还给你的内存"。缓解手段（真实项目里要做）：
            //            a) 在 else/resize 之后加收缩阈值，比如 size 大于 1MB 且 readable
            //               很小（retrieveAll 时）就 resize 回 kInitialSize；
            //            b) 大报文走流式路径（sendfile 或分片 append），不要整块进 Buffer；
            //            c) 靠高水位回调（TcpConnection 的 highWaterMarkCallback_）让应用
            //               自己少发 —— 本项目有这条，能限制发送侧，但收侧仍不受限。
            // 【对比原版】标准 muduo 的 makeSpace 同样只 resize 增长、不 shrink，
            //            陈硕的做法是"用连接的生命周期兜底"（连接断了 Buffer 就析构）。
            //            所以规范里那句"原版有 shrink 阈值"要谨慎对待：背知识点时
            //            说"原版也没有，需要自己加"更稳妥，面试被追问才不至于答错。
            buffer_.resize(writerIndex_ + len);
        }
        // 【设计】else 分支就是"不花内存、只花一次 memcpy"的解法：垃圾区并入可写区，
        //            可读区整体前移到偏移 8（对齐位置）。原注释里的 xxx 就是那块垃圾。
        // 【易错】进入 else 时 readerIndex_ 必然大于 kCheapPrepend（否则左边就等于
        //            writable + 8，不可能 >= len + 8 除非本来就不用腾 —— 那种情况
        //            ensureWritableBytes 根本不会调进来），所以这次搬移几乎总是真的在搬东西。
        else // 这里说明 len <= xxx + writer 把reader搬到从xxx开始 使得xxx后面是一段连续空间
        {
            // 【设计】先算 readable 再动下标：下面重算 writerIndex_ 要用"搬移前的可读长度"，
            //            而此时 readerIndex_ 马上就要被改掉。用局部变量把这一刻的长度钉住，
            //            是这类"就地搬移"代码的通用套路，不需要额外内存。
            // 【易错】注释 "readable = reader的长度" 表述不准：readable 是"可读区长度"
            //            （writer 减 reader），不是 reader 那一段的长度。读代码时按
            //            可读区理解才不会算错。
            size_t readable = readableBytes(); // readable = reader的长度
            // 将当前缓冲区中从readerIndex_到writerIndex_的数据
            // 拷贝到缓冲区起始位置kCheapPrepend处，以便腾出更多的可写空间
            // 【易错】这是全文件技术上最"脏"的一行：源区间 [reader, writer) 与目的区间
            //            [8, 8+readable) 是可能重叠的（当 8+readable > reader，即已读垃圾
            //            比可读数据短时重叠）。C++ 标准要求 std::copy 的两个区间不重叠，
            //            重叠即未定义行为。这里恰好是"向左前移"（目的地址更小），
            //            按地址递增实现时逐字节从左侧开始写，不会覆盖还没读到的源数据，
            //            所以事实上能正常工作 —— 但它是"依赖实现细节"的正确性。
            //            严格合法的写法是 std::memmove(begin()+kCheapPrepend,
            //            begin()+readerIndex_, readable)，它保证任意方向的重叠都对。
            //            原版 muduo 这里也是 std::copy（同一个问题一起带着），所以这不是
            //            本教学版的退化，而是"读代码要能识别隐患"的样本。
            // 【数字演算】局面二：源 [508,532)（24 字节），目的偏移 8；搬完 8 到 32 是新数据，
            //            508 到 532 变成垃圾；reader=8，writer=32，writable=1000。
            // 【语法】三行写成多行只是为了对齐可读（参数是三个 char*），
            //            逗号表达式顺序不能改：先源首、再源尾后、最后目的首。
            std::copy(begin() + readerIndex_,
                      begin() + writerIndex_,
                      begin() + kCheapPrepend);
            // 【设计】reader 归位到 8 而不是 0：保住"前插区永远留 8 字节"这个约定
            //            （这也是 if 条件里加 kCheapPrepend 的那一项换来的保证）。
            // 【易错】两句顺序不能反：writerIndex_ 要用 readable（旧的可读长度）重算，
            //            而这里已经把 readerIndex_ 改掉了 —— 所以下面用的是
            //            readerIndex_ + readable 而不是 writerIndex_ 加什么。
            readerIndex_ = kCheapPrepend;
            // 【设计】writer = 8 加 可读长度，于是不变量重新成立：
            //            reader(8) <= writer(8+readable) <= size，且 readableBytes()
            //            仍然是那个 readable —— 数据一个字节都没丢，只是换了位置。
            //            到这里 makeSpace 的契约（尾部有 len 个连续可写字节）已经兑现，
            //            回到 append 里就可以安全 std::copy 了。
            // 【易错】写成 writerIndex_ += readable 是错的（漏掉前移量）；
            //            写成 writerIndex_ = kCheapPrepend + readable 与现在等价但绕了一下。
            writerIndex_ = readerIndex_ + readable;
        }
    }

    // 【设计】唯一的一块内存。它是值成员，所以拷贝 Buffer 会深拷一份（合法但昂贵；
    //            本项目 TcpConnection 是 noncopyable，没人真去拷 Buffer）。
    // 【易错】buffer_.size() 是"已分配总字节数"，不是"有效数据量"；有效数据只看两个下标。
    //            这条区分一旦混了，writableBytes 与 readableBytes 的算术全乱。
    std::vector<char> buffer_;
    // 【设计】两个下标就是 Buffer 的全部状态：类里没有任何"缓存下来的长度"，
    //            所以不存在一致性问题，所有长度都是即时算出来的。
    // 【线程】属于它所在连接的 subLoop#N，永不跨线程访问 —— 所以不用 atomic、不用锁。
    size_t readerIndex_;
    // 【设计】writer 只被四种操作改动：append（加 len）、retrieveAll（归 8）、
    //            makeSpace 的搬移（重算成 8 加 readable）、readFd 的两条成功分支。
    //            把这四处当"状态迁移表"背下来，整个 Buffer 就通了。
    size_t writerIndex_;
};
