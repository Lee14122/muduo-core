# muduo-core 前置知识补课清单

> 面向对象：**自认"C++ 知识不够"、想只靠读代码+注释就吃透这个网络库的学习者。**
> 用法：读注释版源码时遇到不懂的概念，回到本文对应小节"补课"；每个知识点都按
> **概念是什么 → 在本项目哪里能看到 → 不学会看不懂什么** 三段式组织。
>
> 定位约定：本文**一律用「文件名 + 函数名/变量名」定位，不写行号**（行号会随注释插入漂移）。
> 全链路编号 ①→⑫ 与源码注释里【流程】标签同一套体系；⑤→⑦ 的"线程跳转 1"、⑪ 的"线程跳转 2/3"
> 也沿用之。想先看全链路，读源码注释，或直接读 docs/experiments.md 第 3 节用 gdb 亲眼看。

---

## 1. Linux 文件描述符与 socket API 全家桶

### 1.1 文件描述符（fd）是什么

- **概念**：fd 是进程私有的非负小整数，是内核打开文件/socket/管道的"把手"。每个进程有自己的
  fd 表（文件描述符表），表项指向内核 file 结构；fork 继承、exec 默认保留（除非 O_CLOEXEC）。
  0/1/2 是 stdin/stdout/stderr，之后从 3 开始分配。
- **本项目哪里能看到**：
  - **src/Acceptor.cc** 的 **createNonblocking()**：socket() 返回一个整数 fd，装进 **include/Socket.h** 的私有成员 **sockfd_**（const int，一经构造不再变）。
  - **include/EventLoop.h** 的 **wakeupFd_**：eventfd 返回的 fd，本质也是"一个可读写的文件"。
  - **src/EPollPoller.cc** 的 **EPollPoller()**：**epoll_create1()** 的返回值同样是 fd——"epoll 实例"在 Linux 里就是一个文件。
  - **include/Channel.h** 的 **fd_**：Channel 不拥有 fd，只"观察"它；真正 close 的责任在 **src/Socket.cc** 的 **~Socket()**。
  - 动手观察：**ls -l /proc/$(pidof testserver)/fd**（见 docs/experiments.md 第 2 节）。
- **不学会看不懂什么**：看不懂"Channel 只管事件不管 fd 释放"这条所有权规则，会误以为 **~Channel()** 里该 close；看不懂为什么 **src/EventLoop.cc** 的 **~EventLoop()** 要手动 close(wakeupFd_) 而 wakeupChannel_ 不用——其实 wakeupChannel_ 析构不关，关 fd 的活儿是 ~EventLoop 自己干的。

### 1.2 socket API 全家桶

下表是"这个库用到的每一个系统调用"。**本项目里没有出现在表外的 API**，别在别处找。

| API | 语义要点（返回值/阻塞性/errno） | 本项目出现位置 |
|---|---|---|
| socket | 创建未绑定 TCP 端点；flags SOCK_NONBLOCK=非阻塞、SOCK_CLOEXEC=exec 时自动关 | **src/Acceptor.cc** 的 **createNonblocking()** |
| bind | 把 fd 绑到 ip:port；0 成功 -1 失败（EADDRINUSE/EACCES） | **src/Socket.cc** 的 **bindAddress()** |
| listen | 被动监听，backlog 是 accept 完成队列上限 | **src/Socket.cc** 的 **listen()**（backlog 硬编码 1024） |
| accept/accept4 | 从完成队列取出已连接 socket；-1/EAGAIN（非阻塞无新连接）、/ECONNABORTED（握手后被 RST）、/EMFILE/ENOBUFS 等 | **src/Socket.cc** 的 **accept()**（用 accept4） |
| connect | 客户端发起，本项目不出现（对端由 **nc** 充当） | 见 docs/experiments.md 第 2 节实验 |
| send/recv | 与 write/read 的区别只在 flags（MSG_NOSIGNAL/MSG_PEEK/MSG_DONTWAIT）；send(...,MSG_NOSIGNAL) 可让 EPIPE 老实返回而不触 SIGPIPE | **未使用**（本仓库全用裸 read/write，可核实） |
| read | 从 fd 读；非阻塞空数据时 -1/EAGAIN；socket 返回 0=对端 FIN | **src/EventLoop.cc** 的 **handleRead()**（读 eventfd 8 字节） |
| write | 写 fd；socket 满时 -1/EAGAIN(EWOULDBLOCK)；对端已关触发 SIGPIPE（本项目未处理，见 1.4） | **src/EventLoop.cc** 的 **wakeup()**；**src/Buffer.cc** 的 **writeFd()** |
| readv | 一次系统调用读入多段内存（iovec 数组） | **src/Buffer.cc** 的 **readFd()**（两段：Buffer 剩余空间 + 栈上 64KB extrabuf） |
| writev | 一次写出多段 | **未使用**（本仓库没有异步日志；原版是否在异步日志/FileUtil 里用它攒批写，以原版源码为准） |
| shutdown(fd, SHUT_WR) | 发 FIN 半关闭写端；读端还活着；与 close 的本质区别见 3.3 | **src/Socket.cc** 的 **shutdownWrite()**，被 **src/TcpConnection.cc** 的 **shutdownInLoop()** 调用 |
| close | 减 fd 引用计数，最后一个引用才真正释放；socket 上 close 不等发完的数据 | **src/Socket.cc** 的 **~Socket()**、**src/EPollPoller.cc** 的 **~EPollPoller()**、**src/EventLoop.cc** 的 **~EventLoop()**、**src/Acceptor.cc** 的 **handleRead()**（无回调时丢弃 connfd 直接 close） |
| setsockopt | 选项开关，返回在本项目全被忽略（见 4 条 A2 【⚠问题】） | **src/Socket.cc** 的 **setTcpNoDelay()/setReuseAddr()/setReusePort()/setKeepAlive()** |
| epoll_create1/epoll_ctl/epoll_wait | 见第 2 节 | **src/EPollPoller.cc** 的构造/**update()**/**poll()** |
| eventfd | 一个"可 read/write 的 8 字节计数器"，跨线程唤醒用 | **src/EventLoop.cc** 的 **createEventfd()** |
| getsockname/getsockopt | 取 socket 本机地址/错误码 | **src/TcpServer.cc** 的 **newConnection()**（getsockname 填 localAddr）；**src/TcpConnection.cc** 的 **handleError()**（getsockopt SO_ERROR） |
| sendfile | 内核态直接文件→socket 零拷贝；&offset 会被更新 | **src/TcpConnection.cc** 的 **sendFileInLoop()** |

- **不学会看不懂什么**：第 ⑤/⑧/⑩/⑪ 环节所有【系统调用】注释；**readFd/writeFd** 的返回值语义（0/负数/EAGAIN）是 **handleRead/handleWrite** 所有分支判断的前提。

### 1.3 sockaddr_in 与字节序

- **概念**：IPv4 地址族结构 **sockaddr_in** = sin_family(AF_INET) + sin_port(16 位，**网络字节序**=大端) + sin_addr(32 位，同样大端) + 填充。x86 本机是小端，所以 port 出入必须 **htons/ntohs** 转换；地址用 **inet_pton/inet_ntop**（**inet_addr** 是老式替代品，出错值与 255.255.255.255 无法区分）。
- **本项目哪里能看到**：
  - **include/InetAddress.h** 的成员 **addr_**：整个类就是 sockaddr_in 的包装，**getSockAddr()** 强转 sockaddr* 交给 bind/accept。
  - **src/InetAddress.cc** 的 **InetAddress()**（构造函数）：**::htons(port)** 与 **::inet_addr(ip.c_str())**。
  - **src/InetAddress.cc** 的 **toIp()**：**::inet_ntop** 反向转字符串；**toIpPort()**：同一 buf 上 strlen+sprintf 拼接（为什么安全见 src/InetAddress.cc 的 toIpPort 注释）；**toPort()**：**::ntohs**。
  - **src/TcpServer.cc** 的 **newConnection()**：accept 只给对端地址，本机端口要用 getsockname 再问内核。
- **不学会看不懂什么**：日志里 **Connection UP : 127.0.0.1:xxxxx** 的端口怎么来的；**bindAddress()** 里 sizeof(sockaddr_in) 这个参数在填什么；为什么 **Socket::accept** 里 memset addr 是防脏字段。

### 1.4 非阻塞 fd 与 errno 家族

- **概念**：非阻塞（O_NONBLOCK）意味着"没数据/没空间时不睡"：返回 -1 并把 **errno** 置成 EAGAIN（socket 语境别名 EWOULDBLOCK）。必须**读/写循环直到 EAGAIN**（ET 模式）或依赖 LT 再次通知（本项目）。
- **必须记住的五个错误码**：
  - **EAGAIN/EWOULDBLOCK**：正常的"暂时没数据/暂时写不进"，不是错误。看 **src/TcpConnection.cc** 的 **sendInLoop()**：**errno != EWOULDBLOCK** 才打 LOG_ERROR；**src/TcpConnection.cc** 的 **sendFileInLoop()** 同样。
  - **EPIPE / ECONNRESET**：对端已关闭/被 RST 后还写。看 **sendInLoop()** 的 **faultError = true** 分支。连锁风险：**写已断 socket 内核默认发 SIGPIPE 直接打死进程**——**本仓库没有任何 signal(SIGPIPE,SIG_IGN) 调用**（Buffer.cc 已写【⚠问题】）。原版据称在初始化时忽略 SIGPIPE（以原版源码为准；本仓库确凿没有任何 SIGPIPE 处理），或用 **send(..., MSG_NOSIGNAL)** 让它老实返回 EPIPE。
  - **ECONNABORTED**：accept 拿到一半，对方握手中途死了。看 **src/Socket.cc** 的 **accept()** 与 **src/Acceptor.cc** 的 **handleRead()**：本项目接受一次、不循环，LT 下没问题；原版是否循环 accept 及其理由，以原版源码为准（本环境无法核实）。
  - **EMFILE**：进程 fd 耗尽（ulimit -n）。看 **src/Acceptor.cc** 的 **handleRead()**：专门 if (errno == EMFILE) 打日志。
  - **EINTR**：系统调用被信号打断（不是错误，重试即可）。看 **src/EPollPoller.cc** 的 **poll()**：**saveErrno != EINTR** 才记 LOG_ERROR。
- **不学会看不懂什么**：**handleRead/handleWrite/sendInLoop** 每个 errno 分支在防什么；为什么 fd 必须先设非阻塞（**accept4(SOCK_NONBLOCK)**、**socket(SOCK_NONBLOCK)**、**eventfd(EFD_NONBLOCK)**）才能挂进 epoll 的事件循环——否则一次 read 就能把整个 loop 线程挂死。

---

## 2. IO 多路复用

### 2.1 五种 IO 模型归位（POSIX 分类）

| 模型 | 谁负责等 | 谁负责拷贝数据 | 本项目 |
|---|---|---|---|
| 阻塞 IO | 内核等 + 进程睡 | 进程 read 拷贝 | 不用（会把 loop 线程睡死） |
| 非阻塞 IO | 进程轮询重试 | 进程拷贝 | 配合多路复用使用 |
| IO 多路复用 | 内核等，进程阻塞在 epoll_wait（**同步**：数据仍由进程拷贝） | 进程 read/write 拷贝 | **本项目：epoll(LT) + 非阻塞**，见 **EPollPoller::poll** |
| 信号驱动 IO | 内核发 SIGIO | 进程拷贝 | 不用（信号语义太脏） |
| 异步 IO（AIO/io_uring） | 内核全程 | **内核拷贝完再通知** | 不用；Proactor 模型见 6.3 |

- **易混点**：epoll 是"多路复用"，**不是异步 IO**——它只告诉你"可以读了"，数据还得你亲自 read。

### 2.2 select / poll / epoll 对比

| | select | poll | epoll |
|---|---|---|---|
| fd 上限 | FD_SETSIZE=1024 | 无硬上限 | 无硬上限（受系统） |
| 每次调用传什么 | 把全部 fd 位图拷进内核再拷出 | 把全部 pollfd 数组拷进拷出 | 只拷**就绪的**回来 |
| 找出就绪 | 用户 O(n) 扫位图 | 用户 O(n) 扫数组 | 内核把就绪项挂链表，O(就绪数) |
| 触发模式 | LT | LT | LT + ET |
| 本项目 | 无（DefaultPoller 的 POLL 分支返回 nullptr，设 MUDUO_USE_POLL 直接崩，见 src/DefaultPoller.cc 的【⚠问题】注释） | 无 | **src/EPollPoller.cc** 全套 |

### 2.3 epoll 的内核数据结构：兴趣红黑树 + 就绪双向链表

- **epoll_create1** → 建一个"epoll 对象"（内核：红黑树 + ready list + wait 队列），它本身以 fd 形式存在（**EPollPoller::epollfd_**）。
- **epoll_ctl(ADD/MOD/DEL)** → 在**红黑树**里增删改"兴趣项"（每个 Channel 一个节点，记录了 fd + events + data 指针）。本项目：每一次 **enableReading/disableWriting** 最终打到 **EPollPoller::update()**，用 **EPOLL_CTL_ADD/MOD**，**EPOLL_CTL_DEL** 在 **Channel::disableAll → updateChannel(kDeleted)**（见EPollPoller.cc 里关于 kDeleted 的【易错】）和 **removeChannel** 里。
- 当网卡收包/发送缓冲腾出空间，**中断软中断上下文**把红黑树里对应的表项挂上**就绪双向链表（rdllist）**，并唤醒睡在 **epoll_wait** 的线程。
- **epoll_wait** → 只摘就绪链表，所以复杂度是 **O(活跃事件数)** 而不是 O(总连接数)——这是它能扛 C10K 的根因。
- **data 联合体**：epoll_event.data 是 union（fd / ptr / u64 三选一）。本项目写 **data.ptr = channel**（**EPollPoller::update()**），回调时 **fillActiveChannels()** 再 **static_cast<Channel\*>(events_[i].data.ptr)**——写 data.fd 会被 ptr 覆盖（对应文件的注释）。
- **不学会看不懂什么**：kNew/kAdded/kDeleted 状态迁移图；为什么 1 万个连接只有 3 个活跃时 epoll 仍然飞快。

### 2.4 LT（水平触发）vs ET（边缘触发），以及本项目为什么"LT + 非阻塞"

- **LT**：只要"有数据可读"这一**电平**成立，每次 epoll_wait 都会重复通知。→ 每次事件读一次也行，没读完下轮还来。**可以慢，不会丢。**
- **ET**：只在"没数据→有数据"的**跳变**时通知一次；必须一口气读到 EAGAIN。→ 快，但漏读即永久挂起（直到下一次新数据触发）。
- **本项目**：**EPollPoller::update()** 构造 event.events 时取 **channel->events()**，其值来自 **Channel.h** 的 kReadEvent(EPOLLIN|EPOLLPRI)/kWriteEvent(EPOLLOUT)——**没有 EPOLLET**，所以是 LT。于是 **TcpConnection::handleRead()** 里 **readFd 一次**即可，不必循环读到 EAGAIN；**sendInLoop** 写一半也敢交给下轮 EPOLLOUT。LT+非阻塞的组合含义：循环永不睡死（非阻塞保证单次 read/write 有限时间返回）、正确性不依赖"读干净"（LT 兜底）。
- **对比原版**：本库注册事件从不带 EPOLLET（可核实）；原版是否同为 LT+非阻塞，以原版源码为准。选 ET 的 epoll 库（如 nginx）必须配 nonblocking+循环读，二者是一个整体设计不能拆开抄。
- **不学会看不懂什么**：**handleRead()** 为什么"敢"只读一次；ET 下同样的代码会丢数据。

### 2.5 事件位与 revents 数字（读日志必备）

**Channel::handleEventWithGuard()** 会打 **revents:%d**，数字必须会拆位（详细对照表和练习在 docs/experiments.md 第 2 节）：
1=EPOLLIN、2=EPOLLPRI、4=EPOLLOUT、8=EPOLLERR、16=EPOLLHUP、32=EPOLLRDHUP（本项目没注册它，几乎不会出现）。
**注册（events_）** 与 **发生（revents_）** 是两个字段：**Channel.cc** 的 kReadEvent = EPOLLIN|EPOLLPRI（所以 **Acceptor::handleRead** 被叫醒时 revents 常见 1；EPOLLPRI 对应带外/urgent，罕见）。

---

## 3. TCP 与半关闭

### 3.1 三次握手与 accept 队列

- **概念**：SYN/SYN-ACK/ACK 在内核里完成；完成后连接进**accept 完成队列**等待用户 accept。**backlog** 限制的就是这个队列（半连接 SYN 队列由内核自调）。队列满 → 内核丢 SYN（客户端指数重传），服务端不会立刻可见。
- **本项目哪里能看到**：**src/Socket.cc** 的 **listen()**（backlog=1024）；**Socket::accept()** 用 **accept4** 直接拿非阻塞 fd；**src/Acceptor.cc** 的 **handleRead()** 每次事件只取一个 connfd（LT 下多个连接排队会多次触发，够用）；全链路即本套注释里的 ③→⑤→⑥。
- **不学会看不懂什么**：为什么压测下端口"能连但卡很久才 UP 日志"（完成队列积压）；**Acceptor::handleRead()** 与 **TcpServer::newConnection()** 的分工边界。

### 3.2 FIN 与 TIME_WAIT

- **概念**：每方向各有一个 FIN；四次挥手中**主动关闭方**进入 TIME_WAIT（约 2MSL），目的是可靠回收最后的 ACK、防止旧报文串门。TIME_WAIT 占用**本机端口/四元组**，导致重启服务 bind 同端口失败。
- **本项目哪里能看到**：解法是 **src/Socket.cc** 的 **setReuseAddr()**，**src/Acceptor.cc** 构造 **Acceptor()** 时打开它——所以"重启秒起、不报 Address already in use"（Linux 上 SO_REUSEADDR 的特权用途正是"允许 bind 处于 TIME_WAIT 的本机地址"）。**shutdown(SHUT_WR)** 后进入半关闭（见 3.3）。实验：**ss -tan state time-wait** 观察。
- **不学会看不懂什么**：**setReuseAddr 与 setReusePort 的区别**（面试题 14）；TIME_WAIT 是客户端还是服务端留的（看谁先 close，不背口诀）。

### 3.3 shutdown 的效果：SHUT_WR 语义

- **概念**：**shutdown(fd, SHUT_WR)** = 发 FIN，写端永久关闭；**读端不受影响**，还能收对端的剩余数据。close(fd) 不同：读+写都关，且只减引用计数，若 dup 过 fd 甚至不发 FIN。
- **本项目哪里能看到**：**src/Socket.cc** 的 **shutdownWrite()**；调用者是 **src/TcpConnection.cc** 的 **shutdownInLoop()**（只在 **!channel_->isWriting()**，即 outputBuffer_ 清空后）。用户 API 是 **TcpConnection::shutdown()**。对端视角：**recv 返回 0**，于是 **handleRead()** 里 n==0 → **handleClose()**。半关闭链路的完整编号见 ⑪。
- **动手**：**example/testserver.cc** 的 **EchoServer::onMessage()** 里那行被注释掉的 **conn->shutdown()**——打开它，Echo 变短连接。
- **不学会看不懂什么**：**TcpConnection::shutdown()** 与 **handleClose()** 的"等发完再关"设计（兜底路径见 src/TcpConnection.cc 的 handleWrite 注释）。

### 3.4 EPOLLHUP / EPOLLERR 何时出现

- **EPOLLHUP**：连接两端都关了（收到 FIN 后自己也关了/或对端进程死透）。注意 **handleEventWithGuard()** 的判断：**(revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)** 才走 closeCallback_——如果 HUP 和 IN 同时出现，先处理读（别丢最后一段数据，对应文件的注释），数据读干净后 read 返回 0 才走正常关闭链。
- **EPOLLERR**：socket 发生错误（典型：对端 RST 后你还写、或连接被 keepalive 判死）。它和 EPOLLHUP **无需注册也会上报**。本项目 **TcpConnection::handleError()** 只做一件事：**getsockopt(SO_ERROR)** 把错误码打出来。
- 对照：revents=16(HUP)、8(ERR)、12(ERR|OUT，写挂断连接的经典组合)、24(HUP|ERR)、17(HUP|IN，先读后关)。见 docs/experiments.md 第 2 节位拆解表。

### 3.5 Nagle 与 TCP_NODELAY

- **概念**：Nagle 算法攒小包（有未确认小包时新小包等 ACK），省带宽但增延迟（和 delayed ACK 互相能卡 40ms）。**TCP_NODELAY=1 禁用它**。
- **本项目哪里能看到**：**src/Socket.cc** 的 **setTcpNoDelay()** 实现了开关，但**全工程没人调用它**——即默认 Nagle 开启。对照：同一个构造函数家族里 **TcpConnection::TcpConnection()** 主动调用了 **socket_->setKeepAlive(true)**（TCP 层探活）。本项目可核实：setTcpNoDelay 无人调用（即按内核默认 Nagle 开启跑），且 TcpConnection 没向用户暴露 NODELAY 开关；原版的默认行为与开关暴露方式，以原版源码为准。
- **不学会看不懂什么**：交互式协议（IM/游戏）为什么要手动关 Nagle；为什么实验里"打一个字立刻回显"还是快（小包规则例外+回显方向有数据触发 flush）。

### 3.6 粘包/拆包：为什么本项目不管

- **概念**：TCP 是**字节流**，没有消息边界；"一次 read 读到的可能是半个 JSON 或三个 HTTP 头"——这不是 TCP 的 bug，是应用协议必须自己定边界（长度前缀/分隔符/固定长）。所谓"粘包"是**应用层协议缺失**的症状。
- **本项目哪里能看到**：**include/Buffer.h** 只有裸字节窗口（**peek/retrieve/append**），没有任何消息语义；**src/TcpConnection.cc** 的 **handleRead()** 把"这一批到达的字节"原样交给 **messageCallback_**；**example/testserver.cc** 的 **EchoServer::onMessage()** 干脆 **retrieveAllAsString** 全吃回去——在"人在终端手敲 + echo 原样回"的场景里，消息边界靠人的回车天然形成，所以 demo 成立。
- **判断**：本库的 Buffer/Callbacks 通篇是字节流 API，没有任何消息边界字段（可核实）。"muduo 不处理粘包"是设计哲学（"我传字节流，边界是你的协议的事"）；原版据称另有 prepend 方便你写长度头，以原版源码为准。所以这不是 bug 是留白，面试这么答。
- **不学会看不懂什么**：为什么 onMessage 参数是 **Buffer\***（一个可能含多条消息的池子），而不是"一条消息"。

---

## 4. 多线程与同步

### 4.1 pthread、pid 与 tid

- **概念**：Linux 里线程在内核也是任务，有独立 **tid**（syscall(SYS_gettid)）；**pthread_self()** 返回的是用户态不透明句柄，**不等于 tid**；调试器/ss -p/ps -T 看的是 tid。
- **本项目哪里能看到**：**src/CurrentThread.cc** 的 **cacheTid()** 用 ::syscall(SYS_gettid)；**include/EventLoop.h** 的 **threadId_** 记录 loop 归属线程；**EventLoop::isInLoopThread()**（头文件 inline）就是比较这两个 tid。**Logger 打日志想带线程、EventLoopThread 想自证"one loop per thread"都靠它。**
- **不学会看不懂什么**：【线程】标签（mainLoop / subLoop#N）的判定依据；gdb **info threads** 里那一列数字的含义。

### 4.2 线程局部存储 TLS 与 __thread

- **概念**：**__thread**（GCC 关键字，C++11 写法 thread_local）声明的变量**每个线程一份独立副本**，互不干扰。
- **本项目哪里能看到**：**include/CurrentThread.h** 的 **t_cachedTid**（每线程缓存自己的 tid，免重复 syscall）；**src/EventLoop.cc** 文件级变量 **t_loopInThisThread**——它是 one loop per thread 的执行机构：构造时若本线程已有 loop 直接 LOG_FATAL。
- **不学会看不懂什么**："同一份代码不同线程进来读到不同值"的直觉；为什么"禁止一个线程创建多个 EventLoop"能不加锁就检查。

### 4.3 mutex 与 unique_lock

- **概念**：**std::mutex** 互斥锁；**std::unique_lock** 是可中途加解锁/可转移所有权的 RAII 包装（**condition_variable::wait 必须用它**，lock_guard 不行，因为 wait 内部要解锁）。临界区写法：**{ } 代码块圈住范围**。
- **本项目哪里能看到**：**src/EventLoop.cc** 的 **queueInLoop()**——**{ std::unique_lock lock(mutex_); pendingFunctors_.emplace_back(cb); }** 锁只罩住 push_back，唤醒放锁外；**doPendingFunctors()** 同样小临界区（swap 那一手）。**src/EventLoopThread.cc** 的 **startLoop()** 与 **threadFunc()** 保护 **loop_** 指针。
- **不学会看不懂什么**：**pendingFunctors_** 为什么是"生产者（任意线程）/消费者（loop 线程）"共享；**A4 条目**里 swap 的两个理由都建立在"锁要短"上。

### 4.4 condition_variable：谓词写法与虚假唤醒

- **概念**：**cond.wait(lock, pred)** 等价 **while(!pred()) cond.wait(lock);**。必须带谓词，因为：① **虚假唤醒**：没人 notify 也可能醒来；② **丢失唤醒**：notify 发生在 wait 之前（生产者太快）时，没有谓词你就永远睡错了。**醒来 ≠ 条件成立，醒来 = "再去看看条件"。**
- **本项目哪里能看到**：**src/EventLoopThread.cc** 的 **startLoop()**：**cond_.wait(lock, [this]{ return loop_ != nullptr; })**；**threadFunc()** 里在持锁段写 **loop_ = &loop; cond_.notify_one();**。（原版另有 MutableCondition 一类的自封装条件，其形态以原版源码为准；本项目就是裸 mutex+condition_variable+lambda 谓词。）
- **不学会看不懂什么**：**startLoop()** 的返回时机为什么"保证 loop_ 已就绪"（阻塞→唤醒→判谓词→才返回的完整时序，即全链路 ③ 里"起 N 个 subLoop"的实现）。

### 4.5 sem_t（POSIX 信号量）

- **概念**：计数信号量：**sem_init(&s,false,0)**（false=仅线程间），sem_wait 原子地"计数>0 则减一并返回，否则睡"，sem_post 加一唤醒。**比 mutex 多了"可跨线程当一次性事件旗"的用法。**
- **本项目哪里能看到**：**src/Thread.cc** 的 **start()**：栈上 **sem_t sem**; 子线程 lambda 先 **tid_ = CurrentThread::tid(); sem_post(&sem);** 再 **func_();**；父线程 **sem_wait(&sem)** 才从 start() 返回。这保证了**start() 返回前 tid_ 必已被子线程写好**——一个用信号量建立 happens-before 的教科书样本（对应文件里的【易错】：sem 在调用者栈上、lambda 捕获 this 引用，Thread 对象不能先析构）。
- **不学会看不懂什么**：面试题 19（Semaphore 在 Thread::start 的作用）；为什么不能用"睡 100ms 再读 tid_"碰运气。

### 4.6 detach / join，以及"std::thread 析构为什么 terminate"

- **概念**：join = 等线程结束；detach = 撒手不管，线程成孤儿由系统回收。C++ 标准规定：**std::thread 对象析构时若仍 joinable，直接 std::terminate()**——防止你无意中泄漏或悬着一个没人收尸的线程。
- **本项目哪里能看到**：**src/Thread.cc** 的 **~Thread()**：**if (started_ && !joined_) thread_->detach();** 就是兜这个底的；**Thread::join()** 转调 thread_->join()。**src/EventLoopThread.cc** 的 **~EventLoopThread()**：**loop_->quit(); thread_.join();**——这是全链路 ⑫ 退出的真身。
- **进阶**：Thread 用 **shared_ptr\<std::thread\>** 而非裸成员，绕开 std::thread 不可拷贝不可赋值导致的构造顺序问题（线程要立刻拿到 this 写成员，但成员初始化列表时又还没构造完），细节读Thread.cc 的注释。
- **不学会看不懂什么**：detach 之后进程退出时线程被硬切，**~EventLoopThread 一定 join** 才能保证"loop 线程干净退出、析构顺序可控"。

### 4.7 原子变量与 CAS

- **概念**：**std::atomic** 保证单次读/写不可分割且跨线程可见；底层常用 **CAS（compare-and-swap）**：比较旧值、相等才换，否则重试——**fetch_add/compare_exchange** 都是它。**但原子只覆盖"单个变量这一次操作"，不组合多个语句。**
- **本项目哪里能看到**：**include/EventLoop.h** 的 **looping_ / quit_ / callingPendingFunctors_**（atomic_bool）；**TcpConnection.h** 的 **state_**（atomic_int）；**src/TcpServer.cc** 的 **start()** 里 **started_.fetch_add(1)==0** 防重复启动；**include/Thread.h** 的 **static std::atomic_int numCreated_**（**src/Thread.cc** 里类外定义，见 5.5）。
- **不学会看不懂什么**：**quit()** 的"置标志 + 必要时 wakeup"两步为什么不算无锁正确（标志给 loop 线程看，epoll_wait 有 10 秒超时兜底，src/EventLoop.cc 里 kPollTimeMs 的注释）。

### 4.8 内存序最低限度认知

- **概念**：atomic 默认 seq_cst 够用；知道两件事即可读本项目：① 编译器/CPU 会重排，seq_cst 下同一原子操作自带同步边；② **跨变量的顺序要靠锁或显式 memory_order**，"flag=true 然后 publish 数据"这种模式裸 atomic 是有坑的（release/acquire 或互斥锁解决）。
- **本项目哪里能看到**：**src/EventLoop.cc** 的 **doPendingFunctors()** 与 **queueInLoop()** 的配合：真正发布 pendingFunctors_ 内容的是 **mutex_**（不是 callingPendingFunctors_）；callingPendingFunctors_ 只是"要不要顺手 wakeup"的提示。细究有一处经典竞态（标志清除与新入队交错），对应文件已标【易错】，此竞态与原版机制是否同源，以原版源码为准。
- **不学会看不懂什么**：面试里"swap 为什么能缩临界区"的说服力；"muduo 靠锁而不是靠无锁队列"这句话。

---

## 5. C++ 语法补课清单

> 每条都遵循：概念 → 项目位置 → 不学会看不懂什么。学完这一节，源码里所有【语法】标签都该无障碍。

### 5.1 RAII

- **概念**：资源获取即初始化——把"释放"绑在析构上，异常/提前 return 都不漏。**unique_ptr** 是它的标准载体。
- **本项目**：**include/Socket.h**（fd 在 ~Socket 释放）；**include/EPollPoller.h** 析构 close(epollfd_)；**src/EventLoop.cc** 析构关 wakeupFd_；**TcpServer.h** 用 unique_ptr 持有 acceptor_、shared_ptr 持有 threadPool_；**Thread.cc** 析构里 detach 也是 RAII 思想（对象死了别留 joinable 线程）。
- **不学会**：读不懂 ⑪ 里"引用计数归零 → ~TcpConnection → ~Socket 才 close(connfd)"这条销毁链，会到处找 free 而找不到。

### 5.2 = delete / = default / protected 构造析构

- **概念**：=delete 禁止某函数（编译期）；=default 要编译器生成。把构造/析构设 protected：外部不能 new/栈建，但**能继承**——" mixin 基类"惯用法；拷贝设 delete + protected 构造 = noncopyable。
- **本项目**：**include/noncopyable.h** 全套四种写法（拷贝构造与赋值 = delete；构造与析构 = default + protected）；**Logger** 继承 noncopyable，构造函数是 private，只能走 **Logger::instance()**。**EventLoop/TcpServer/Acceptor/Channel/Buffer 全都 noncopyable**——数一遍就知道"资源/线程绑定的对象不许拷贝"的共识。
- **不学会**：为什么 **TcpServer::newConnection** 里 **TcpConnectionPtr conn(new TcpConnection(...))** 只能这么建，而写值拷贝 **TcpConnection c = *conn** 编译就红。
- **顺带**：noncopyable.h 里有被注释掉的一行"返回值 void"，通行描述：那是原版的写法（以原版仓库为准），include/noncopyable.h 的注释讲清了区别。

### 5.3 const 成员函数与 const 引用参数

- **概念**：函数尾 const 承诺不改对象（const this），const 对象才能调用；不加 const 则"只有非 const 对象可用"。setter 收 **const T&** 避免拷贝、且立刻拷进成员。
- **本项目**：**Timestamp::toString() const**；**Buffer** 的 **readableBytes()/writableBytes()/peek()** 等一整套 const；**begin()/beginWrite()** 各两版（const/非 const 重载，**&*buffer_.begin()** 拿到可写裸指针那版非 const）；**Socket::fd() const**；回调注册全走 const&（**TcpServer::setConnectionCallback(const ConnectionCallback&)**）。
- **对照**：**src/Logger.cc** 的 **Logger::log(std::string msg)** 按值收——多拷一次，A1 的【易错】条目（原版据称按 const std::string& 收，以原版源码为准）。
- **不学会**：满屏 const/& 读成噪音，就看不出"谁可能改状态、谁保证不改"这条最重要的接口信息。

### 5.4 前向声明能做什么、不能做什么

- **概念**：**class X;** 告诉编译器 X 是个类型。能做：声明 **X\* / X& / 引用类型形参、函数声明（返回 X\*）**——因为指针大小固定 8 字节；不能：成员里放 **X 值**、继承、调用 X 的方法、任何需要 sizeof 的事。
- **本项目**：**include/EventLoop.h** 顶部 **class Channel; class Poller;**（正文只存指针和引用参数）；**include/Channel.h** 的 **class EventLoop;**；**include/Socket.h** 的 **class InetAddress;**；**include/Callbacks.h** 干脆只 **class Buffer; class TcpConnection; class Timestamp;** 然后 **shared_ptr\<TcpConnection\>**——shared_ptr 成员在头文件里只需要"知道有这类型"，析构要完整类型（在 cpp 里 include）。
- **不学会**：为什么头文件互相都不 include 对方——那不是为了省编译，是**打断循环依赖**（EventLoop 用 Channel，Channel 也用 EventLoop）。

### 5.5 static 成员：类内声明、类外定义

- **概念**：静态成员属于类不属于对象；类内是**声明**，还要在某个 .cc 里给一份**定义**（否则 undefined reference；static constexpr int 除外——见项目反例）。
- **本项目**：**Channel.h** 声明 kNoneEvent/kReadEvent/kWriteEvent，**src/Channel.cc** 开头**三条类外定义赋 epoll 常量**——这是全工程"声明/定义分离"样本；**Thread.h** 的 **static std::atomic_int numCreated_** 在 **src/Thread.cc** 第一行定义；反例：**EPollPoller.h** 的 **kInitEventListSize** 只在类内用 static const int 初始化、没在类外定义——也**能过**，因为它只被用作常量表达式（值编译期展开），没被"取地址"式使用；C++17 后 inline 变量也免了这规矩。**Timestamp.h** 的 static 函数成员 **now()** 无状态，不涉及该规则。
- **不学会**：为什么改一个常量要动两个文件；以及 include/noncopyable.h 的【语法】注释里"类内声明类外定义"那句在指什么。

### 5.6 inline

- **概念**：inline 是给链接器的"允许多份等价定义"许可（打破 ODR 一次定义规则），不是"函数内联展开"的请求（优化器说了算）。定义在头里的函数建议加 inline。
- **本项目**：**CurrentThread.h** 的 **tid()**（头里定义+inline，且注释里那句"只在当前文件起作用"不准确，src/CurrentThread.cc 的注释会纠正：真正"每线程一份"的是 __thread 变量，不是 inline）。**Buffer.h/Channel.h/EventLoop.h** 大量类内定义的成员函数**隐式 inline**。
- **不学会**：为什么多个 .cc include 同一个头不重复定义炸链接。

### 5.7 std::function / std::bind / placeholders

- **概念**：**std::function\<R(Args...)\>** = 可调用对象的统一盒子（类型擦除：函数指针/lambda/成员函数+对象/bind 结果都装）。**std::bind(&C::f, obj, _1, _2)** 把"成员函数 + 对象"打包成一个普通可调用物；**_1/_2**（std::placeholders::）是占位形参。bind 在**创建时就把实参按值拷进闭包**，lambda 同理。
- **本项目**：**include/Callbacks.h** 六个 using 别名全是 std::function（这是"读这个库必须先读它"的核心文件）；注册现场：**src/EventLoop.cc** 的构造函数 **std::bind(&EventLoop::handleRead, this)**；**src/TcpConnection.cc** 的构造 **std::bind(&TcpConnection::handleRead, this, std::placeholders::_1)**；**src/Acceptor.cc** 的构造 **bind(&Acceptor::handleRead, this)**；**src/TcpServer.cc** 的构造 **bind(&TcpServer::newConnection, this, _1, _2)** 与 **removeConnection**；用户侧 **example/testserver.cc** 的 **EchoServer** 构造。按值拷贝的样本：**src/TcpConnection.cc** 的 **send(const std::string&)** 跨线程路径里 bind 拷贝 string（A5 必讲条目：这正是线程安全的原因）。
- **不学会**：满屏 bind 在干什么、_1 是什么、"回调里那个 this 什么时候失效"。
- **lambda 等价**：把 bind(&C::f, this, _1) 换成 [this](Timestamp t){ f(t); }——读懂其一即可，项目通篇 bind 是历史风格。

### 5.8 智能指针三兄弟

- **概念**：unique_ptr 独占（不可拷贝只 move，零开销）；shared_ptr 计数共享（控制块+引用计数，原子增减）；weak_ptr 观察者（不延长寿命，**lock() 提升成 shared 或空**）。
- **本项目**：unique_ptr——**EventLoop.h** 的 poller_/wakeupChannel_、**TcpConnection.h** 的 socket_/channel_、**EventLoopThreadPool.h** 的 threads_；shared_ptr——**TcpServer.h** 的 threadPool_、**Callbacks.h** 的 **TcpConnectionPtr = shared_ptr\<TcpConnection\>**、**Thread.h** 的 thread_；weak_ptr——**Channel.h** 的 tie_。
- **不学会**：**TcpServer::~TcpServer()** 里 **item.second.reset()** + 栈上局部 conn 那招（A5 注释讲"为什么这样对象才能回到 loop 线程里析构"）；以及全工程最重的销毁时序（⑪）。

### 5.9 enable_shared_from_this 与 weak_ptr::lock

- **概念**：**enable_shared_from_this\<T\>** 让"对象内部"合法拿到**共享同一控制块**的 shared_ptr（**shared_from_this()**）；前提：**对象已经活在某个 shared_ptr 里**，否则抛 **bad_weak_ptr**。自己再套一个 **shared_ptr\<T\>(this)** = 两个独立控制块 → 各自计数 → **双重释放**。**weak_ptr::lock()**：活着就提升一份强引用，死了给空——"用前提升"保命标准姿势。
- **本项目**：**TcpConnection.h** 的类声明就挂着 **public std::enable_shared_from_this\<TcpConnection\>**；合法用法遍布 **src/TcpConnection.cc**（**connectEstablished/handleRead/sendInLoop** 里把 **shared_from_this()** 递给回调）与 **Channel::tie(shared_from_this())**；保命用法在 **src/Channel.cc** 的 **handleEvent()**：**std::shared_ptr\<void\> guard = tie_.lock();** 失败就不碰回调。
- **配套实验**：**docs/experiments.md 第 4 节**有"双控制块崩 + lock 保命"两个可编译小程序。
- **不学会**：**handleRead** 为什么给用户回调传 shared_ptr；**tie** 到底在防哪一次野指针。

### 5.10 移动语义与 std::move

- **概念**：std::move 把左值转成右值引用，让"接管资源"（偷指针/偷缓冲区）代替"完整拷贝"；对 std::function 就是偷小对象指针，代价极小。
- **本项目**：**Channel.h** 四个 setXxxCallback：**xxxCallback_ = std::move(cb);**（形参本来按值收，move 只是把栈上的空壳转所有权）；**Thread.cc** 构造 **func_(std::move(func))**（std::thread 不可拷贝，函数对象必须 move 进构造）。
- **不学会**："形参 by value + std::move"为什么是标准写法而不是画蛇添足。

### 5.11 位运算标志

- **概念**：| 置位、& ~ 清位、& 测试、==0 判空集；宏全是 2 的幂（1,2,4,8,16…），一个 int 当一组开关。
- **本项目**：**src/Channel.cc** 开头三个 static const：**kNoneEvent=0、kReadEvent=EPOLLIN|EPOLLPRI、kWriteEvent=EPOLLOUT**；**Channel.h** 的 enableReading：**events_ |= kReadEvent; update();**、disableWriting：**events_ &= ~kWriteEvent; update();**、isWriting：**events_ & kWriteEvent**。epoll 侧 **revents_** 的位测试在 **src/Channel.cc** 的 **handleEventWithGuard()**。
- **不学会**：日志里 revents:12 这种数字完全无法阅读，⑩ 段注释白读。

### 5.12 enum 与 enum class

- **概念**：老 enum 的成员泄漏到外层作用域、可隐式转 int（命名污染、乱比较）；**enum class** 作用域内（**E::kA** 才能写）、禁止隐式转。类内老 enum 至少由类名限定，算是半个折中。
- **本项目**：**include/Logger.h** 的 **enum LogLevel{INFO,ERROR,FATAL,DEBUG}**——**非 scoped，全局可见**（这也是 Logger.h 里 LOG_INFO 宏里那句 logger.setLogLevel(INFO) 不用写 Logger:: 的原因，还兼容 setLogLevel(int)）；**TcpConnection.h** 的 **enum StateE{kDisconnected,...}**（类内）；**TcpServer.h** 的 **enum Option**。面试高频：为什么 state_ 用 atomic_int 而不是 atomic\<StateE\>。
- **不学会**：**setState(kDisconnecting)** 在语法层面怎么成立、(int)state_ 强转为什么常见。

### 5.13 可变参数宏与 ##__VA_ARGS__

- **概念**：**#define F(fmt, ...)** 里 __VA_ARGS__ 展开除 fmt 外的全部参数；**##__VA_ARGS__** 的 ## 是"吃逗号"：后面为空时把前一个逗号一起删掉（GNU 扩展，严格 ISO 写法是 /##__VA_ARGS__/ 或 C++20 的 __VA_OPT__）。
- **本项目**：**include/Logger.h** 四个 LOG_XXX 宏全是 **snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__)** 的模板。注意铁律：**这些宏的续行反斜杠之间禁止插注释**（注释会在每个 #define 上方写注释，内部不碰）。
- **不学会**：**LOG_INFO("x=%d", fd)** 展开成什么；为什么 LOG_DEBUG 在没定义 MUDEBUG 时是**空宏**（编译器整段丢弃，零成本）。

### 5.14 do{...}while(0)

- **概念**：把多语句包成"一个语句"：① 让 if/else 挂宏时不出现 else 错配；② 允许宏里用 return/break 语义包裹局部变量；③ 分号习惯不变。
- **本项目**：**include/Logger.h** 每个 LOG_XXX 宏体内都套 **do { ... } while(0)**（且反斜杠续行，同 5.13 铁律）。
- **不学会**：为什么不用 **{ }** 就行（if(...) LOG_INFO(x); else ... 会语法错/语义错）。

### 5.15 变长数组（VLA）是 GNU 扩展

- **概念**：**char buf[n+32];** n 运行时才定——**C99 有 VLA，C++11 及以后标准从未有**；GCC/Clang 默认（gnu++ 模式）当扩展支持，严格模式 **-std=c++17（非 gnu++）** 直接报错。它活在栈上，过大就爆栈。
- **本项目**：**src/EventLoopThreadPool.cc** 的 **start()** 里 **char buf[name_.size() + 32];**——src/EventLoopThreadPool.cc 标了【对比原版】（原版是否同样用 VLA，以原版源码为准），并说明"它依赖顶层 CMAKE_CXX_STANDARD 生成 -std=gnu++17 才编得过"（与 CMake 注释 A7 联动，见顶层 CMakeLists.txt 注释）。
- **不学会**：为什么"哪天有人加了 set(CMAKE_CXX_EXTENSIONS OFF)，这里先炸"。

---

## 6. 事件驱动编程范式

### 6.1 回调与闭包：捕获 this 的生命周期陷阱

- **概念**：事件驱动 = 把"函数"作为参数交给框架，稍后由框架调用（控制反转）。闭包（bind 结果/lambda）**捕获裸 this 指针**时，回调执行那刻对象可能早死 → UB。解法：捕获 shared_from_this() 强引用，或像 Channel::tie 那样用 weak_ptr 在触发时提升。
- **本项目**：安全样本——**src/TcpServer.cc** 的 **newConnection()** 里 bind 了 **&TcpServer::removeConnection, this**：TcpServer 活得比所有 conn 久（~TcpServer 先清 connections_）所以成立；危险边界——**src/Channel.cc** 的 **handleEvent()**：Channel 的 readCallback_ 里藏着别人的 this（**TcpConnection::handleRead** 绑定的），靠 **tie()** 保护。用户层 **example/testserver.cc** 的 EchoServer 把 **this** 绑进两个回调：它活在 main 栈上、比 loop 晚死吗？——main 里 **server 在 loop 之后声明**？实际 main 里 loop 先建、server 后建，析构反序：server 先死、loop 后死，但 loop.loop() 已返回，不再有回调，成立且脆弱（读testserver.cc 的【易错】）。
- **不学会**：Channel::tie 的注释与 handleEvent 的 if(guard) 分支完全读不进去。

### 6.2 one loop per thread

- **概念**：每个线程恰好一个 EventLoop 在跑（epoll_wait 阻塞→处理就绪事件→跑 pendingFunctors_），loop 与线程绑定后**不再迁移**；跨线程操作一律收敛成"往目标 loop 的队列塞回调"。
- **本项目**：**src/EventLoop.cc** 构造里 **t_loopInThisThread** 强制；**threadId_** 出生时钉死；**src/EventLoopThread.cc** 的 **threadFunc()** 把 EventLoop 放**子线程栈上**（一一对应；对应文件注释：这就是 EventLoopThreadPool::~EventLoopThreadPool 里"Don't delete loop, it's stack variable"的原因）；**src/TcpServer.cc** 的 **newConnection()** 用 **getNextLoop()** 轮询把连接分给某个 loop。
- **不学会**：**Channel::ownerLoop()**、"为什么 EventLoop 不能跨线程"（4.2/4.3 的锁只保护队列，不保护 epoll 操作本身）、⑥⑦⑪ 的三次线程跳转。

### 6.3 Reactor vs Proactor

- **概念**：Reactor：**内核通知"就绪"**，数据拷贝由你自己 read/write 做，**事件循环线程全程参与 IO**。Proactor（IOCP/io_uring 风格）：**内核通知"完成"**，拷贝已由内核做完，循环线程只做分发。
- **本项目**：教科书 Reactor——**EPollPoller::poll** 只收集就绪 Channel，真正读写发生在 **TcpConnection::handleRead/handleWrite**（loop 线程调用）。
- **不学会**："muduo 是 Reactor，不是 epoll 就叫异步"这句话，面试被追一句"那 Proactor 长什么样"就露怯。

### 6.4 三种 Reactor 形态 ↔ 本项目配置对照

| 形态 | 结构 | 本项目对应配置 | 代码可见位置 |
|---|---|---|---|
| 单 Reactor 单线程 | 一个 loop 干 accept+IO+业务 | **server.setThreadNum(0)**（或不设；默认 numThreads_ 就是 0） | **src/EventLoopThreadPool.cc** 的 **start()** 里 **numThreads_ == 0** 分支；**getNextLoop()** 恒回 baseLoop |
| 单 Reactor 多线程 | 一个 loop 做 accept+IO，**业务**丢线程池 | 本库**没有现成配置**；手工用 **EventLoop::queueInLoop** 自建业务池即可模拟（把 onMessage 的重活塞给独立线程池） | 机制在 **src/EventLoop.cc** 的 **queueInLoop()/doPendingFunctors()** |
| 多 Reactor 多线程 | mainReactor 只 accept；N 个 subReactor 各自 epoll 管 IO | **server.setThreadNum(3)** —— **example/testserver.cc** 的 **EchoServer 构造**默认就是它 | **src/TcpServer.cc** 的 **start()**、**newConnection()**、**src/EventLoopThread.cc** 全部 |
- 注意："多线程"三个字在三种形态里指代的东西不同（业务线程 vs IO 线程），这正是面试爱考的区分点；"主 Reactor + 子 Reactor"的说法出自 muduo 官方资料，以原版文档/源码为准。

### 6.5 本项目的三次线程跳转（与 ①→⑫ 对齐）

1. **跳转 1（⑥→⑦）**：**src/TcpServer.cc** 的 **newConnection()**（mainLoop）→ **ioLoop->runInLoop(connectEstablished)** → **src/EventLoop.cc** 的 **queueInLoop + wakeup**（eventfd）→ subLoop 的 **doPendingFunctors()** 执行。
2. **跳转 2（⑪）**：subLoop 的 **TcpConnection::handleClose()** → closeCallback_ = **src/TcpServer.cc** 的 **removeConnection()**（仍在 subLoop！）→ **loop_->runInLoop** 把 **removeConnectionInLoop** 送回 mainLoop。
3. **跳转 3（⑪）**：mainLoop 里 **connections_.erase** 后 → **ioLoop->queueInLoop(connectDestroyed)** 送回原 subLoop 做最终 disableAll/remove，**对象引用计数在 subLoop 里归零析构**。
- 读懂这三次，等于读懂"为什么跨线程安全靠队列+唤醒，而不是到处加锁"。

---

## 附：学完本文的 5 分钟自测（能答再往下读代码）

1. EPOLLHUP=16，revents=17 是什么组合？代码里为什么 **handleEventWithGuard** 要判 **&& !(EPOLLIN)**？（→ 3.4/5.11）
2. **queueInLoop** 里为什么锁只罩 emplace_back，wakeup 在锁外？（→ 4.3）
3. **shared_from_this** 和 **shared_ptr\<TcpConnection\>(this)** 的本质区别？崩在什么时候？（→ 5.9）
4. **startLoop()** 的 wait 不带谓词会有什么后果？虚假唤醒和丢失唤醒分别指什么？（→ 4.4）
5. 把 **setThreadNum(3)** 改成 0，全链路 ⑥⑦ 还发生吗？哪些【线程】标签变了？（→ 6.4/6.5）

> 配套实操手册：**docs/experiments.md**（编译、观察、gdb、小程序、练习、面试题）。
