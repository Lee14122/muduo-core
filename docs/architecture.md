# muduo-core 架构与完整调用链（阶段 6 主读物）

> 配套阅读顺序见 `../LEARNING_GUIDE.md`。本文**不引用行号**（注释会让行号漂移），一律用「文件 :: 函数」定位。
> 本文的 ①→⑫ 与源码里所有 `【流程】` 标签是同一套编号。

---

## 1. 一句话看懂整个库

**`Poller` 负责「谁有事」，`Channel` 负责「有事怎么办」，`EventLoop` 负责「什么时候去问」，`Acceptor` 负责「新客户端」，`TcpConnection` 负责「一个客户端的数据与一生」，`TcpServer` 负责「把它们串起来并拥有它们」，`EventLoopThreadPool` 负责「把一个客户端交给一个线程」。**

剩下的（`Buffer`、`Socket`、`InetAddress`、`Timestamp`、`Logger`、`CurrentThread`、`Thread`）全都是支撑设施。

---

## 2. 分层架构图

```
  ┌─────────────────────────────── 用户层 ────────────────────────────────┐
  │  example/testserver.cc   EchoServer::onConnection / onMessage         │
  └──────────────────────────────────┬────────────────────────────────────┘
                          只注册 2~4 个回调 + start()
  ┌──────────────────────────────────▼────────────────────────────────────┐
  │  门面层  TcpServer（拥有 connections_ 表）  TcpConnection（一条连接） │
  └─────┬────────────────────────────┬──────────────────────────┬─────────┘
        │                            │                          │
  ┌─────▼─────────┐          ┌───────▼────────┐        ┌────────▼───────────┐
  │  接入层       │          │  分发层        │        │  线程层            │
  │  Acceptor     │          │  Channel       │        │  EventLoopThread   │
  │  listenfd     │          │  EPollPoller   │        │  Pool / Thread     │
  │               │          │  Poller        │        │  （每线程一个loop）│
  └─────┬─────────┘          └───────┬────────┘        └────────┬───────────┘
        │                            │                          │
  ┌─────▼────────────────────────────▼──────────────────────────▼─────────┐
  │  封装层  Socket(fd)  Buffer(readv/双下标)  InetAddress  Timestamp      │
  │          CurrentThread  Logger  noncopyable                          │
  └──────────────────────────────────┬────────────────────────────────────┘
  ┌──────────────────────────────────▼────────────────────────────────────┐
  │  Linux 内核  socket / bind / listen / accept4 / epoll_ctl /            │
  │              epoll_wait / readv / write / eventfd / shutdown / close   │
  └───────────────────────────────────────────────────────────────────────┘
```

对应 Reactor 模型的三块：**Acceptor = 建立连接；Channel + Poller = 事件分离（demultiplex）；EventLoop + TcpConnection 回调 = 事件处理（handler）**。

---

## 3. 线程与对象布局（以 `setThreadNum(3)` 为例，共 4 个线程）

```
主线程 (用户 main)
├── EventLoop loop                ← 栈对象，就是 baseLoop / mainLoop
│    ├── unique_ptr<Poller> poller_        → EPollPoller(epollfd_A)
│    ├── unique_ptr<Channel> wakeupChannel_ (wakeupFd_A, eventfd)
│    └── ChannelList activeChannels_
└── EchoServer / TcpServer
     ├── unique_ptr<Acceptor>
     │    ├── Socket acceptSocket_   (listenfd)
     │    └── Channel acceptChannel_ (fd=listenfd, 读事件 → Acceptor::handleRead)
     ├── shared_ptr<EventLoopThreadPool>
     └── unordered_map<string, TcpConnectionPtr> connections_   ← 只由主线程读写

IO线程0  ── EventLoopThread#0 ── 子线程栈上 EventLoop(loop_B, wakeupFd_B, epollfd_B)  ─┐
IO线程1  ── EventLoopThread#1 ── 子线程栈上 EventLoop(loop_C, wakeupFd_C, epollfd_C)  ─┼─ 各自 channels_ 里
IO线程2  ── EventLoopThread#2 ── 子线程栈上 EventLoop(loop_D, wakeupFd_D, epollfd_D)  ─┘   装着分到的 connfd 的 Channel

客户端 connfd#1 → loop_B     客户端 connfd#2 → loop_C     客户端 connfd#3 → loop_D ...（轮询 getNextLoop）
```

### 进程里到底有哪些 fd（阶段 6 用 `ls -l /proc/$(pidof testserver)/fd` 亲眼核对）

| fd 类型 | 数量 | 谁拥有 | 谁负责 close |
|---|---|---|---|
| listenfd | 1 | `Acceptor::acceptSocket_` | `~Socket`（随 `Acceptor` 析构） |
| epollfd | 1 + N（主线程 1 个 + 每 subLoop 1 个） | `EPollPoller::epollfd_` | `~EPollPoller` |
| wakeupFd（eventfd） | 1 + N | `EventLoop::wakeupFd_` | `~EventLoop` |
| connfd | 每个客户端 1 个 | `TcpConnection::socket_` | `~Socket`（随 `TcpConnection` 析构） |
| 0/1/2 | stdio | 进程 | 内核 |

这张表能回答面试里很硬的一问：**「fd 到底谁关？」** 答：`Socket` 用 RAII 关，`Channel` 不关 fd，`Poller` 只删注册表，`EventLoop` 不管 fd 生命周期。

---

## 4. 对象持有关系图（谁 new 谁、谁 delete 谁）

```
TcpServer
 ├─ EventLoop* loop_                     不拥有（用户的 mainLoop）
 ├─ unique_ptr<Acceptor>  acceptor_      独占
 │    ├─ Socket acceptSocket_            独占（拥有 listenfd）
 │    └─ Channel acceptChannel_          独占
 ├─ shared_ptr<EventLoopThreadPool>      独占（其实只有它用）
 │    └─ vector<unique_ptr<EventLoopThread>>
 │         └─ Thread thread_ + 子线程栈上的 EventLoop   ← loop_ 裸指针指向它，绝不可 delete
 └─ unordered_map<string, TcpConnectionPtr> connections_  ← shared_ptr 的唯一起点
      └─ TcpConnection（shared_ptr 管理）
           ├─ unique_ptr<Socket>   独占（拥有 connfd）
           ├─ unique_ptr<Channel>  独占
           │    └─ weak_ptr<void> tie_ ──┐ 反向弱引用，防对象先亡
           ├─ Buffer inputBuffer_        │
           ├─ Buffer outputBuffer_       │
           └─ EventLoop* loop_  ─────────┘ 指向 subLoop（不拥有）

Channel ──EventLoop*──▶ EventLoop ──unique_ptr<Poller>──▶ EPollPoller ──unordered_map<int,Channel*>──▶ Channel
                          （裸指针回指，环靠「谁都不拥有谁」打破）
```

三条不变式（本项目全部靠约定，原版靠 assert）：
1. **一个 `Channel` 只属于一个 `EventLoop`**，构造时定死（`Channel::loop_`）。
2. **任何 `Channel` 的注册/删除/回调执行都必须在它的 `ownerLoop()` 线程里**（`EventLoop::isInLoopThread`）。
3. **`TcpConnection` 只能由 `shared_ptr` 持有**（`enable_shared_from_this`），栈上/裸 new 都会崩。

---
## 5. 完整时序：一个客户端从 connect 到彻底销毁（①→⑫）

### 建立阶段

```
① main: EventLoop loop;                       【主线程】
   EventLoop::EventLoop()
     ├─ threadId_ = CurrentThread::tid()        ← 从此这个 loop 只认这个线程
     ├─ poller_ = Poller::newDefaultPoller()    → EPollPoller(epoll_create1)
     ├─ wakeupFd_ = createEventfd()             → eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)
     ├─ wakeupChannel_(this, wakeupFd_)
     ├─ wakeupChannel_ → setReadCallback(handleRead)
     └─ wakeupChannel_ → enableReading()        → epoll_ctl(ADD, wakeupFd, EPOLLIN)
        同时 if (t_loopInThisThread) LOG_FATAL  ← one loop per thread 的强制点

② TcpServer server(&loop, addr, name);         【主线程】
   TcpServer::TcpServer()
     ├─ acceptor_(new Acceptor(loop, listenAddr, reuseport))
     │    ├─ createNonblocking()                → socket(SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC)
     │    ├─ acceptSocket_.setReuseAddr(true)   （SO_REUSEADDR：解决重启时 TIME_WAIT 占端口）
     │    ├─ acceptSocket_.setReusePort(reuseport)
     │    ├─ acceptSocket_.bindAddress(listenAddr)
     │    ├─ acceptChannel_(loop, listenfd)
     │    └─ acceptChannel_ → setReadCallback(bind(Acceptor::handleRead))   ← 注意：此刻还没注册事件
     ├─ threadPool_(new EventLoopThreadPool(loop, name))
     └─ acceptor_->setNewConnectionCallback(bind(TcpServer::newConnection))  ← 两级回调挂接

③ server.start();                              【主线程】
   TcpServer::start()  if (started_.fetch_add(1)==0)
     ├─ threadPool_->start(threadInitCallback_)
     │    └─ for i in 0..numThreads_:  new EventLoopThread(cb, 线程池名+序号) → startLoop()
     │         子线程里：EventLoop loop(栈上) → cond_.notify → loop.loop()   ← 每个 subLoop 自己就开转了
     │         主线程里：cond_.wait(pred: loop_ != nullptr)  ← 保证 startLoop 返回时 loop 一定造好了
     └─ loop_->runInLoop(bind(Acceptor::listen))          【已在主线程，直接执行】
          Acceptor::listen()
            ├─ listenning_ = true
            ├─ acceptSocket_.listen()               → listen(fd, 1024)
            └─ acceptChannel_.enableReading()       → epoll_ctl(ADD, listenfd, EPOLLIN)

④ loop.loop();                                  【主线程，永久阻塞在这】
   EventLoop::loop()  while(!quit_):
     ├─ activeChannels_.clear()
     ├─ pollRetureTime_ = poller_->poll(kPollTimeMs, &activeChannels_)   → epoll_wait
     ├─ for channel : activeChannels_  channel->handleEvent(receiveTime)
     └─ doPendingFunctors()
```

### 接入阶段（新客户端 connect）

```
⑤ listenfd EPOLLIN                            【主线程】
   Channel::handleEvent → handleEventWithGuard → readCallback_()
   Acceptor::handleRead()
     └─ acceptSocket_.accept(&peerAddr) → accept4(listenfd, ..., SOCK_NONBLOCK|SOCK_CLOEXEC) → connfd

⑥ Acceptor 的 NewConnectionCallback_          【主线程】
   TcpServer::newConnection(connfd, peerAddr)
     ├─ ioLoop = threadPool_->getNextLoop()      ← 轮询：loop = loops_[next_++ % size]
     ├─ connName = name_ + "-" + ipPort + "#" + nextConnId_
     ├─ getsockname(connfd, &local) → localAddr
     ├─ TcpConnectionPtr conn(new TcpConnection(ioLoop, connName, connfd, localAddr, peerAddr))
     │    └─ 构造里：socket_(new Socket(connfd))、channel_(new Channel(ioLoop, connfd))
     │              channel_ 绑 4 个回调：handleRead/handleWrite/handleClose/handleError
     │              socket_->setKeepAlive(true)
     ├─ connections_[connName] = conn            ← 引用计数 +1（现在 = 2：map + 局部）
     ├─ conn->setConnectionCallback / setMessageCallback / setWriteCompleteCallback（从 TcpServer 转发）
     ├─ conn->setCloseCallback(bind(TcpServer::removeConnection, this, _1))
     └─ ioLoop->runInLoop(bind(TcpConnection::connectEstablished, conn))
          → 不在 ioLoop 线程 → queueInLoop(cb) → pendingFunctors_ 加一项 → wakeup()
          → write(wakeupFd_, 8 字节)  ★线程跳转 1★  主线程把活交给 IO 线程

⑦ subLoop#k 醒来                              【IO 线程 k】
   epoll_wait 因 wakeupFd 可读返回 → Channel::handleEvent(wakeupChannel) → EventLoop::handleRead
     → read(wakeupFd_, 8 字节)（计数清零）；随后 loop() 末尾 doPendingFunctors()
   TcpConnection::connectEstablished()
     ├─ setState(kConnected)
     ├─ channel_->tie(shared_from_this())        ← 关键：让 Channel 弱引用住自己
     ├─ channel_->enableReading()                → epoll_ctl(ADD, connfd, EPOLLIN)  注册进本线程 epollfd
     └─ connectionCallback_(shared_from_this())  → 用户 onConnection，此时 connected()==true
```

### 数据阶段

```
⑧ 客户端 write 数据                             【IO 线程 k】
   connfd EPOLLIN → Channel::handleEvent → TcpConnection::handleRead(receiveTime)
     ├─ inputBuffer_.readFd(connfd, &errno)
     │    └─ readv(fd, vec[2], iovcnt)：先填 Buffer 可写区，不够再落栈上 64KB，再 append 回 Buffer
     ├─ n>0 → messageCallback_(shared_from_this(), &inputBuffer_, receiveTime) → 用户 onMessage
     ├─ n==0（对端 FIN）→ handleClose()
     └─ n<0 → 恢复 errno → handleError()

⑨ 用户 conn->send(msg)                          【通常就是 IO 线程 k；也可能是别的线程】
   TcpConnection::send(buf)  if (state_==kConnected)
     ├─ 同线程 → sendInLoop(buf.c_str(), buf.size())
     └─ 异线程 → loop_->runInLoop(bind(sendInLoop, this, buf.c_str(), buf.size()))
          ★ std::bind 按值拷贝 string，避免跨线程引用悬垂
   TcpConnection::sendInLoop(data, len)
     ├─ 若 !channel_->isWriting() 且 outputBuffer_ 空 → 直接 write()
     │    └─ 全写完 且 有 writeCompleteCallback_ → queueInLoop(回调)（不注册 EPOLLOUT）
     └─ 没写完（remaining>0）→
          ├─ 越过 highWaterMark_ 时 queueInLoop(highWaterMarkCallback_)
          ├─ outputBuffer_.append(data+nwrote, remaining)
          └─ if (!isWriting()) channel_->enableWriting() → epoll_ctl(MOD, connfd, EPOLLIN|EPOLLOUT)

⑩ 内核发送缓冲区腾出空间                        【IO 线程 k】
   connfd EPOLLOUT → Channel::handleEvent → TcpConnection::handleWrite()
     ├─ 前提检查 channel_->isWriting()（不写就直接报错返回，防止多余唤醒）
     ├─ outputBuffer_.writeFd(connfd, &errno)  → write(fd, peek(), readableBytes())
     ├─ n>0 → outputBuffer_.retrieve(n)
     │    └─ 若已空 → disableWriting()（EPOLLCTL_MOD 去掉 EPOLLOUT，否则会疯狂空转）
     │              → writeCompleteCallback_ → 若 state_==kDisconnecting → shutdownInLoop()
     └─ 至此一条数据完成一次往返
```

### 关闭阶段（本项目最精彩的部分）

```
⑪ 客户端 close()                                【IO 线程 k】
   两种表现都会走到 handleClose：
     · connfd EPOLLIN 可读且 read 返回 0（对正常 FIN 的响应）
     · connfd EPOLLHUP（shutdownWrite 之后，或 RST）→ handleEventWithGuard 第一个分支
   TcpConnection::handleClose()
     ├─ setState(kDisconnected)；channel_->disableAll() → epoll_ctl(DEL, connfd)
     ├─ connectionCallback_(connPtr)             → 用户 onConnection，connected()==false
     └─ closeCallback_(connPtr)  ←必须是函数最后一行★ → TcpServer::removeConnection(conn)

   TcpServer::removeConnection(conn)             【可能在 IO 线程 k，但 connections_ 归主线程】
     └─ loop_->runInLoop(bind(removeConnectionInLoop, this, conn))
          ★线程跳转 2★ IO 线程 → 主线程

   TcpServer::removeConnectionInLoop(conn)       【主线程】
     ├─ connections_.erase(conn->name())         ← 引用计数 -1，但因为 functor 捕获了 conn，对象仍存活
     └─ ioLoop->queueInLoop(bind(connectDestroyed, conn))
          ★线程跳转 3★ 主线程 → IO 线程 k（ functor 里持有一份 shared_ptr）

   TcpConnection::connectDestroyed()             【IO 线程 k】
     ├─ setState(kDisconnected)；channel_->disableAll()；connectionCallback_(...)
     └─ channel_->remove() → EventLoop::removeChannel → EPollPoller::removeChannel
            → channels_.erase(fd) + epoll_ctl(DEL) + set_index(kNew)

   functor 执行完、局部 shared_ptr 析构 → 引用计数归 0
     → ~TcpConnection → ~Channel（不关 fd）→ ~unique_ptr<Socket> → ~Socket → close(connfd)  ★唯一真正的 close★

⑫ 整体退出：用户 quit / 主线程 loop 退出
   EventLoop::quit(){ quit_=true; if(!isInLoopThread()) wakeup(); }
   → loop() 的 while 条件失败 → EventLoopThread::threadFunc 的 loop.loop() 返回
   → 栈上 EventLoop 析构（关 wakeupFd、摘掉 wakeupChannel）→ ~EventLoopThread{ loop_->quit(); thread_.join(); }
```

### 三个非问不可的点

1. **为什么有数据可读时 `handleRead` 里 `read` 返回 0 而不是直接收到 EPOLLHUP？**
   对正常 FIN，内核把连接标为可读并让 `read` 返回 0；EPOLLHUP 一般出现在对端 RST 或本端 `shutdownWrite` 之后。所以本项目两条路都通向 `handleClose`，注释里两处都有 `【易错】`。
2. **为什么 `closeCallback_` 必须是 `handleClose` 的最后一行？**
   它可能一路把 `TcpConnection` 销毁掉（经 ⑪ 的三跳），之后 `this` 就是野指针。任何在其后访问 `this` 的语句都是 UB。见 `src/TcpConnection.cc::handleClose` 的 `【⚠问题】`/`【易错】`。
3. **为什么 `Channel::tie` 还必须有？**
   销毁是异步的：`removeConnectionInLoop` 在**主线程**执行 `erase`，而 `Channel` 可能正在**IO 线程**的 `handleEvent` 里。`tie` 用 `weak_ptr::lock()` 在进回调前先提升出一份 `shared_ptr` 当 guard，保证「回调期间对象一定活着」，提升失败就什么都不做。这是 muduo 最漂亮的一段设计。见 `src/Channel.cc::tie` / `handleEvent`。

---

## 6. 两张状态机表（面试默写级）

### 6.1 `Channel::index_`（定义在 `src/EPollPoller.cc`，声明在 `include/Channel.h`）

```
                        updateChannel()                updateChannel() 且 isNoneEvent()
   kNew(-1) ─────────────────────▶ kAdded(1) ─────────────────────▶ kDeleted(2)
      ▲                              │   ▲                             │
      │                              │   └──── updateChannel() ────────┘（MOD 或直接 ADD）
      │ removeChannel()              │
      └──────────────────────────────┘
```

| 当前 index_ | 调用 updateChannel 时做什么 | 结果 |
|---|---|---|
| kNew | `channels_[fd]=channel` + `epoll_ctl(ADD)` | kAdded |
| kDeleted | 只 `epoll_ctl(ADD)`（map 里还在） | kAdded |
| kAdded 且 `isNoneEvent()` | `epoll_ctl(DEL)` | kDeleted |
| kAdded 且仍有事件 | `epoll_ctl(MOD)` | kAdded |
| 任意 + `removeChannel()` | `channels_.erase(fd)` + 若 kAdded 则 `DEL` | kNew |

### 6.2 `TcpConnection::state_`（`include/TcpConnection.h::StateE`）

```
  kDisconnected ──构造函数──▶ kConnecting ──connectEstablished──▶ kConnected
        ▲                                                    │       │
        │                          用户 shutdown() ──────────┘       │ 对端断/handleClose
        ├── connectDestroyed / handleClose                            │
        └───────────────────────────────────────────────────────────────┘
                     （中间态 kDisconnecting：等 outputBuffer_ 写完再 shutdownWrite）
```

| 状态 | 谁设置 | 含义 | 此时 send 会怎样 |
|---|---|---|---|
| kConnecting | 构造函数 | fd 已 accept，还没注册读事件 | 直接返回（不发） |
| kConnected | `connectEstablished` | 正常工作中 | 正常发送 |
| kDisconnecting | `shutdown` | 正在优雅关闭：先把待发数据发完 | 仍会发，发完再 `shutdownWrite` |
| kDisconnected | `handleClose` / `connectDestroyed` | 已断 | 只打日志（**本项目此处缺 `return`**） |

---

## 7. 跨线程通信的唯一手段：eventfd 唤醒

```
别的线程                        本 loop 所在线程
queueInLoop(cb)                 epoll_wait(阻塞中)
  ├─ lock; pendingFunctors_ += cb
  └─ if(!同线程 || callingPendingFunctors_)  ──write(wakeupFd,8)──▶ wakeupFd EPOLLIN
                                                                    └─▶ epoll_wait 立即返回
                                                                         Channel(wakeupFd)::handleRead
                                                                          → EventLoop::handleRead 读走 8 字节
                                                                         最后 doPendingFunctors() 执行 cb
```

三个必须理解的细节（注释里都有 `【易错】`）：
1. **为什么用 eventfd 而不是 mutex + condition_variable**：要把「阻塞在 `epoll_wait` 的线程」叫醒，唯一办法是让它关注的一个 fd 变为就绪。`eventfd` 就是一个「可被 epoll 关注的信号量」。
2. **为什么要判 `callingPendingFunctors_`**：本线程正在跑 `doPendingFunctors`，新加进来的 functor 不可能被这一轮看到（已经 swap 走了），而下一次 `epoll_wait` 可能睡 10 秒 —— 所以要主动 wake 自己，让下一轮立刻醒。
3. **为什么 swap 而不是遍历 `pendingFunctors_`**：① 执行回调时不持锁（回调里再 `queueInLoop` 不会自锁）；② 回调里 `queueInLoop` 的新 functor 进的是新的 `pendingFunctors_`，本轮不会重复执行；③ 锁的临界区只有一次 swap 的时间。

---

## 8. 全库的锁清单（就两个，面试重点）

| 锁 | 保护什么 | 为什么需要它 | 位置 |
|---|---|---|---|
| `std::mutex mutex_` | `pendingFunctors_` 这个 vector | 唯一的**多线程共享可写容器** | `include/EventLoop.h`，`queueInLoop` / `doPendingFunctors` 临界区 |
| `mutex_ + condition_variable` | `EventLoopThread::loop_` 的初始化握手 | 主线程必须等子线程造好 loop 才能拿到指针 | `src/EventLoopThread.cc::startLoop` / `threadFunc` |
| `std::atomic` | `looping_` `quit_` `callingPendingFunctors_` `started_` `state_` `numCreated_` | 只作为标志位跨线程看，不需要锁 | 各文件 |
| `sem_t`（栈上） | `Thread::start` 里等 tid | 保证 `start()` 返回前 `tid_` 已写入 | `src/Thread.cc::start` |

而下面这些**看起来该加锁但没加**，是 muduo 的精髓：

| 容器 | 为什么不用锁 |
|---|---|
| `Poller::channels_` | 只被本 loop 线程读写（单线程独占） |
| `TcpServer::connections_` | 只在 mainLoop 线程读写，别的线程靠 ⑪ 的跳转 2 把操作交回主线程 |
| `EventLoopThreadPool::next_` | 只在 mainLoop 里轮询时用 |
| `Buffer` 两个下标 | 连接固定属于一个 subLoop，单线程 |
| `TcpServer::nextConnId_` | 同上，只在 mainLoop 中 ++ |

**面试标准答案：muduo 不用「一把大锁保平安」，而是「数据归属线程」+「跨线程一律转成事件投回目标 loop 执行」（runInLoop / queueInLoop + wakeup）。** 这就是所谓的 "share by communication, not by sharing memory"。

---

## 9. 一图看懂 Buffer（阶段 2 默写）

```
      0        kCheapPrepend(8)                readerIndex_              writerIndex_        buffer_.size()
      │               │                              │                          │                   │
      ▼               ▼                              ▼                          ▼                   ▼
      ┌───────────────┬─────────────────┬──────────────────────┬───────────────────────────┐
      │  预留(可放协议头)│ 已读丢弃的垃圾   │   可读 readable       │   可写 writable           │
      └───────────────┴─────────────────┴──────────────────────┴───────────────────────────┘
   prependableBytes() = readerIndex_      readableBytes() = writer - reader      writableBytes() = size - writer

读：readv 写进 writerIndex_ 之后，writerIndex_ += n      （Buffer::readFd）
取：用户 retrieve(len) → readerIndex_ += len            （Buffer::retrieve）
清空：reader = writer = kCheapPrepend                     （Buffer::retrieveAll）
写不够：makeSpace → 能搬就搬（memmove 语义），不能搬就 resize（Buffer::append → ensureWritableBytes）
```

为什么需要两个 Buffer：`inputBuffer_` 由内核 → 用户（`readFd` 填，`onMessage` 取），`outputBuffer_` 由用户 → 内核（`send` 填，`handleWrite` 的 `writeFd` 取）。**两个方向速率不同，必须各自排队**。

---

## 10. 三种并发形态对应本项目的哪种配置

| 形态 | 怎么配 | 代码路径 |
|---|---|---|
| 单 Reactor 单线程 | `setThreadNum(0)`（默认） | `EventLoopThreadPool::getNextLoop` 永远返回 `baseLoop_`，accept 与 IO 全在主线程 |
| 单 Reactor 多线程 | 本项目**做不到**（没有把 IO 交给工作线程队列的机制） | —— 可作为练习：把 `onMessage` 丢进自建任务队列 |
| 多 Reactor 多线程 | `setThreadNum(N)`（EchoServer 用了 3） | mainLoop 只做 accept，N 个 subLoop 各自 `epoll_wait` 做 IO |

> 本项目没有 `Proactor`、没有线程池任务队列、没有定时器 —— 这三样正好是练习 3/4/5 和面试拓展的方向。
