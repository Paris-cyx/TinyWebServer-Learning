# My TinyWebServer (Learning Process)

## 📅 当前进度 (Phase 1: Reactor Core)
目前实现了一个高性能的 **Reactor 模型** 服务器。
* **核心架构**: Epoll (主线程) + ThreadPool (工作线程池)。
* **当前功能**: Echo Server (收到什么回什么，尚未实现 HTTP 解析)。
* **并发模型**: 模拟了 Proactor 模式，主线程负责 IO 读取，子线程负责业务逻辑。

## 🛠️ 如何运行
```bash
mkdir build && cd build
cmake ..
make
./server_core