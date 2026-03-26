# HTTP GET 请求端到端生命周期全链路分析

> 本文档以一次 `GET /index.html HTTP/1.1` 请求为线索，严格对照 `main` 分支源码，按时间顺序逐步追踪请求从服务器启动到返回 `200 OK` 的完整路径。每一步均标注 **C++ 类/函数**、**源文件** 及 **关键 Linux 系统调用（含行号）**，并明确 **主线程与工作线程的交接点**。

---

## 目录

1. [阶段一：服务器初始化（main 启动）](#阶段一服务器初始化main-启动)
2. [阶段二：主线程事件循环阻塞等待](#阶段二主线程事件循环阻塞等待)
3. [阶段三：客户端连接到达（accept）](#阶段三客户端连接到达accept)
4. [阶段四：客户端发送数据（read）](#阶段四客户端发送数据read)
5. [阶段五：主线程 → 工作线程交接（核心切换点）](#阶段五主线程--工作线程交接核心切换点)
6. [阶段六：工作线程处理 HTTP 请求](#阶段六工作线程处理-http-请求)
7. [阶段七：主线程发送响应（write）](#阶段七主线程发送响应write)
8. [阶段八：连接保持或关闭](#阶段八连接保持或关闭)
9. [系统调用速查表](#系统调用速查表)
10. [线程交互流程图](#线程交互流程图)

---

## 阶段一：服务器初始化（main 启动）

**文件**：`src/server_epoll.cpp`，`main()` 函数（第 51 行起）

### 步骤 1.1 — 日志系统初始化

```
src/server_epoll.cpp:53
Log::Instance()->init("./log/ServerLog", 0, 2000, 800000, 800)
```

- **类**：`Log`（单例，`src/log.h` / `src/log.cpp`）
- 内部通过 `BlockQueue<std::string>` 实现生产者-消费者异步写盘，写日志不阻塞主线程。

### 步骤 1.2 — 数据库连接池初始化

```
src/server_epoll.cpp:56
SqlConnPool::Instance()->init("localhost", 3306, "tiny", "123456", "webserver", 8)
```

- **类**：`SqlConnPool`（单例，`src/sql_conn_pool.h` / `src/sql_conn_pool.cpp`）
- 预先建立 8 条 MySQL 长连接，避免每次请求都重新握手。

### 步骤 1.3 — 创建监听 socket

```
src/server_epoll.cpp:61
int server_fd = socket(AF_INET, SOCK_STREAM, 0);   // ★ 系统调用 socket()
```

- 设置端口复用（`setsockopt` + `SO_REUSEADDR`）。

### 步骤 1.4 — 绑定地址并开始监听

```
src/server_epoll.cpp:70
bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));  // ★ 系统调用 bind()

src/server_epoll.cpp:71
listen(server_fd, 10000);                                               // ★ 系统调用 listen()
```

- 绑定到 `0.0.0.0:8080`，内核开始将到达的 SYN 放入半连接队列，三次握手完成后放入全连接队列（backlog=10000）。

### 步骤 1.5 — 创建 epoll 实例并注册 server_fd

```
src/server_epoll.cpp:73
epoll_fd = epoll_create1(0);                        // ★ 系统调用 epoll_create1()

src/server_epoll.cpp:77
addfd(epoll_fd, server_fd, false);
  └─ src/http_conn.cpp:42  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);  // ★ 系统调用 epoll_ctl()
  └─ 事件标志：EPOLLIN | EPOLLET | EPOLLRDHUP（边缘触发，不含 EPOLLONESHOT）
```

- `HttpConn::m_epollfd` 静态变量同步保存 `epoll_fd`，供所有连接对象共用。

### 步骤 1.6 — 创建信号管道（统一事件源）

```
src/server_epoll.cpp:80
socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);         // ★ 系统调用 socketpair()
```

- 写端（`pipefd[1]`）设为非阻塞，在信号处理函数 `sig_handler()` 中通过 `send()` 写入信号值（第 28 行）。
- 读端（`pipefd[0]`）加入 epoll，主循环中统一读取并处理 `SIGALRM`/`SIGTERM`/`SIGINT`。

### 步骤 1.7 — 创建线程池与连接对象数组

```
src/server_epoll.cpp:90
ThreadPool pool(4);
```

- **类**：`ThreadPool`（`src/ThreadPool.h`，第 13 行构造函数）
- 构造函数立即启动 4 个工作线程，每个线程在内部 lambda 中阻塞在 `condition.wait()`，等待任务入队。

```
src/server_epoll.cpp:91
users = new HttpConn[MAX_FD];     // 预分配 1000 个连接对象（以 fd 为下标）

src/server_epoll.cpp:92
users->initmysql_result(SqlConnPool::Instance());
  └─ src/http_conn.cpp:58  // 查询 user 表并将结果缓存到全局 map<string,string> users
```

### 步骤 1.8 — 开启定时器

```
src/server_epoll.cpp:88
alarm(TIMESLOT);     // 每隔 5 秒发送 SIGALRM，驱动超时检测
```

- **类**：`time_heap`（最小堆，`src/lst_timer.h`）

---

## 阶段二：主线程事件循环阻塞等待

**文件**：`src/server_epoll.cpp`，第 101 行

```cpp
while (!stop_server) {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);  // ★ 系统调用 epoll_wait()，第 102 行
    ...
}
```

- **主线程在此阻塞**，直到有以下任一事件就绪：
  - `server_fd` 可读 → 新连接到达
  - `pipefd[0]` 可读 → 信号到达
  - 已有连接 fd 可读 → 客户端发送数据
  - 已有连接 fd 可写 → 内核缓冲区可写（响应可以发送）

---

## 阶段三：客户端连接到达（accept）

**文件**：`src/server_epoll.cpp`，第 113–138 行

```
客户端发起 TCP 三次握手
    → 内核完成握手，放入 server_fd 全连接队列
    → epoll_wait() 返回，events[i].data.fd == server_fd
```

### 步骤 3.1 — 摘取新连接

```cpp
// src/server_epoll.cpp:116
int connfd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);  // ★ 系统调用 accept()
```

- 从内核全连接队列摘取一个已完成三次握手的连接，返回新的文件描述符 `connfd`。

### 步骤 3.2 — 初始化 HttpConn 对象

```cpp
// src/server_epoll.cpp:124
users[connfd].init(connfd, client_addr);
```

- **类/方法**：`HttpConn::init()`（`src/http_conn.cpp`，第 78 行）
- 内部调用：
  ```
  src/http_conn.cpp:83
  addfd(m_epollfd, sockfd, true);
    └─ src/http_conn.cpp:42  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event)  // ★ 系统调用 epoll_ctl()
    └─ 事件标志：EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT（含 ONESHOT，用完须通过 modfd() 重新 arm）
    └─ setnonblocking(fd) → fcntl(fd, F_SETFL, O_NONBLOCK)   // 设为非阻塞
  ```
- `init_parse_state()` 重置所有解析状态（状态机归位到 `CHECK_STATE_REQUESTLINE`）。

### 步骤 3.3 — 绑定超时定时器

```cpp
// src/server_epoll.cpp:130-137
util_timer *timer = new util_timer;
timer->cb_func = cb_func;        // 回调：删除 epoll 注册并关闭 fd
timer->expire  = time(NULL) + 3 * TIMESLOT;  // 15 秒后过期
timer_lst.add_timer(timer);      // 加入最小堆
```

- **类**：`time_heap`，`util_timer`（`src/lst_timer.h`）
- 若连接 15 秒内无活动，`cb_func` 将被 `tick()` 调用，执行 `epoll_ctl(DEL)` + `close(fd)`。

---

## 阶段四：客户端发送数据（read）

**文件**：`src/server_epoll.cpp`，第 159–173 行

```
客户端发送：GET /index.html HTTP/1.1\r\nHost: ...\r\n\r\n
    → 数据到达内核接收缓冲区
    → epoll_wait() 返回，events[i].events & EPOLLIN
```

### 步骤 4.1 — 主线程读取数据到 HttpConn 缓冲区

```cpp
// src/server_epoll.cpp:161
users[sockfd].read_once()
```

- **类/方法**：`HttpConn::read_once()`（`src/http_conn.cpp`，第 131 行）

```cpp
// src/http_conn.cpp:135（循环读取直到 EAGAIN）
bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                  sizeof(m_read_buf) - m_read_idx, 0);  // ★ 系统调用 recv()
```

- **边缘触发（EPOLLET）** 要求循环读取直到 `errno == EAGAIN`，确保将内核缓冲区数据全部读入 `m_read_buf`（大小 2048 字节）。
- 读取成功后，刷新该连接的定时器（`timer_lst.adjust_timer(timer)`）防止超时踢出。

---

## 阶段五：主线程 → 工作线程交接（核心切换点）

**文件**：`src/server_epoll.cpp`，第 166–168 行

```cpp
pool.enqueue([sockfd] {
    users[sockfd].process();   // 将处理逻辑封装为 lambda，投入任务队列
});
```

- **类/方法**：`ThreadPool::enqueue()`（`src/ThreadPool.h`，第 37 行）

```cpp
// src/ThreadPool.h:38（加锁入队）
{ std::unique_lock<std::mutex> lock(queue_mutex);
  tasks.emplace(std::forward<F>(f)); }

// src/ThreadPool.h:39（通知一个空闲工作线程）
condition.notify_one();
```

### ✅ 交接点总结

| 角色 | 职责 | 代码位置 |
|------|------|---------|
| **主线程** | epoll 事件监听、accept、recv 读数据、enqueue 投递任务、writev 发送响应 | `server_epoll.cpp` |
| **工作线程** | HTTP 解析、业务逻辑、响应构建（写入缓冲区） | `http_conn.cpp::process()` |

> **数据传递方式**：共享全局数组 `users[fd]`（`HttpConn` 对象）+ lambda 捕获 `sockfd`。主线程将数据读入 `users[sockfd].m_read_buf`，工作线程从中解析并将响应写入 `users[sockfd].m_write_buf`；主线程读取 `m_write_buf` 并调用 `writev()` 发送。

---

## 阶段六：工作线程处理 HTTP 请求

工作线程被条件变量唤醒后，执行如下调用链：

### 步骤 6.1 — process() 入口

```
src/http_conn.cpp:692
HttpConn::process()
  └─ process_read()   // HTTP 解析
  └─ process_write()  // 响应构建
  └─ modfd(epoll_fd, sockfd, EPOLLOUT)  // 触发主线程 write 事件
```

### 步骤 6.2 — 有限状态机解析请求行

```
src/http_conn.cpp:354
HttpConn::process_read()
  │
  ├─ [状态 CHECK_STATE_REQUESTLINE]
  │    └─ parse_line()           // src/http_conn.cpp:145，扫描 \r\n，定位一行
  │    └─ parse_request_line()   // src/http_conn.cpp:169
  │         ├─ strpbrk(text, " \t")       → 切出方法 "GET"
  │         ├─ strcasecmp(method, "GET")  → 设置 m_method = GET
  │         ├─ strspn/strpbrk            → 切出 URL "/index.html"
  │         ├─ strcasecmp(version, "HTTP/1.1") → 校验协议版本
  │         └─ 状态转移 → CHECK_STATE_HEADER
  │
  ├─ [状态 CHECK_STATE_HEADER]（逐行循环）
  │    └─ parse_headers()        // src/http_conn.cpp:197
  │         ├─ "Connection: keep-alive" → m_linger = true
  │         ├─ "Content-Length: 0"      → m_content_length = 0
  │         ├─ "Host: ..."              → m_host = text
  │         └─ 空行 \r\n → Content-Length==0，直接返回 GET_REQUEST
  │
  └─ 返回 GET_REQUEST，调用 do_request()
```

### 步骤 6.3 — do_request()：路由与文件映射

```
src/http_conn.cpp:382
HttpConn::do_request()
  │
  ├─ 构建磁盘路径："resources" + "/index.html" → "resources/index.html"
  ├─ stat(m_real_file, &m_file_stat)   // 检查文件是否存在、权限
  │
  ├─ int fd = open(m_real_file, O_RDONLY)    // 打开文件
  │
  ├─ src/http_conn.cpp:494
  │    m_file_address = (char*)mmap(0, m_file_stat.st_size,  // ★ 系统调用 mmap()
  │                                 PROT_READ, MAP_PRIVATE, fd, 0);
  │    // 将文件内容映射到进程地址空间，实现零拷贝传输
  │
  ├─ close(fd)   // mmap 后立即关闭 fd，映射仍有效
  └─ 返回 FILE_REQUEST
```

### 步骤 6.4 — process_write()：构建 HTTP 响应头

```
src/http_conn.cpp:617
HttpConn::process_write(FILE_REQUEST)
  │
  ├─ add_status_line(200, "OK")       → "HTTP/1.1 200 OK\r\n"         // 第 565 行
  ├─ add_headers(m_file_stat.st_size)
  │    ├─ add_content_length()        → "Content-Length: <size>\r\n"  // 第 584 行
  │    ├─ add_content_type()          → "Content-Type: text/html\r\n" // 第 587 行
  │    ├─ add_response("Connection: keep-alive\r\n")                   // 第 571 行
  │    └─ add_blank_line()            → "\r\n"  // 头部结束              // 第 613 行
  │
  ├─ 设置 scatter-gather IO 结构：
  │    m_iv[0] = { m_write_buf, m_write_idx }   // HTTP 响应头（内存）
  │    m_iv[1] = { m_file_address, st_size }    // 文件内容（mmap 映射）
  │    m_iv_count = 2
  └─ 返回 true
```

### 步骤 6.5 — 触发写事件，通知主线程

```
src/http_conn.cpp:702
modfd(m_epollfd, m_sockfd, EPOLLOUT)
  └─ src/http_conn.cpp:55  epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event)  // ★ 系统调用 epoll_ctl()
     // 事件改为 EPOLLOUT | EPOLLET | EPOLLONESHOT，通知主线程"可以发送响应了"
```

> **至此工作线程任务完成**，控制权交还给主线程的 `epoll_wait()` 循环。

---

## 阶段七：主线程发送响应（write）

**文件**：`src/server_epoll.cpp`，第 176–185 行

```
epoll_wait() 返回，events[i].events & EPOLLOUT
    → 调用 users[sockfd].write()
```

### 步骤 7.1 — 零拷贝分散写

```
src/server_epoll.cpp:178
users[sockfd].write()
  └─ src/http_conn.cpp:507  HttpConn::write()
```

```cpp
// src/http_conn.cpp:519（核心：散列 IO，一次系统调用发送头 + 文件）
temp = writev(m_sockfd, m_iv, m_iv_count);  // ★ 系统调用 writev()
```

- `m_iv[0]`：响应头（已写入 `m_write_buf`）
- `m_iv[1]`：文件内容（`mmap` 映射的 `m_file_address`）
- 若内核缓冲区满（`EAGAIN`），调用 `modfd(..., EPOLLOUT)` 重新注册，下次 epoll 触发继续发送。

### 步骤 7.2 — 释放 mmap 映射

```cpp
// src/http_conn.cpp:532
unmap();
  └─ src/http_conn.cpp:502  munmap(m_file_address, m_file_stat.st_size);  // ★ 系统调用 munmap()
```

---

## 阶段八：连接保持或关闭

```
src/http_conn.cpp:534
if (m_linger) {              // HTTP/1.1 keep-alive
    init_parse_state();      // 重置状态机，等待下一个请求
    modfd(epoll_fd, sockfd, EPOLLIN);  // 重新注册读事件
    return true;
} else {
    return false;            // 返回 false → 主线程调用 close_conn()
}
```

- **关闭连接**：`HttpConn::close_conn()`（`src/http_conn.cpp`，第 123 行）
  ```
  removefd(m_epollfd, m_sockfd)
    └─ epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0)  // ★ 系统调用 epoll_ctl()
    └─ close(fd)                                   // ★ 系统调用 close()
  ```
- 同时从 `time_heap` 删除定时器（`timer_lst.del_timer(timer)`）。

---

## 系统调用速查表

| # | 系统调用 | 文件 | 行号 | 触发时机 |
|---|---------|------|------|---------|
| 1 | `socket()` | `src/server_epoll.cpp` | 61 | 服务器启动，创建监听 fd |
| 2 | `bind()` | `src/server_epoll.cpp` | 70 | 绑定 0.0.0.0:8080 |
| 3 | `listen()` | `src/server_epoll.cpp` | 71 | 开始接受连接，backlog=10000 |
| 4 | `epoll_create1()` | `src/server_epoll.cpp` | 73 | 创建 epoll 实例 |
| 5 | `epoll_ctl(ADD,server_fd)` | `src/http_conn.cpp` | 42 | 将 server_fd 注册到 epoll |
| 6 | `socketpair()` | `src/server_epoll.cpp` | 80 | 创建信号统一事件源管道 |
| 7 | `epoll_wait()` | `src/server_epoll.cpp` | 102 | 主线程阻塞等待事件（每轮循环） |
| 8 | `accept()` | `src/server_epoll.cpp` | 116 | 摘取新的 TCP 连接 |
| 9 | `epoll_ctl(ADD,connfd)` | `src/http_conn.cpp` | 42 | 将连接 fd 注册到 epoll（含 EPOLLONESHOT） |
| 10 | `recv()` | `src/http_conn.cpp` | 135 | 主线程读取 HTTP 请求数据（循环至 EAGAIN） |
| 11 | `mmap()` | `src/http_conn.cpp` | 494 | 工作线程映射静态文件，零拷贝 |
| 12 | `epoll_ctl(MOD,EPOLLOUT)` | `src/http_conn.cpp` | 55 | 工作线程通知主线程"响应已就绪可发送" |
| 13 | `writev()` | `src/http_conn.cpp` | 519 | 主线程分散写：一次发送头部 + 文件内容 |
| 14 | `munmap()` | `src/http_conn.cpp` | 502 | 发送完毕，解除文件内存映射 |
| 15 | `epoll_ctl(DEL)` / `close()` | `src/http_conn.cpp` | 47/48 | 连接关闭，注销 epoll 并释放 fd |
| 16 | `send()` (信号管道) | `src/server_epoll.cpp` | 28 | 信号处理函数向管道写信号值 |
| 17 | `recv()` (信号管道) | `src/server_epoll.cpp` | 142 | 主线程从管道读取信号值 |

---

## 线程交互流程图

```
时间轴 ──────────────────────────────────────────────────────────────────►

主线程（main）                              工作线程（ThreadPool worker × 4）
──────────────────────────────────────      ─────────────────────────────────
socket() bind() listen()                    [初始化后阻塞在 condition.wait()]
epoll_create1()
epoll_wait() ← 阻塞

客户端 SYN 到达 →
epoll_wait() 返回
accept()                                    (继续等待)
HttpConn::init() → epoll_ctl(ADD, EPOLLIN)
time_heap::add_timer()
epoll_wait() ← 再次阻塞

客户端 GET 请求到达 →
epoll_wait() 返回
HttpConn::read_once()                       (继续等待)
  └─ recv() [循环直到 EAGAIN]
pool.enqueue(lambda{users[fd].process()})
  └─ tasks.push() + condition.notify_one() ─────────────────────────────────►
epoll_wait() ← 再次阻塞                    工作线程唤醒
                                            HttpConn::process()
                                              └─ process_read()
                                                   └─ parse_request_line()
                                                   └─ parse_headers()
                                                   └─ do_request()
                                                        └─ mmap() [映射文件]
                                              └─ process_write()
                                                   └─ 构建响应头到 m_write_buf
                                                   └─ 设置 m_iv[0/1]
                                              └─ modfd(EPOLLOUT) → epoll_ctl()
                                            工作线程回到 condition.wait() ◄──

◄────────────────── epoll_wait() 返回（EPOLLOUT 就绪）
HttpConn::write()
  └─ writev() [发送响应头 + 文件]
  └─ munmap()
  └─ keep-alive: modfd(EPOLLIN) / 否则 close_conn()
epoll_wait() ← 循环
```

---

## 关键设计解析

### 1. 为何用 EPOLLONESHOT？
`connfd` 注册时加 `EPOLLONESHOT`，确保同一个 fd 在工作线程处理期间不会被主线程重复触发 EPOLLIN 事件，避免多个线程同时读同一连接。工作线程处理完毕后，必须通过 `modfd()` 重新 arm（`epoll_ctl(MOD)`），否则该连接将再无法触发事件。

### 2. 为何用 EPOLLET（边缘触发）？
ET 模式下，epoll 只在状态变化时通知一次（如：从无数据变为有数据）。因此 `read_once()` 必须在循环中调用 `recv()` 直到返回 `EAGAIN`，将内核缓冲区数据全部读完，否则后续数据将永远不再触发事件。

### 3. mmap + writev 零拷贝原理
- `mmap()` 将文件直接映射到用户进程虚拟地址，不经过 `read()` 的内核缓冲区 → 用户缓冲区拷贝。
- `writev()` 将响应头（`m_write_buf`）和文件内容（`m_file_address`）合并为一次系统调用发出，减少了上下文切换次数。

### 4. 主线程负责 I/O，工作线程负责计算
这是"半同步/半反应堆（half-sync/half-reactor）"模式的体现：
- **主线程**：纯 I/O 密集型（epoll、accept、recv、writev），不做任何业务计算。
- **工作线程**：纯计算密集型（HTTP 解析、状态机、数据库查询、响应构建），不做 socket 读写。
- **交接媒介**：`ThreadPool::enqueue()` + 共享 `users[]` 数组。
