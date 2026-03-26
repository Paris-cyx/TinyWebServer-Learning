# Reactor 模式完整调用链路

> 本文档以「一个客户端发起 `GET /index.html HTTP/1.1` 请求，直到服务端返回 `200 OK`」为主线，
> 按时间顺序梳理 Reactor 模式在本项目中的**完整调用链路**，
> 精确标注所经过的 C++ 类名、核心函数与底层系统调用。

---

## 一、服务器启动阶段（`main` 函数 → `server_epoll.cpp`）

```
main()                                          [server_epoll.cpp]
├── Log::Instance()->init(...)                  [log.cpp] 初始化异步双缓冲日志
├── SqlConnPool::Instance()->init(...)          [sql_conn_pool.cpp] 初始化 MySQL 连接池
│
├── socket(AF_INET, SOCK_STREAM, 0)             [syscall] 创建 TCP 监听套接字 → server_fd
├── setsockopt(server_fd, SO_REUSEADDR, ...)    [syscall] 端口复用，防止重启时 "Address in use"
├── bind(server_fd, &server_addr, ...)          [syscall] 绑定 IP:8080
├── listen(server_fd, 10000)                    [syscall] 开始监听，半连接队列长度 10000
│
├── epoll_create1(0)                            [syscall] 创建 epoll 实例 → epoll_fd
│   └── HttpConn::m_epollfd = epoll_fd          [HttpConn] 所有连接对象共享同一 epoll 实例
│
├── addfd(epoll_fd, server_fd, false)           [http_conn.cpp]
│   └── epoll_ctl(EPOLL_CTL_ADD, server_fd,    [syscall] 将监听 fd 加入 epoll，监听 EPOLLIN
│           EPOLLIN|EPOLLET|EPOLLRDHUP)
│
├── socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd) [syscall] 创建信号通知管道
├── setnonblocking(pipefd[1])                   [http_conn.cpp] → fcntl(F_SETFL, O_NONBLOCK)
├── addfd(epoll_fd, pipefd[0], false)           [syscall→epoll_ctl] 管道读端加入 epoll
│
├── addsig(SIGALRM) / addsig(SIGTERM) /         [server_epoll.cpp]
│   addsig(SIGINT)                              统一信号源：信号到来 → sig_handler → send(pipefd[1])
├── alarm(TIMESLOT=5)                           [syscall] 启动 5s 定时器滴答
│
├── ThreadPool pool(4)                          [ThreadPool.h] 创建含 4 个工作线程的线程池
│   └── std::thread([this]{ while(true){        每个工作线程阻塞在 condition_variable::wait()
│           condition.wait(...);                等待任务入队后被唤醒
│           task(); } })  × 4
│
├── users = new HttpConn[MAX_FD]                [HttpConn] 预分配所有连接对象
├── users->initmysql_result(connPool)           [HttpConn] 从 MySQL 缓存用户表到内存 map
└── 进入主事件循环 ↓
```

---

## 二、主事件循环（`epoll_wait` 阻塞等待）

```
while (!stop_server)
└── epoll_wait(epoll_fd, events, MAX_EVENTS, -1)  [syscall] 阻塞，直到有事件就绪
    │
    ├── 事件类型 A：server_fd 可读  → 新连接到达  → §三
    ├── 事件类型 B：pipefd[0] 可读  → 信号到来    → §定时/退出
    ├── 事件类型 C：connfd 可读     → 客户端发数据 → §四
    └── 事件类型 D：connfd 可写     → 内核缓冲区可写→ §六
```

---

## 三、建立连接（`accept` 阶段）

```
if (sockfd == server_fd)                         [server_epoll.cpp]
├── accept(server_fd, &client_addr, &client_len) [syscall] 从完成队列取出已建立的连接 → connfd
│
├── HttpConn::init(connfd, client_addr)          [HttpConn]
│   ├── m_sockfd = connfd                        保存 fd
│   ├── setsockopt(m_sockfd, SO_REUSEADDR, ...)  [syscall] 连接级别端口复用
│   ├── addfd(m_epollfd, connfd, true)           [http_conn.cpp]
│   │   ├── epoll_ctl(EPOLL_CTL_ADD, connfd,     [syscall] 将新连接加入 epoll
│   │   │       EPOLLIN|EPOLLET|EPOLLRDHUP|EPOLLONESHOT)
│   │   │   注：EPOLLONESHOT 保证同一 fd 在同一时刻只被一个线程处理
│   │   └── setnonblocking(connfd)               [syscall→fcntl] 设为非阻塞
│   ├── m_user_count++
│   └── init_parse_state()                       重置 HTTP 状态机到初始状态
│
└── 为该连接创建定时器（util_timer），加入 time_heap 最小堆
    └── timer_lst.add_timer(timer)               O(log N) 堆插入
```

---

## 四、读取数据（`EPOLLIN` 事件 → `recv` 阶段）

```
if (events[i].events & EPOLLIN)                  [server_epoll.cpp]
└── HttpConn::read_once()                        [HttpConn / http_conn.cpp]
    └── while(true)
        └── recv(m_sockfd,                       [syscall] 非阻塞读取，数据追加到 m_read_buf
                m_read_buf + m_read_idx,
                READ_BUFFER_SIZE - m_read_idx, 0)
            ├── 返回 > 0：m_read_idx += bytes_read，继续读（ET 边沿触发必须读尽）
            ├── 返回 -1 & errno==EAGAIN：内核缓冲区已空，退出循环，返回 true
            └── 返回 0 或其他错误：对端关闭连接，返回 false

读取成功后：
├── timer_lst.adjust_timer(timer)                刷新该连接的超时计时
└── pool.enqueue([sockfd]{ users[sockfd].process(); })  [ThreadPool]
    ├── queue_mutex.lock()
    ├── tasks.push(lambda)                       将任务推入任务队列
    ├── queue_mutex.unlock()
    └── condition.notify_one()                   唤醒一个阻塞的工作线程 → §五
```

---

## 五、线程池处理请求（Worker Thread → HTTP 状态机）

```
工作线程（std::thread）被 condition_variable 唤醒
└── task()  即  users[sockfd].process()         [HttpConn / http_conn.cpp]
    │
    ├── process_read()                           驱动 HTTP 有限状态机
    │   ├── 状态 CHECK_STATE_REQUESTLINE
    │   │   └── parse_request_line(text)
    │   │       ├── 解析 "GET /index.html HTTP/1.1"
    │   │       ├── m_method = GET, m_url = "/index.html", m_version = "HTTP/1.1"
    │   │       └── 状态切换 → CHECK_STATE_HEADER
    │   │
    │   ├── 状态 CHECK_STATE_HEADER
    │   │   └── parse_headers(text)              逐行解析 Host/Connection/Content-Length/Cookie 等
    │   │       └── 若遇到空行（请求头结束）→ 返回 GET_REQUEST → do_request()
    │   │
    │   └── (POST 时) 状态 CHECK_STATE_CONTENT
    │       └── parse_content(text)              读取完整 body → do_request()
    │
    ├── do_request()                             [HttpConn] 业务路由
    │   ├── (静态文件请求 GET /index.html)
    │   │   ├── 拼接路径: m_real_file = "resources/index.html"
    │   │   ├── stat(m_real_file, &m_file_stat)  [syscall] 获取文件元信息（大小/权限）
    │   │   ├── open(m_real_file, O_RDONLY)      [syscall] 只读打开文件 → fd
    │   │   ├── mmap(0, st_size, PROT_READ,      [syscall] 将文件映射到进程地址空间（零拷贝）
    │   │   │       MAP_PRIVATE, fd, 0) → m_file_address
    │   │   ├── close(fd)                        [syscall] 关闭文件 fd（mmap 后 fd 可关闭）
    │   │   └── 返回 FILE_REQUEST
    │   │
    │   └── (POST 登录/注册 /2 /3)
    │       ├── 解析 body 中的 user=&passwd=
    │       ├── mysql_query(mysql, "SELECT ...")  [MySQL C API] 查询用户表
    │       └── 返回 GET_REQUEST（JSON 响应）
    │
    └── process_write(ret)                       [HttpConn] 构建 HTTP 响应
        ├── add_status_line(200, "OK")           写入 "HTTP/1.1 200 OK\r\n" 到 m_write_buf
        ├── add_headers(content_length)          写入 Content-Length / Content-Type / Connection 等头部
        ├── (FILE_REQUEST) 设置 iovec 分散写向量
        │   ├── m_iv[0] = { m_write_buf, m_write_idx }   HTTP 响应头
        │   └── m_iv[1] = { m_file_address, st_size }    mmap 映射的文件内容
        └── modfd(m_epollfd, m_sockfd, EPOLLOUT)  [http_conn.cpp]
            └── epoll_ctl(EPOLL_CTL_MOD, connfd,  [syscall] 将监听事件从 EPOLLIN 切换到 EPOLLOUT
                    EPOLLOUT|EPOLLET|EPOLLONESHOT|EPOLLRDHUP)
                主线程下一次 epoll_wait 将返回此 fd 的写就绪事件 → §六
```

---

## 六、发送响应（`EPOLLOUT` 事件 → `writev` 阶段）

```
if (events[i].events & EPOLLOUT)                 [server_epoll.cpp]  （主线程）
└── HttpConn::write()                            [HttpConn / http_conn.cpp]
    └── while(true)
        └── writev(m_sockfd, m_iv, m_iv_count)   [syscall] 分散写：一次系统调用同时发送
            │                                    响应头（m_write_buf）+ 文件数据（mmap 区域）
            ├── 返回 > 0：bytes_to_send -= temp，继续
            ├── 返回 -1 & errno==EAGAIN：内核缓冲区满，
            │   └── modfd(epoll_fd, m_sockfd, EPOLLOUT)  [epoll_ctl] 再次注册等待可写
            └── bytes_to_send <= 0（发送完毕）：
                ├── munmap(m_file_address, st_size)      [syscall] 释放 mmap 内存映射
                ├── if (m_linger=true / keep-alive)
                │   └── modfd(epoll_fd, m_sockfd, EPOLLIN)  [epoll_ctl] 切回监听读事件，复用连接
                └── else
                    └── HttpConn::close_conn()
                        └── removefd(m_epollfd, m_sockfd)
                            ├── epoll_ctl(EPOLL_CTL_DEL, m_sockfd) [syscall]
                            └── close(m_sockfd)                    [syscall]
```

---

## 七、定时器超时处理（信号 → 管道 → tick）

```
alarm(TIMESLOT) 触发 SIGALRM
└── sig_handler(SIGALRM)                         [server_epoll.cpp]
    └── send(pipefd[1], &sig, 1, 0)              [syscall] 将信号编号写入管道

epoll_wait 返回 pipefd[0] 的 EPOLLIN 事件
└── recv(pipefd[0], signals, ...)                [syscall] 读出信号编号
    └── case SIGALRM: timeout = true

主循环末尾（事件处理完后）
└── timer_lst.tick()                             [time_heap] 遍历最小堆，处理所有过期定时器
    └── cb_func(&users_timer[connfd])            [server_epoll.cpp] 超时回调
        ├── epoll_ctl(EPOLL_CTL_DEL, sockfd)     [syscall]
        ├── close(sockfd)                        [syscall]
        └── HttpConn::m_user_count--
└── alarm(TIMESLOT)                              [syscall] 重新设置下一个 5s 滴答
```

---

## 八、全链路系统调用速查表

| 阶段         | 系统调用 / 函数               | 所在位置                  | 说明                             |
|--------------|-------------------------------|---------------------------|----------------------------------|
| 启动         | `socket`                      | `server_epoll.cpp:main`   | 创建 TCP 监听套接字               |
| 启动         | `setsockopt`                  | `server_epoll.cpp:main`   | SO_REUSEADDR 端口复用             |
| 启动         | `bind`                        | `server_epoll.cpp:main`   | 绑定地址与端口                    |
| 启动         | `listen`                      | `server_epoll.cpp:main`   | 进入监听状态，设置 backlog         |
| 启动         | `epoll_create1`               | `server_epoll.cpp:main`   | 创建 epoll 实例                   |
| 启动/连接    | `epoll_ctl(EPOLL_CTL_ADD)`    | `http_conn.cpp:addfd`     | 注册 fd 到 epoll                  |
| 启动         | `socketpair`                  | `server_epoll.cpp:main`   | 创建信号通知管道                  |
| 启动/全程    | `fcntl(F_SETFL, O_NONBLOCK)`  | `http_conn.cpp:setnonblocking` | 设置非阻塞 IO                |
| 事件循环     | `epoll_wait`                  | `server_epoll.cpp:while`  | 阻塞等待 IO 就绪事件              |
| 建立连接     | `accept`                      | `server_epoll.cpp:loop`   | 从完成队列取出连接                |
| 读取数据     | `recv`                        | `http_conn.cpp:read_once` | 非阻塞读取 HTTP 请求数据          |
| 文件访问     | `stat`                        | `http_conn.cpp:do_request`| 获取文件元信息                    |
| 文件访问     | `open`                        | `http_conn.cpp:do_request`| 打开静态文件                      |
| 文件访问     | `mmap`                        | `http_conn.cpp:do_request`| 零拷贝映射文件到内存              |
| 文件访问     | `close`                       | `http_conn.cpp:do_request`| 关闭文件 fd                       |
| 写事件注册   | `epoll_ctl(EPOLL_CTL_MOD)`    | `http_conn.cpp:modfd`     | 切换监听事件为 EPOLLOUT           |
| 发送响应     | `writev`                      | `http_conn.cpp:write`     | 分散写：响应头 + mmap 文件数据    |
| 发送完毕     | `munmap`                      | `http_conn.cpp:write`     | 释放内存映射                      |
| 关闭连接     | `epoll_ctl(EPOLL_CTL_DEL)`    | `http_conn.cpp:removefd`  | 从 epoll 中注销 fd                |
| 关闭连接     | `close`                       | `http_conn.cpp:removefd`  | 关闭连接 fd                       |
| 信号处理     | `send` / `recv`（管道）       | `server_epoll.cpp`        | 统一事件源，信号安全写入管道      |
| 定时器       | `alarm`                       | `server_epoll.cpp`        | 触发 SIGALRM 定时滴答             |

---

## 九、关键设计要点

### 9.1 为什么用 `EPOLLONESHOT`？
每个已连接 fd 注册时带 `EPOLLONESHOT` 标志，保证同一个 fd 的读事件在被处理完之前
不会被 epoll 再次触发——即使数据到来，也不会有两个工作线程同时处理同一连接，
彻底避免数据竞争。处理完毕后通过 `modfd` 重新注册，恢复监听。

### 9.2 为什么 `mmap` + `writev` 而不是 `read` + `write`？
- `mmap` 将文件直接映射到进程地址空间，省去了 `read` 时的一次内核→用户态拷贝。
- `writev` 一次系统调用同时发送不连续的两块内存（响应头 + 文件内容），
  避免了两次 `write` 调用，减少系统调用次数，降低延迟。

### 9.3 为什么用管道统一信号源？
`SIGALRM`/`SIGTERM`/`SIGINT` 的处理函数中只做一件事：
把信号编号 `send` 写入管道写端（`pipefd[1]`）。
主线程通过 `epoll_wait` 监听管道读端（`pipefd[0]`），在事件循环中统一处理信号。
这样避免了在异步信号处理函数中调用非异步信号安全函数，杜绝潜在的死锁与数据竞争。

### 9.4 线程池的"半同步/半异步"模式
- **主线程（Reactor/Dispatcher）**：只负责 IO 事件的感知（`epoll_wait`）与任务派发（`enqueue`）。
- **工作线程（Worker）**：只负责业务逻辑（HTTP 解析、文件读取、响应构建）。
- 两者通过带互斥锁的任务队列 + 条件变量解耦，实现高并发下的职责分离。

---

## 十、完整链路一图流

```
main()
  │
  ├─[初始化]─ socket → bind → listen → epoll_create1
  │            epoll_ctl(ADD, server_fd)   socketpair → epoll_ctl(ADD, pipefd[0])
  │            ThreadPool(4 threads)
  │
  └─[事件循环]─ epoll_wait ──────────────────────────────────────────┐
                    │                                                 │
           server_fd 可读                                   connfd 可读/可写
                    │                                                 │
                 accept(server_fd)                         EPOLLIN: recv → pool.enqueue
                    │                                         (工作线程唤醒)
             HttpConn::init(connfd)                              │
             epoll_ctl(ADD, connfd,                     process_read → 状态机解析
                  EPOLLIN|ONESHOT)                       do_request → stat/open/mmap
             add_timer → time_heap                       process_write → 构建响应头
                                                         modfd → epoll_ctl(MOD, EPOLLOUT)
                                                                 │
                                                       EPOLLOUT: writev(头+文件)
                                                         munmap → close 或 keep-alive
```
