# TinyWebServer-Learning 设计文档

> 仓库：`Paris-cyx/TinyWebServer-Learning`（main 分支）
>
> 本文档**只包含与当前项目代码/README 已实现功能一致**的内容：
> - Reactor(Epoll) + 线程池
> - HTTP 解析与静态资源返回（`resources/`）
> - MySQL 用户注册/登录（`user` 表）
> - Cookie 登录态（`is_login=true`）
> - multipart/form-data 文件上传（保存到 `resources/upload_*`，不入库）
> - 异步日志系统
> - 最小堆定时器（超时踢连接）

---

## 1. 数据设计

### 1.1 E-R 图（数据库）

```mermaid
erDiagram
    %% 物理数据库实体
    USER {
        varchar username PK "用户名 (主键)"
        varchar passwd "密码"
    }

    %% 系统业务抽象实体
    HTTP_CONN {
        int sockfd PK "套接字描述符"
        varchar client_ip "客户端IP地址"
        int check_state "主状态机当前状态"
        int method "HTTP请求方法(GET/POST)"
        varchar read_buf "应用层读缓冲区"
        varchar write_buf "应用层写缓冲区"
    }

    THREAD_POOL {
        int pool_id PK "线程池实例ID"
        int thread_number "固定工作线程数量"
        int max_requests "请求队列最大允许长度"
    }

    SQL_CONN_POOL {
        int pool_id PK "数据库连接池ID"
        varchar url "数据库主机地址"
        varchar db_name "目标数据库名"
        int max_conn "最大连接数"
        int free_conn "当前空闲连接数"
    }

    TIMER {
        int timer_id PK "定时器节点ID"
        time expire "连接超时绝对时间"
        func cb_func "超时回调断开函数"
    }

    LOG {
        int log_id PK "日志实例ID"
        varchar file_name "日志文件基础名称"
        int split_lines "单文件最大行数拆分阈值"
        int log_buf_size "异步写缓冲区大小"
    }

    %% 实体间的业务流转关系
    THREAD_POOL ||--o{ HTTP_CONN : "提取并执行业务逻辑 (processes)"
    HTTP_CONN }o--|| SQL_CONN_POOL : "申请并使用数据库连接 (acquires)"
    HTTP_CONN ||--o| USER : "执行登录/注册鉴权 (validates)"
    HTTP_CONN ||--|| TIMER : "绑定生命周期超时控制 (binds)"
    HTTP_CONN }o--|| LOG : "触发并写入运行状态 (writes)"
    SQL_CONN_POOL ||--o{ USER : "底层直接执行增查操作 (queries)"
```
---

### 1.2 数据结构表（带说明）

#### 1.2.1 数据库表：`user`

> 来源：`README.md` 中的 Database Setup。

| 字段名 | 类型 | 允许为空 | 建议约束/索引 | 说明 | 代码/位置 |
|---|---|---:|---|---|---|
| username | char(50) | 是（README） | **建议：NOT NULL + UNIQUE（主键/唯一键）** | 用户名（业务主键） | `HttpConn::initmysql_result()`、注册 INSERT、登录校验 |
| passwd | char(50) | 是（README） | 建议：NOT NULL | 密码（当前明文，仅学习） | 同上 |


#### 1.2.2 关键内存数据结构（项目运行时数据）

| 名称 | 类型 | 位置 | 用途/说明 |
|---|---|---|---|
| `users` | `map<string,string>` | `src/http_conn.cpp`（全局） | 用户名->密码缓存；启动时从 DB 加载；注册成功后写入（带互斥锁） |
| `m_lock` | `mutex` | `src/http_conn.cpp`（全局） | 保护 `users` 的并发读写 |
| `HttpConn* users` | `HttpConn[MAX_FD]` | `src/server_epoll.cpp` | fd 作为索引保存每个连接的状态机/缓冲区 |
| `client_data* users_timer` | `client_data[MAX_FD]` | `src/server_epoll.cpp` | 每个连接的定时器上下文（sockfd/address/timer 指针） |
| `time_heap timer_lst` | 最小堆 | `src/lst_timer.h` | 连接超时管理：SIGALRM 驱动 `tick()`，回调踢连接 |
| `pipefd` | `int[2]` | `src/server_epoll.cpp` | socketpair：将信号统一为 epoll 可读事件 |

---

## 2. 功能设计

### 2.1 UML 用例图（Use Case）

```mermaid
flowchart LR
  U[客户端/浏览器] --> UC1((访问静态资源 GET))
  U --> UC2((注册 POST /3))
  U --> UC3((登录 POST /2))
  U --> UC4((访问受保护页面 /welcome.html /media.html))
  U --> UC5((文件上传 multipart/form-data))

  subgraph S[WebServer]
    UC1
    UC2
    UC3
    UC4
    UC5
  end
```

#### 说明
- `GET /` 会被补全为 `index.html` 并从 `resources/` 返回。
- 注册/登录由 `src/http_conn.cpp::do_request()` 处理：
  - `/3` 注册：INSERT
  - `/2` 登录：校验并 Set-Cookie
- 受保护页面：当访问 `/welcome.html` 或 `/media.html` 且 Cookie 不包含 `is_login=true` 时，会被重写到 `/logError.html`。

---

### 2.2 UML 类图（Class Diagram）

```mermaid
flowchart TB

ThreadPool["**ThreadPool**\n----------------------\n- workers\n- tasks\n- queue_mutex\n- condition\n- stop\n----------------------\n+ ThreadPool(threads)\n+ ~ThreadPool()\n+ enqueue(task)"]

HttpConn["**HttpConn**\n----------------------\n+ m_epollfd : static\n+ m_user_count : static\n----------------------\n+ init(sockfd, addr)\n+ close_conn()\n+ read_once() : bool\n+ write() : bool\n+ process()\n+ initmysql_result(connPool)\n----------------------\n- process_read()\n- process_write(ret)\n- parse_request_line(text)\n- parse_headers(text)\n- parse_content(text)\n- parse_multipart_content(text)\n- do_request()\n- add_response(...)\n- add_headers(content_length)"]

SqlConnPool["**SqlConnPool** <<singleton>>\n----------------------\n- connList\n- sem\n- mtx\n- MAX_CONN\n----------------------\n+ Instance()\n+ init(host, port, user, pwd, dbName, connSize)\n+ GetConn()\n+ FreeConn(conn)\n+ ClosePool()"]

SqlConnRAII["**SqlConnRAII**\n----------------------\n- sql\n- pool\n----------------------\n+ SqlConnRAII(sqlPtr, pool)\n+ ~SqlConnRAII()"]

Log["**Log** <<singleton>>\n----------------------\n+ Instance()\n+ init(file_name, close_log, ...)\n+ write_log(level, format, ...)\n+ flush()"]

client_data["**client_data** <<struct>>\n----------------------\n+ address\n+ sockfd\n+ timer"]

util_timer["**util_timer**\n----------------------\n+ expire\n+ cb_func\n+ user_data"]

time_heap["**time_heap**\n----------------------\n- array\n- capacity\n- cur_size\n----------------------\n+ add_timer(timer)\n+ del_timer(timer)\n+ adjust_timer(timer)\n+ tick()"]

SqlConnPool -->|provides| SqlConnRAII
HttpConn -->|uses| SqlConnPool
HttpConn -->|logs| Log
util_timer --> client_data
time_heap --> util_timer
```

---

### 2.3 功能结构图（模块架构图）

```mermaid
flowchart TB
  subgraph Reactor[Reactor 驱动层]
    EP[主线程 epoll_wait 事件循环\nserver_epoll.cpp]
    SIG[信号统一事件源\nSIGALRM/SIGINT/SIGTERM -> socketpair]
  end

  subgraph Concurrency[并发处理层]
    TP[线程池 ThreadPool\n把业务处理放到 worker]
  end

  subgraph Protocol[协议解析层]
    HC[HttpConn\nHTTP FSM解析 + 业务处理 + 响应生成]
  end

  subgraph Infra[基础设施层]
    DB[MySQL连接池 SqlConnPool + RAII]
    LOG[异步日志 Log + BlockQueue]
    TIMER[最小堆定时器 time_heap]
  end

  EP -->|accept 新连接| HC
  EP -->|EPOLLIN 就绪| TP
  TP -->|执行 HttpConn::process| HC
  HC --> DB
  HC --> LOG
  SIG --> EP
  EP --> TIMER
```

#### 说明
- **主线程**负责事件循环：accept、读写事件分发、信号处理、定时器 tick。
- **线程池**负责执行 `HttpConn::process()`（解析请求、业务、生成响应）。
- **HttpConn**使用 `mmap + writev` 实现静态文件发送；登录注册通过 MySQL 连接池访问数据库。
- **定时器**通过 `alarm(TIMESLOT)` 定时触发 SIGALRM，再通过 socketpair 写入管道，使 epoll 可感知并调用 `timer_lst.tick()` 踢出超时连接。

---

### 2.4 时序图（主要功能模块处理流程）

#### 2.4.1 静态资源 GET（含受保护页面拦截）

```mermaid
sequenceDiagram
  participant C as Client
  participant E as EpollLoop_main
  participant T as ThreadPool_worker
  participant H as HttpConn
  participant FS as FileSystem_resources

  C->>E: connect and send GET /index.html
  E->>H: accept connection and init HttpConn
  E->>T: enqueue task when EPOLLIN ready
  T->>H: process_read parse request
  H->>H: do_request auth check for protected pages
  H->>FS: stat open mmap
  H->>H: process_write for FILE_REQUEST
  H->>E: modfd set EPOLLOUT
  E->>H: EPOLLOUT event
  H->>C: writev send response
```

#### 2.4.2 注册/登录（POST /3、POST /2，JSON + Cookie）

```mermaid
sequenceDiagram
  participant C as Client
  participant E as EpollLoop(main)
  participant T as ThreadPool(worker)
  participant H as HttpConn
  participant P as SqlConnPool(MySQL)

  C->>E: POST /3(register) 或 /2(login)
  E->>T: EPOLLIN -> enqueue(HttpConn::process)
  T->>H: parse_content() 得到 m_string
  H->>H: do_request() 识别 /2 /3 => m_is_json=true

  alt 注册 /3
    H->>P: RAII 获取 MYSQL* 并执行 INSERT
    H->>H: 生成 m_json_string(code=200/400/500)
  else 登录 /2
    H->>H: 校验 users[name]==password
    H->>H: 登录成功 => m_set_cookie=1
    H->>H: 生成 m_json_string(code=200/401)
  end

  H->>H: process_write(GET_REQUEST) 组装 JSON
  H->>H: add_headers() 若m_set_cookie=1则先写 Set-Cookie
  H->>E: modfd(EPOLLOUT)
  E->>H: EPOLLOUT
  H->>C: writev/send 响应
```

#### 2.4.3 文件上传（multipart/form-data，保存到 resources/upload_*）

```mermaid
sequenceDiagram
  participant C as Client
  participant T as ThreadPool_worker
  participant H as HttpConn
  participant FS as FileSystem

  C->>T: send POST multipart form data
  T->>H: parse_headers detect multipart and boundary
  T->>H: parse_content then parse_multipart_content
  H->>H: extract filename and locate binary range
  H->>FS: write file into resources directory
  H->>H: set url to /welcome.html
```

---

## 3. 主要功能流程图

> 该流程图聚合 `server_epoll.cpp` 的主循环和 `HttpConn::process()` 的核心处理逻辑。

```mermaid
flowchart TD
  S[Server start] --> L[init Log]
  L --> DB[init SqlConnPool]
  DB --> SOCK[create listen socket]
  SOCK --> EP[create epoll and add listen fd]
  EP --> PIPE[socketpair + add pipe read fd]
  PIPE --> SIG[register signals + alarm TIMESLOT]
  SIG --> POOL[create ThreadPool]
  POOL --> LOAD[load users from DB]
  LOAD --> LOOP{epoll_wait}

  LOOP -->|listen fd| ACC[accept new connection]
  ACC --> INIT[HttpConn.init and add timer]
  INIT --> LOOP

  LOOP -->|pipe fd readable| SIGEV[handle SIGALRM or SIGINT]
  SIGEV -->|SIGALRM| TICK[timer_heap.tick]
  TICK --> LOOP
  SIGEV -->|SIGINT/SIGTERM| STOP[stop_server true]

  LOOP -->|EPOLLIN| READ[read_once]
  READ -->|ok| ENQ[adjust timer and enqueue HttpConn.process]
  READ -->|fail| CLOSE1[del timer and close conn]
  ENQ --> LOOP
  CLOSE1 --> LOOP

  LOOP -->|EPOLLOUT| WRITE[HttpConn.write]
  WRITE -->|ok| ADJ[adjust timer]
  WRITE -->|fail| CLOSE2[del timer and close conn]
  ADJ --> LOOP
  CLOSE2 --> LOOP

  STOP --> CLEAN[cleanup fds and delete arrays]
  CLEAN --> END[Server exit]
```

---

## 4. 测试用例（与当前项目实现一致）

> 推荐工具：浏览器 + `curl` + `webbench`。

### 4.1 静态资源

| 用例ID | 场景 | 请求 | 期望结果 |
|---|---|---|---|
| TC-GET-01 | 访问首页 | `GET /` | 返回 `resources/index.html` |
| TC-GET-02 | 访问页面 | `GET /welcome.html`（无Cookie） | 被重写为 `/logError.html` 并返回对应页面 |
| TC-GET-03 | 访问页面 | `GET /welcome.html`（Cookie含`is_login=true`） | 正常返回 welcome 页面 |
| TC-GET-04 | 资源不存在 | `GET /no_such_file.html` | 返回 404（NO_RESOURCE） |
| TC-GET-05 | 大文件 | `GET /video.mp4` | 可正常播放/下载（mmap + writev） |

### 4.2 注册/登录（JSON）

| 用例ID | 场景 | 请求 | 期望结果 |
|---|---|---|---|
| TC-AUTH-01 | 注册成功 | `POST /3` body: `user=abc&passwd=123` | JSON：code=200，DB 插入成功 |
| TC-AUTH-02 | 重复��册 | 再次 `POST /3` 同用户名 | JSON：code=400（User Exist） |
| TC-AUTH-03 | 登录成功 | `POST /2` 正确账号 | JSON：code=200；响应头含 `Set-Cookie: is_login=true` |
| TC-AUTH-04 | 登录失败 | `POST /2` 密码错误 | JSON：code=401 |

### 4.3 文件上传（multipart）

| 用例ID | 场景 | 请求 | 期望结果 |
|---|---|---|---|
| TC-UP-01 | 上传图片 | multipart/form-data 上传 `a.jpg` | 生成 `resources/upload_a.jpg`（或同名前缀） |
| TC-UP-02 | boundary 缺失 | multipart 但无 boundary/filename | 解析失败，返回 BAD_REQUEST/500（取决于失败点） |

### 4.4 并发与稳定性

| 用例ID | 场景 | 操作 | 期望结果 |
|---|---|---|---|
| TC-CON-01 | 并发 GET | webbench 高并发压测静态页 | 无崩溃；日志无大量错误 |
| TC-TIMER-01 | 连接超时 | 建立连接后不发数据等待 >15s | 定时器回调踢连接，日志出现 Kick Client (Timeout) |

---

## 附：与代码文件的对应关系（索引）

- 主流程：`src/server_epoll.cpp`
- HTTP 解析/业务/响应：`src/http_conn.h`、`src/http_conn.cpp`
- DB 连接池：`src/sql_conn_pool.h`、`src/sql_conn_pool.cpp`
- 线程池：`src/ThreadPool.h`
- 日志：`src/log.h`、`src/log.cpp`（依赖 `src/block_queue.h`）
- 定时器：`src/lst_timer.h`
