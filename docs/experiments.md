# muduo-core 动手实验与面试自测

> 配套文件：docs/prerequisites.md（概念补课）、各源文件里的【流程】/【线程】注释（全链路 ①→⑫）。
> 本文**只用「文件名 + 函数名/变量名」定位，不写行号**；所有命令在 WSL 里、仓库根目录执行：
>
> ~~~bash
> cd /home/lee/projects/muduo-core
> ~~~
>
> 涉及改代码的实验，**只在副本或你自己新建的文件里改**；本仓库注释工作正在进行中，
> ⚠️ 不要随手 git checkout / git restore：当前工作区里有大量未提交内容（包括本次新增的全部注释，以及你自己之前的改动），还原会把它们一起抹掉。要回退某个文件，先确认它已提交或已备份。
> 改前备份到 /tmp，改完从备份拷回。

---

## 0. 全链路速查表（gdb 走读前先背下来）

| 编号 | 事件 | 函数（文件） | 线程 |
|---|---|---|---|
| ① | main 构造栈上 EventLoop | main（example/testserver.cc） | mainLoop |
| ② | TcpServer/Acceptor 构造：socket+bind+acceptChannel_ 绑 handleRead | TcpServer()、Acceptor()（src/TcpServer.cc、src/Acceptor.cc） | mainLoop |
| ③ | start：起 3 个 subLoop 线程 + runInLoop(Acceptor::listen) | TcpServer::start、EventLoopThreadPool::start、Acceptor::listen | mainLoop（线程创建阻塞在 EventLoopThread::startLoop） |
| ④ | loop 阻塞在 epoll_wait | EventLoop::loop、EPollPoller::poll | 各 loop 自己 |
| ⑤ | 新连接：handleRead→accept4→NewConnectionCallback_ | Acceptor::handleRead、Socket::accept | mainLoop |
| ⑥ | 轮询选 subLoop、new TcpConnection、注册回调、跨线程投递 | TcpServer::newConnection（【线程跳转 1】queueInLoop+wakeup） | mainLoop→subLoop#N |
| ⑦ | connectEstablished：tie+enableReading+onConnection | TcpConnection::connectEstablished、doPendingFunctors | subLoop#N |
| ⑧ | 数据到达：readFd(readv)→onMessage | TcpConnection::handleRead、Buffer::readFd | subLoop#N |
| ⑨ | 用户 send：直写或入 outputBuffer_+enableWriting | TcpConnection::send、sendInLoop | subLoop#N |
| ⑩ | EPOLLOUT：writeFd→retrieve→disableWriting→writeComplete | TcpConnection::handleWrite | subLoop#N |
| ⑪ | 对端关闭：handleClose→【跳转 2】removeConnection 回 mainLoop→【跳转 3】connectDestroyed 回 subLoop→引用归零析构→close(fd) | handleClose、TcpServer::removeConnection、removeConnectionInLoop、connectDestroyed、~Socket | subLoop→mainLoop→subLoop |
| ⑫ | 退出：quit→wakeup→loop 返回→~EventLoopThread join | EventLoop::quit、~EventLoopThread、Thread::join | mainLoop + subLoop |

---

## 1. 编译运行

### 1.1 从零编译

~~~bash
cd /home/lee/projects/muduo-core
cmake -S . -B build                       # 配置（生成 build/）
# 如果报错 Compatibility with CMake < 3.5 has been removed（CMake 4.x 会报，因为顶层
# CMakeLists.txt 的 cmake_minimum_required 写的是 3.0）：
cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j"$(nproc)"          # 全量构建
~~~

### 1.2 产物在哪（对照三份 CMakeLists.txt 的注释）

| 产物 | 路径 | 由谁决定 |
|---|---|---|
| 动态库 | lib/libmuduo_core.so | 顶层 set(LIBRARY_OUTPUT_PATH ...) + src 的 add_library(muduo_core SHARED ...) |
| 可执行 | example/testserver | example 的 set_target_properties(testserver PROPERTIES RUNTIME_OUTPUT_DIRECTORY ...) |
| 中间文件 | build/** | cmake -B build |

运行：./example/testserver（二进制里已烧 RUNPATH 指向 lib/，不用设 LD_LIBRARY_PATH；
拷走它会报 error while loading shared libraries: libmuduo_core.so）。

### 1.3 只编一个翻译单元做语法检查（改完 .cc 最快自检，不链接）

~~~bash
g++ -std=c++17 -fsyntax-only -Iinclude src/EventLoop.cc
for f in src/*.cc example/*.cc; do g++ -std=c++17 -fsyntax-only -Iinclude "$f" || echo FAIL $f; done
~~~

（注意：-fsyntax-only 只做语法+语义检查，不生成 .o 也不链接；它**绕过了** example 里
`-std=c++11` 那个冲突，所以别用它验证 §6 面试题里的标准之争。）

### 1.4 常见报错与原因

| 症状 | 原因 | 处置 |
|---|---|---|
| TcpServer.h: No such file or directory | 手工 g++ 没给 -Iinclude；或 cmake 里 PUBLIC 传播链断了 | 加 -Iinclude；正常走 cmake 构建则检查 target_link_libraries 是否还链 muduo_core |
| undefined reference to 'pthread_create'（glibc<2.34） | 链接行没有 -lpthread（example/CMakeLists.txt 的 `${{LIBS}` 被删） | 恢复 `${{LIBS}`；本机 glibc 2.43 不会遇到（符号已并入 libc，但别因此养成删它的习惯） |
| error while loading shared libraries: libmuduo_core.so | 二进制被拷走/换机器，RUNPATH 指向的 lib/ 路径失效 | 回仓库跑，或 LD_LIBRARY_PATH=/home/lee/projects/muduo-core/lib ./testserver |
| 新增 src/Foo.cc 后 cmake --build 完全无视它 | file(GLOB) 只在配置期求值 | 重新 cmake -S . -B build 再 build（三份 CMakeLists 的【易错】注释） |
| 加了第二个带 main 的 example 文件 → multiple definition of 'main' | GLOB 把 example/*.cc 全塞进 testserver 一个目标 | 新 add_executable，或替换 testserver.cc（见 §5 练习 6） |
| MUDUO_USE_POLL=1 ./example/testserver 立刻 Segmentation fault | src/DefaultPoller.cc 的 Poller::newDefaultPoller 在该环境变量存在时 **return nullptr**（本项目没实现 PollPoller）；EventLoop 构造初始化列表 poller_(newDefaultPoller) 拿到空 unique_ptr，紧接着构造体里 wakeupChannel_->enableReading() → Channel::update → EventLoop::updateChannel → **对空指针调虚函数**，当场段错误（活不到 loop()） | 用 gdb 复现（见 1.5）；想让它别崩就把 DefaultPoller 该分支改成返回真 PollPoller——但这就是本项目和原版的差异之一（面试题 20） |
| gdb 里 bt 全是 ?? / 变量看不见 | checked-in 的 build/ 和 example/testserver 是旧配置产物，编译时还没有 -g（见顶层 CMakeLists 注释） | rm -rf build 重新 cmake（顶层现在 set(CMAKE_BUILD_TYPE Debug) → CMAKE_CXX_FLAGS_DEBUG=-g），再 cmake --build build -j |

### 1.5 亲手制造并看清 MUDUO_USE_POLL 崩溃（10 分钟）

~~~bash
gdb -q --args ./example/testserver
(gdb) set environment MUDUO_USE_POLL=1
(gdb) run
Program received signal SIGSEGV ...
(gdb) bt
#0  ... EventLoop::updateChannel ...   # poller_ 为空
#1  Channel::update ...
#2  Channel::enableReading ...
#3  EventLoop::EventLoop ...           # 还没进 main 的 loop.loop() 就死了
~~~

对照阅读：src/EventLoop.cc 构造函数、src/DefaultPoller.cc 的【⚠问题】注释。

---

## 2. 观察 Echo 行为

### 2.1 起服务 + 收发

~~~bash
./example/testserver &            # 终端 A；或前台跑
nc 127.0.0.1 8080                 # 终端 B：敲 hello 回车 → 原样收到 hello（这就是全链路 ⑤~⑨）
~~~

服务端日志逐行对号入座（每行前缀 [INFO]+时间，Timestamp 精度只有秒——见练习 1）：

| 日志片段 | 链路位置 | 解读 |
|---|---|---|
| EventLoop %p start looping ×4 | ④（1 主 + 3 子，四个 loop 全打印；%p 差异记下，§3 gdb 用得上） | 每个 EventLoop::loop 进入时打一次 |
| func=updateChannel => fd=.. events=3 | ③/⑦（enableReading 注册 EPOLLIN|EPOLLPRI=1|2=3） | src/EPollPoller.cc 的 updateChannel LOG |
| channel handleEvent revents:1（listenfd 上） | ⑤ | 新连接完成，见 2.2 拆位 |
| TcpServer::newConnection [EchoServer] ... from 127.0.0.1:xxxxx | ⑥ | mainLoop 里轮询选 subLoop |
| TcpConnection::ctor[...] at fd=connfd | ⑥ | fd 号是动态的，逐连接递增 |
| channel handleEvent revents:1（connfd 上） | ⑧ | 数据到了 |
| Connection DOWN / handleClose / removeConnectionInLoop / dtor | ⑪ | 见 2.4 |

### 2.2 把 revents 数字拆成 epoll 位（重点）

**对照表（十进制值）**：

| 位 | 值 | 宏 | 何时来 |
|---|---|---|---|
| 1 | 1 | EPOLLIN | 可读（新连接完成 / 数据到达 / eventfd 被写） |
| 2 | 2 | EPOLLPRI | 带外数据（教学碰不到；但 kReadEvent 注册了它，见 src/Channel.cc） |
| 4 | 4 | EPOLLOUT | 内核发送缓冲有空位（只有注册了写事件才会上报——Echo demo 里基本看不到 4） |
| 8 | 8 | EPOLLERR | socket 出错（注册与否都上报） |
| 16 | 16 | EPOLLHUP | 连接挂断（双向都关；注册与否都上报） |
| 32 | 32 | EPOLLRDHUP | 对端半关闭/关闭（**本项目没注册，永远别指望见到**） |

拆法 = 十进制拆 2 的幂：

| revents | 拆解 | 人话 |
|---|---|---|
| 1 | IN | 普通可读：新连接或新数据 |
| 4 | OUT | 可写：outputBuffer_ 排空进度 |
| 16 | HUP | 挂断 |
| 10 | 8+2 = ERR+PRI | 出错（罕见，多半在带 urgent 数据后） |
| 12 | 8+4 = ERR+OUT | **对端已 RST 你还想写**——socket 错误与"可写"同时出现（挂断连接永远"可写"，写即报错） |
| 17 | 16+1 = HUP+IN | 最后一段数据 + 挂断同到；读这条链要看 src/Channel.cc 的 handleEventWithGuard 里 `&& !(revents_ & EPOLLIN)`——正因如此 17 先走读不丢数据 |
| 24 | 16+8 = HUP+ERR | 又挂又有错（RST 之后） |
| 3 | 1+2 | IN+PRI |

心算验证：python3 -c "print(bin(12))"。
**动手见到 12 的方法**：先让连接被 RST（拔掉网线/对端强制关），服务再 send 一次。demo 里常见的是 1；出现 4 说明你输出积压了（可把练习 4 的大文件 sendFile 当触发器）。

### 2.3 用 ss 与 /proc 看 fd 随连接变化

~~~bash
ss -ltnp | grep 8080                     # LISTEN 一行；确认进程持有监听口
nc 127.0.0.1 8080 < /dev/null &          # 造一个连接
ss -tnp | grep 8080                      # 两条 ESTAB（本端+对端视角）
ls -l /proc/$(pidof testserver)/fd | wc -l
~~~

fd 基线（testserver，setThreadNum(3)）：**3**（stdin/out/err）+ **4 个 epollfd**（每个 EventLoop 一个 Poller）+ **4 个 eventfd**（每个 EventLoop 一个 wakeupFd_，见 src/EventLoop.cc createEventfd）+ **1 个 listenfd** = **12**；每进来一个连接 +1（connfd）。多开几个 nc 数一数是否严格 +1、断开是否 -1（⑪ 的销毁链是否真的把 fd 还回去了——~Socket 的功劳）。glibc/终端偶尔会多 1~2 个内部 fd，看**增量**别看绝对值。

### 2.4 kill 客户端，观察 handleClose 链

~~~bash
nc 127.0.0.1 8080        # 记下 PID：echo $$ 或 jobs -l
# 服务日志先确认：Connection UP
kill %1                  # （另开终端）kill <nc 的 pid>
~~~

正常退出（进程死亡时内核替它 close → 对端收 FIN）：服务日志依次出现——

1. `channel handleEvent revents:1`（connfd 可读，其实是 EOF）
2. `TcpConnection::handleClose fd=.. state=2`（src/TcpConnection.cc）
3. `Connection DOWN : 127.0.0.1:xxxx`（testserver.cc 的 onConnection，connected()==false）
4. `TcpServer::removeConnectionInLoop`（mainLoop，【线程跳转 2】之后）
5. `TcpConnection::dtor`（subLoop，【跳转 3】之后，state=0）

对应全链路 ⑪。如果顺序与你的注释/文档描述不符 → 说明理解错了，回 §3 用 gdb 钉死每一步。

---

## 3. gdb 断点清单（亲眼看见【线程跳转 1/2/3】）

### 3.0 先决条件：产物必须带 -g

~~~bash
rm -rf build && cmake -S . -B build && cmake --build build -j"$(nproc)"
# 顶层现在 set(CMAKE_BUILD_TYPE Debug)：Debug 只追加 -g、无 -O 优化（等效 -O0），变量全可见。
# 直接调试仓库里 checked-in 的旧 example/testserver 是看不到变量的（旧配置没有 -g）。
~~~

LOG_INFO 刷屏拖慢 gdb 的缓解（二选一，实验前必读三份 CMakeLists 注释里的告诫）：
1. **推荐**：只在 nc 时打断点，run 之前不打日志相关断点；或不用 gdb 时过滤服务输出——`./example/testserver 2>&1 | grep -vE 'revents|func='`。
2. 彻底清净：备份后把 include/Logger.h 的 LOG_INFO 宏体掏空（do{}while(0) 只留空壳）重编，**实验完从 /tmp 备份拷回**。⚠️ 别用 git checkout/restore 来还原（工作区里全是未提交内容，注释）。注意宏内部（续行反斜杠之间）不许插注释是铁律，掏空整个宏体则没问题。

### 3.1 八个核心断点

~~~bash
gdb -q --args ./example/testserver
(gdb) set print pretty on
(gdb) handle SIGPIPE nostop noprint pass     # 本项目没忽略 SIGPIPE（Buffer.cc 有【⚠问题】注释），不这样设置会被它咬
(gdb) break EventLoop::loop
(gdb) break EPollPoller::poll
(gdb) break Acceptor::handleRead
(gdb) break TcpServer::newConnection
(gdb) break TcpConnection::connectEstablished
(gdb) break TcpConnection::handleRead
(gdb) break TcpConnection::sendInLoop
(gdb) break EventLoop::doPendingFunctors
(gdb) run
~~~

（在库函数上设断点时 gdb 可能提示 has not been defined yet, will be pending——按 y；.so 加载后会补上。）

### 3.2 完整会话示例（一次连接的完整旅程）

~~~
Thread 1 hit Breakpoint 1, EventLoop::loop () at src/EventLoop.cc
(gdb) info threads
  Id   Target Id         Frame
* 1    Thread 1 (LWP ...) main ...        # 此刻 subLoop 已在 epoll_wait（start() 先于 loop() 调用）
  2    ... EventLoopThread::threadFunc ...
  3    ... EventLoopThread::threadFunc ... # 3 个 IO 线程全部就位
  4    ... EventLoopThread::threadFunc ...
(gdb) c                                  # 另开终端: nc 127.0.0.1 8080
Thread 1 hit Breakpoint 3, Acceptor::handleRead ()    # ⑤ mainLoop
(gdb) c
Thread 1 hit Breakpoint 4, TcpServer::newConnection () # ⑥ mainLoop
(gdb) p ioLoop                                          # 记住这个 subLoop 的 EventLoop 地址
$1 = (EventLoop *) 0x7f...
(gdb) c
Thread 5 hit Breakpoint 5, TcpConnection::connectEstablished ()   # ★线程号变了！【线程跳转 1】
(gdb) info threads        # 当前 * 号已不在 1
(gdb) bt                  # #1 doPendingFunctors #2 EventLoop::loop —— ⑦ 的铁证
(gdb) p this->channel_->fd_
$2 = 8                   # 记下 connfd=8
(gdb) condition 6 this->channel_->fd_ == 8     # 让 handleRead 断点只在这条连接触发（条件断点）
(gdb) c
(gdb) thread apply all bt                      # 快照：主线程在 epoll_wait，另外 2 个 subLoop 也在 epoll_wait，
                                               # 本线程在断点上 —— one loop per thread 一图看全
# 在 nc 里敲 hello 回车：
Thread 5 hit Breakpoint 6, TcpConnection::handleRead ()   # ⑧
(gdb) c
Thread 5 hit Breakpoint 7, TcpConnection::sendInLoop (...) # ⑨（onMessage 里的 conn->send）
(gdb) p nwrote; p remaining                                # 快路径一次写完：nwrote==len, remaining==0 → 不注册 EPOLLOUT
(gdb) c
# 退出 nc：
Thread 5 hit Breakpoint 8, EventLoop::doPendingFunctors ... # 每个 loop 每轮都到这，靠条件断点筛
(gdb) c   # 反复按 c，把 handleClose→removeConnection(mainLoop)→connectDestroyed(subLoop) 的 bt 逐个截下来
(gdb) quit
~~~

观察点核对表：

| 断点 | 期望命中线程 | 证明的事 |
|---|---|---|
| Acceptor::handleRead / TcpServer::newConnection | 主线程（与 EventLoop::loop 首次命中同一个） | ⑤⑥ 在 mainLoop |
| connectEstablished / handleRead / sendInLoop | 某个子线程（info threads 的 * 变了） | 【线程跳转 1】 |
| removeConnection（可补 break TcpServer::removeConnection） | **子线程**命中（closeCallback 是同步直调） | 跳转 2 上游 |
| removeConnectionInLoop | **主线程**命中 | 【线程跳转 2】 |
| connectDestroyed / ~TcpConnection | **子线程**命中 | 【线程跳转 3】+ ⑫ 前的对象销毁顺序 |

（thread apply all bt、condition、ignore N 次数、p 变量，都是你验证任何注释断言的三板斧。
若某次行为和本表不符：以 gdb 为准，怀疑对象是你的理解或某条注释。）

---

## 4. 关键机制独立小程序（每个都可整段编译）

放 /tmp 做实验：mkdir -p /tmp/muduo-play && cd /tmp/muduo-play，逐个保存为文件名。

### 4.1 eventfd —— 为什么能拿来唤醒 epoll_wait（evfd.cc）

~~~cpp
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
int main() {
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);      // 同 createEventfd()
    int ep  = epoll_create1(0);
    epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = efd;
    epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);

    epoll_event got;
    int n = epoll_wait(ep, &got, 1, 50);                    // 先空等一次：50ms 超时返回 0
    printf("before post: epoll_wait=%d (0=timeout)\n", n);

    uint64_t one = 1;
    write(efd, &one, 8);                                    // 同 EventLoop::wakeup()：8 字节进计数器
    n = epoll_wait(ep, &got, 1, 50);                        // 立刻可读
    uint64_t cnt = 0;
    read(efd, &cnt, 8);                                     // 读走=清零，同 EventLoop::handleRead()
    printf("after post: epoll_wait=%d counter=%llu\n", n, (unsigned long long)cnt);

    errno = 0;
    ssize_t r = read(efd, &cnt, 8);                         // 再读：EAGAIN，证明非阻塞语义
    printf("second read=%zd errno=%d(EAGAIN=%d)\n", r, errno, EAGAIN);
    close(efd); close(ep);
}
~~~
编译运行：`g++ -std=c++17 -g evfd.cc -o evfd && ./evfd`
（对照：src/EventLoop.cc 的 createEventfd/wakeup/handleRead，全链路 ⑥⑦。）

### 4.2 readv + iovec —— Buffer::readFd 的双缓冲原型（readv.cc）

~~~cpp
#include <sys/uio.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
int main() {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);                // 造一对相连的 socket
    const char* msg = "0123456789ABCDEF";
    write(sv[0], msg, 16);

    char a[5] = {0};                                        // 小"Buffer 剩余空间"
    char b[16] = {0};                                       // 大"栈上 extrabuf"
    iovec vec[2] = { {a, 4}, {b, sizeof b} };               // 两段内存，一次系统调用
    ssize_t n = readv(sv[1], vec, 2);                       // 同 Buffer::readFd
    printf("readv=%zd seg1=%.4s seg2=%.12s\n", n, a, b);
    // 期望：readv=16 seg1=0123 seg2=456789ABCDEF —— 第一段填满自动溢出到第二段，这就是"双缓冲"
    close(sv[0]); close(sv[1]);
}
~~~
编译：`g++ -std=c++17 -g readv.cc -o readv && ./readv`

### 4.3 enable_shared_from_this 双控制块崩溃 demo（dblctrl.cc）

~~~cpp
#include <memory>
#include <cstdio>
#include <vector>
struct Conn : std::enable_shared_from_this<Conn> {
    ~Conn() { printf("~Conn %p\n", (void*)this); }
    void poison() {
        std::shared_ptr<Conn> bad(this);   // ★错：新建第二个控制块，计数体系分裂
        pool.push_back(bad);               // 模拟"被容器再持有"
    }
    void cure() {
        std::shared_ptr<Conn> good = shared_from_this(); // 对：与外部 shared_ptr 共享控制块
        pool.push_back(good);
    }
    std::vector<std::shared_ptr<Conn>> pool;
};
int main() {
    {
        auto c = std::make_shared<Conn>();   // 控制块 A：计数 1
        c->poison();                          // 控制块 B：计数 1
    }                                        // A 归零 → ~Conn；随后 pool 销毁 → B 归零 → 再 ~Conn → double free！
    return 0;
}
~~~
编译：`g++ -std=c++17 -g -fsanitize=address dblctrl.cc -o dblctrl && ./dblctrl`
现象：~Conn 打印两次，随后 ASAN 报 @@"double-free / heap-use-after-free"`（不加 ASAN 也常在退出时 abort: double free detected in tcache）。
原因一句话：**同一个裸 this 被两个独立控制块管辖，各自认为自己是最后一个主人，各自释放一次。**
把 poison() 换成 cure()（用 shared_from_this()）即修复；前提别忘了：对象必须先活在 shared_ptr 里，否则 shared_from_this 抛 bad_weak_ptr。
（对照：src/TcpConnection.cc 全文件只用 shared_from_this()，从不 shared_ptr(this)；Channel::tie 收的就是它。）

### 4.4 weak_ptr::lock 保命 demo（weakguard.cc，模拟 Channel::tie + handleEvent）

~~~cpp
#include <memory>
#include <cstdio>
struct Handler { void onEvent() { printf("event handled\n"); } };
struct Chan {
    std::weak_ptr<void> tie_;
    void tie(const std::shared_ptr<void>& o) { tie_ = o; }
    void handleEvent() {                       // 同 src/Channel.cc 的 handleEvent
        auto guard = tie_.lock();              // 提升：活着才有强引用
        if (guard) { Handler().onEvent(); }
        else       { printf("owner gone, skip (no dangling this!)\n"); }
    }
};
int main() {
    Chan c;
    {
        auto owner = std::make_shared<Handler>();
        c.tie(owner);                          // 只存 weak_ptr，owner 计数不变
        c.handleEvent();                       // → event handled
    }                                          // owner 出作用域析构（weak 不续命）
    c.handleEvent();                           // → owner gone, skip —— 若没有这层 lock，这里就是野指针回调
}
~~~
编译：`g++ -std=c++17 -g weakguard.cc -o weakguard && ./weakguard`

---

## 5. 六个递进练习（目标 → 改哪 → 验收）

> 新增 .cc/.h 后记得：GLOB 只在配置期求值，**重新 cmake -S . -B build** 才会纳入（三份 CMakeLists 的【易错】注释都讲了）。

### 练习 1：把 Timestamp 修成真微秒，日志带小数

- **目标**：src/Timestamp.cc 的 Timestamp::now() 弃用 time(NULL)，改 **clock_gettime(CLOCK_REALTIME)**（或 gettimeofday）取秒+纳秒换算成微秒；**同时**改 toString()：现在它是把字段"当秒"直接喂 localtime()（now() 塞的其实是秒，将错就错才显示正常——include/Timestamp.h 的【⚠问题】注释），修好单位后必须 **先除 10^6 得 time_t 再 localtime_r()**，并追加 ".%06ld" 小数部分。Logger.cc 的 Logger::log 拼接处自然带上。
- **改哪**：src/Timestamp.cc（now/toString 两个函数）；include/Timestamp.h 注释可选更新（别动声明）。
- **验收**：cmake 全量重编后跑 testserver，同一次连接里至少两行日志出现**可区分**的 6 位小数；附加题：用两个线程各打 1000 行日志，比较 localtime() 与 localtime_r() 的时间列是否串扰（这正是 A1【易错】条目讲的线程安全问题；原版做法以原版源码为准）。
- **语法自检**：`g++ -std=c++17 -fsyntax-only -Iinclude src/Timestamp.cc`

### 练习 2：给 Buffer 加 prepend 系列 + 防误用哨兵

- **目标**：在 include/Buffer.h/.cc 补：**prependableBytesAlreadyUsed()**（= readerIndex_ - kCheapPrepend）、**prepend(size_t len)**（把 readerIndex_ 往回退 len，返回可写指针，供你写长度头）、hasReadOffset() 可选。防呆设计：一旦 prepend 过，后续 append/retrieve 若覆盖了前缀区或二次 prepend 越界，要能当场发现——原版 muduo 的做法（用 kPrependIndicator 魔数哨兵，以原版源码为准），你也可以自己定一个 4 字节魔数放在前缀区末尾做校验。本项目现状：**prependable 除了 kCheapPrepend 那 8 字节根本没人管**（A2【⚠问题】）。
- **改哪**：include/Buffer.h（加 3~4 个成员函数声明 + 1 个哨兵常量）、src/Buffer.cc 可选（或全 inline 在头里）。
- **验收**：写个 main 小测：append("hello") → prepend(4) 写大端长度 → readableBytes()==9、peek() 指向长度头；retrieveAllAsString 得到 @@"\\0\\0\\0\\x05hello"`；故意 append 覆盖前缀 → 哨兵断言触发。Echo 升级：onMessage 里带长度头回包，客户端 hexdump -C 肉眼可见 00 00 00 05。

### 练习 3：最简 TimerQueue + 空闲 10 秒自动踢人

- **目标**：用 **timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC)** 造一个"到期时变可读的 fd"，配一个 **Channel** 注册到连接所在 EventLoop（与 wakeupChannel_ 同款做法，见 src/EventLoop.cc 构造），读掉 8 字节到期计数即收到超时通知。
- **改哪**：新建 include/TimerQueue.h + src/TimerQueue.cc（一次性 one-shot 版即可：startAfter(sec) / cancel() / onTimer 回调）；src/TcpConnection.cc 的 handleRead 每次数据到达 reset timer；到期回调调 **shutdown()**。example/testserver.cc 里给 EchoServer 加 setIdleTimeoutSeconds(10) 调用。
- **验收**：nc 连上不发数据 → 10~11 秒内出现 Connection DOWN + handleClose 日志链（对照 §2.4）；期间敲一个字 → 再等 8 秒不掉线、10+ 秒后掉。全部在 setThreadNum(3) 下测三种不同连接各自超时（证明 timer 活在 conn 自己的 subLoop）。
- **坑预告**：timerfd 的 Channel 必须用 conn->getLoop() 构造（one loop per thread），跨线程操作 Channel = 灾难（prerequisites 6.2）；以及记得重新配置（GLOB）。

### 练习 4：修 sendFileInLoop 忙轮询（对齐【⚠问题】注释）

- **现状**：src/TcpConnection.cc 的 sendFileInLoop 在 remaining>0 时**立刻 queueInLoop 把自己再排一次**——内核发送缓冲满时 sendfile 返回 EWOULDBLOCK/0，它就 100% CPU 空转烧轮次（对应文件里的【⚠问题】写得明白）。
- **目标**：改成 **EPOLLOUT 驱动**：还有剩余且 !channel_->isWriting() 时 enableWriting()，把未发的 (fileFd, offset, remaining) 存成 TcpConnection 成员（只在 loop 线程读写，普通成员即可）；handleWrite 里发现"文件发送模式"就先继续 sendfile，发完 → disableWriting + writeCompleteCallback_。与 §2.2 里 revents:4 的观察闭环。
- **改哪**：src/TcpConnection.cc 的 sendFileInLoop/handleWrite；include/TcpConnection.h 加 3 个私有成员与一个小 enum。
- **验收**：造 100MB 文件（dd if=/dev/urandom of=/tmp/big.bin bs=1M count=100），onMessage 收到字符串 "file" 即 open+sendFile；top -H 看该服务线程 CPU 从接近 100% 降到个位数；md5sum 两端一致；发大文件期间**另一条 nc 连接**依然秒回 echo（证明 loop 没被忙轮询饿死）。

### 练习 5：加异步日志（前端缓冲 + 后端线程双缓冲）

- **对照不足**（读 include/Logger.h、src/Logger.cc 即可确认）：现在每条 LOG 都在**调用线程**里 snprintf + std::cout + endl 直接 write+flush——慢设备会把 loop 线程拖住；多线程下 setLogLevel+log 两步没有原子性，前缀/内容可能交错；也没有文件落盘。
- **目标**：仿原版 AsyncLogging 的思路做一个简化版（具体参数以原版源码为准）：前端把格式化好的行 append 进一块 64KB 循环缓冲（满则提前 flush）；一个专用 **后端线程** 用 **mutex + condition_variable** 等待，醒来 **swap 双缓冲**（当前块交给后端写盘，自己换一块新——只换指针不拷贝，这正是 EventLoop::doPendingFunctors swap 的同款思想）；后端 fwrite+fflush；atexit 注册 flush；每 3 秒无条件刷一次。
- **改哪**：新建 include/AsyncLogger.h、src/AsyncLogger.cc（GLOB 重新配置！）；LOG_XXX 宏先不动（避免动 include/Logger.h 宏内部——铁律！可新增 LOG_AINFO 宏做切换）。
- **验收**：10 万行 LOG 压测，异步版墙钟时间明显低于同步版（时间戳自己对比）；kill 前 atexit/信号处理里 flush，行数一条不少；gdb thread apply all bt 能看见那个专用后端线程在 wait。

### 练习 6：聊天室广播服务器

- **目标**：任意客户端发消息 → 广播给其他所有连接；进出房间全员通知。
- **改哪**：新建 example/chatserver.cc（记得单独 add_executable，避开 GLOB 双 main 坑）。EchoServer 换 ChatServer：
  - 成员 `std::mutex mutex_; std::vector<TcpConnectionPtr> clients_;`（**为什么要自己存**：TcpServer 的 connections_ 是 private、没有 getConnection/广播 API——所以应用要自己维护"名字→conn"的 map，按昵称查也方便）。
  - onConnection：connected() 时 push_back（并记 peerAddress 当昵称），否则 erase 对应 conn。
  - onMessage：`msg = buf->retrieveAllAsString();` 然后遍历 clients_ 给 `c != conn` 的每个 `c->send(prefix + msg)`。
- **跨线程问题**（重点讨论）：3 个 subLoop 会并发进出 clients_ → **数据竞争**。三种解法按优雅排序：① 直接上 mutex（能活，但 send 要在别的线程调用要相信 send 自身线程安全——src/TcpConnection.cc 的 send/runInLoop 机制正好证明它）；② 广播动作全部 `conn->getLoop()->runInLoop([msg]{...})` 投递到目标 loop 上做，clients_ 用 shared_ptr 拷贝快照后无锁遍历；③ 指定一个固定"广播 loop"（getAllLoops()[0]）串行化。原版 muduo 的聊天室示例采用哪种请以原版源码为准，思路②是社区最常见做法。
- **验收**：两个 nc 窗口互见对方消息与自己上下线通知；客户端 kill 后另一窗口收到"xxx 离开"且不崩（对照 ⑪ 销毁链：广播遍历时若某 conn 正在析构，runInLoop 的 shared_ptr 捕获保证它活到回调执行——A5 条目 removeConnectionInLoop 先 erase 再 queueInLoop 的意义）；setThreadNum(0)/3 两种配置都跑一遍。

---

## 6. 二十道高频面试题（题干 → 一句话答案 → 本仓库代码指针）

| # | 题干 | 一句话答案 | 看这里 |
|---|---|---|---|
| 1 | one loop per thread 靠什么保证？ | 每个 EventLoop 构造时记录 threadId_ 并用 __thread 指针 t_loopInThisThread 拒绝同线程建第二个 loop；新 loop 线程都在 EventLoopThread::threadFunc 的栈上建唯一 EventLoop | src/EventLoop.cc EventLoop()；src/EventLoopThread.cc threadFunc() |
| 2 | EventLoop 为什么不能跨线程操作？ | 它的 ChannelList/Poller 操作（epoll_ctl、enableWriting）全无锁，正确性建立在"只有归属线程碰它"上；threadId_ + isInLoopThread() 是这道契约的守卫 | include/EventLoop.h isInLoopThread；src/EventLoop.cc updateChannel |
| 3 | Channel::tie 为什么需要？ | fd 被 remove 后 Channel 仍可能本轮执行回调，若其宿主 TcpConnection 已析构就是野指针；tie 把宿主 weak_ptr 存进 Channel，handleEvent 先 lock 提升再调用 | src/Channel.cc tie()/handleEvent() |
| 4 | weak_ptr 在这里解决什么？ | "观察不续命"：Channel 不能延长 TcpConnection 寿命（会泄漏），lock() 只在活着时放行调用 | src/Channel.cc handleEvent() |
| 5 | runInLoop 与 queueInLoop 区别？ | 在自己线程就直接同步执行，否则退化成 queueInLoop（入队+条件唤醒）；queueInLoop 永远入队并可能 wakeup | src/EventLoop.cc runInLoop()/queueInLoop() |
| 6 | doPendingFunctors 为什么 swap？ | ① 锁只罩住交换，执行回调不持锁（临界区短）；② 回调里再 queueInLoop 不会自锁死 | src/EventLoop.cc doPendingFunctors() |
| 7 | 为什么用 eventfd 唤醒？ | 睡着的 epoll_wait 需要一个"可读事件"才能被叫醒；eventfd 给内核里最便宜的计数器 fd，write 8 字节即触发 EPOLLIN，无数据拷贝、天然适合 LT | src/EventLoop.cc createEventfd()/wakeup()/handleRead() |
| 8 | epoll LT 与 ET 区别？muduo 用哪个？ | LT：有数据就一直报；ET：只在状态跳变报一次，必须读到 EAGAIN。LT+非阻塞可慢可不丢；ET 快但易漏读。本库注册事件从不带 EPOLLET，全程 LT | src/Channel.cc enableReading；src/TcpConnection.cc handleRead |
| 9 | 高水位回调怎么工作？ | sendInLoop 里：待写数据量 oldLen+remaining 跨过阈值且此前未跨（oldLen < 水位）时，queueInLoop 上报一次，给应用"限流"的机会（不真限流） | src/TcpConnection.cc sendInLoop()；include/Callbacks.h HighWaterMarkCallback |
| 10 | shutdown 与 close 区别？ | shutdown(SHUT_WR) 立刻对端可见 FIN 且写端永久关闭、读端照常；close 只减本进程 fd 引用计数、dup 过甚至不发 FIN。半关闭=一方不再发、仍能收 | src/Socket.cc shutdownWrite()/~Socket()；prerequisites 3.3 |
| 11 | 连接关闭时对象销毁顺序？ | handleClose→disableAll→用户 connectionCallback(false)→closeCallback 进 mainLoop 删 connections_→回原 subLoop 跑 connectDestroyed→最后一个 shared_ptr 出作用域→~TcpConnection→~unique_ptr<Socket>→close(fd) | src/TcpConnection.cc handleClose()；src/TcpServer.cc removeConnectionInLoop()、src/TcpConnection.cc connectDestroyed() |
| 12 | TcpConnection 为什么必须 shared_ptr？ | 它的回调在别的线程排队/执行，生命周期不由任何单点决定；栈上对象活不到回调执行，引用计数保证"用到最后一个人" | include/Callbacks.h TcpConnectionPtr；src/TcpServer.cc newConnection() |
| 13 | readv 为什么两个缓冲区？ | 不知道对端要来多少字节：先用 Buffer 剩余空间接，不够再溢出到栈上 64KB extrabuf，一次系统调用全收下再 append 回 Buffer——既免循环 read 又免二次 syscall | src/Buffer.cc readFd() |
| 14 | SO_REUSEADDR 与 SO_REUSEPORT 区别？ | ADDR：允许 bind 处于 TIME_WAIT 的本机地址（重启秒起）；PORT：多个 socket 真能同绑一个端口各自 accept（内核分流）。⚠本仓库可核实：Acceptor 构造把两个都无条件打开，reuseport 形参根本没被使用 | src/Socket.cc setReuseAddr()/setReusePort()；src/Acceptor.cc Acceptor() |
| 15 | 惊群问题本项目怎么看？ | 单 listenfd 单 mainLoop，进程内只有一只鸡，无惊群；真惊群在多进程/多线程同 accept 同一 fd——常规解法是 SO_REUSEPORT 每进程各建 listenfd 由内核分流；本项目没演示多进程，属留白（原版怎么处理以原版源码为准） | src/Acceptor.cc listen()；include/TcpServer.h Option |
| 16 | Buffer 为什么留 kCheapPrepend 8 字节？ | 给协议头（长度/类型）预留"往前写"的便宜空间，retrieveAll 回到这里而不是 0，免搬移；本项目 prepend 系列还没做（练习 2） | include/Buffer.h kCheapPrepend/retrieveAll()/makeSpace() |
| 17 | 三种 Reactor 形态？ | 单 Reactor 单线程=setThreadNum(0)；单 Reactor 多线程=一个 loop+业务线程池（本库靠 queueInLoop 手工可搭）；多 Reactor 多线程=setThreadNum(N)，mainLoop accept + N 个 subLoop 管 IO——testserver 默认 3 | example/testserver.cc EchoServer()；src/EventLoopThreadPool.cc start()/getNextLoop() |
| 18 | muduo 为什么不处理粘包？ | 它是字节流传输库，消息边界属于应用协议；demo 靠"人敲回车"天然分帧。本项目连 prepend 都没做，想组长度头先做练习 2 | src/TcpConnection.cc handleRead()；src/Buffer.cc readFd() |
| 19 | Semaphore 在 Thread::start 的作用？ | sem_wait 把"子线程写完 tid_"变成 start() 返回的前置条件——用计数信号量同时解决竞态与发布（happens-before）；sem 在调用者栈上、lambda 捕获 this 引用是它的两个易错前提 | src/Thread.cc start() |
| 20 | 本项目与原版 muduo 的 3 个主要差异？ | ① 没有 TimerQueue/异步日志/FileUtil 等整层能力；② Timestamp 退化到秒精度且用非线程安全 localtime；③ Poller 只有 EPollPoller，DefaultPoller 的 POLL 分支返回 nullptr 一设就崩（另可加：Buffer 无 prepend、EPollPoller kDeleted 空分支、setTcpNoDelay 在本仓库无人调用） | src/DefaultPoller.cc；src/Timestamp.cc now()；include/Buffer.h |

---

## 7. 读不懂时的三级降级策略

**L1 只读签名和名字**：合上函数体，只过 include/*.h——每个类先回答"它是谁、它持有什么、它能把消息给谁"。顺序建议（依赖从叶子到根）：Timestamp/noncopyable/Logger → InetAddress/Socket/Buffer → Channel/Poller/EPollPoller → EventLoop/Thread/EventLoopThread/EventLoopThreadPool → Callbacks/TcpConnection/Acceptor/TcpServer → example/testserver.cc。配套 grep：`grep -nE 'class [A-Z]|void |int ' include/EventLoop.h`。

**L2 只看【流程】/【线程】标签串链路**：不读正文，只 grep 标签并按 ①→⑫ 排序拼故事：

~~~bash
grep -rn '【流程】' src include
grep -rn '【线程' src include
grep -rn '【⚠问题】' src include        # 顺便收集全部已知退化点
~~~

拼完拿本文 §0 速查表对答案；对不上就回源码把那条注释重读。

**L3 上 gdb 打栈验证**：用 §3 的八个断点 + info threads + thread apply all bt + 条件断点，把 L2 拼出来的每条【线程】断言变成你亲眼见过的命中现场。**注释与 gdb 冲突时，信 gdb，并把分歧反馈给注释对应文件。**

三级都卡住 → 回 docs/prerequisites.md 查对应小节编号（每个知识点都标了"不学会看不懂什么"）。

---

## 附：实验完成度自检

- [ ] §1 能背出"从零编译两条命令 + 三类产物路径"
- [ ] §2 见过一次 revents:1 全链日志，并能拆 12/17/24
- [ ] §3 截到过"connectEstablished 的 bt 里有 doPendingFunctors"
- [ ] §4 四个小程序全跑过，双控制块那个见过崩溃原文
- [ ] §5 至少做完练习 1 和 3（微秒时间戳 + 空闲踢人）
- [ ] §6 面试题 20 道全部能当场复现代码位置
- [ ] §7 用 gdb 验证过自己的一条错误理解
