# 🚀 TinyWebServer | Linux 高性能 C++ Web 服务器

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)
![Platform](https://img.shields.io/badge/platform-Linux-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![C++](https://img.shields.io/badge/language-C++11-orange)

> 这是一个基于 Linux Epoll 和 Reactor 模型的高性能 Web 服务器。
>
> 项目旨在深入理解 Linux 网络编程核心技术，实现了 **6000+ 并发** 下的稳定运行。

## ✨ 核心特性 (Key Features)

- **高并发模型**：基于 **Epoll (ET 边缘触发)** + **Reactor** 事件处理模型，非阻塞 I/O，有效应对高并发场景。
- **高效线程池**：实现了半同步/半反应堆 (Half-Sync/Half-Reactor) 线程池，避免频繁创建/销毁线程的开销，榨干 CPU 性能。
- **数据库连接池**：基于 **RAII 机制** 封装的 MySQL 连接池，复用数据库连接，大幅减少 SQL 握手耗时。
- **定时器系统**：**【亮点】** 实现了基于 **升序链表** 的定时器容器，配合 `socketpair` 和 `sigaction` 实现 **统一事件源**，能精准剔除非活动连接（默认 15s），防御恶意空闲连接攻击。
- **异步日志**：采用 **生产者-消费者模型** 实现的异步日志系统（阻塞队列），将磁盘 I/O 从主工作线程中剥离，保证服务器的高响应速度。
- **HTTP 解析**：使用 **有限状态机 (FSM)** 高效解析 HTTP 请求报文，支持 GET/POST 请求。

## 📊 压力测试 (Performance)

在虚拟机环境下（限制资源），使用 WebBench 进行压力测试：

- **并发数**：6000
- **测试时间**：30秒
- **QPS**：10,000+
- **结果**：**0 失败 (Zero Failure)**

![Pressure Test Result](./assets/stress_test.png)
*(注：请将 WebBench 截图命名为 stress_test.png 并放在 assets 目录下，或此处直接贴图)*

## 🛠️ 环境要求 (Environment)

- **OS**: Linux (Ubuntu/CentOS)
- **Compiler**: g++ 4.8+ (支持 C++11)
- **Build Tool**: Make / CMake
- **Database**: MySQL

## 🚀 快速开始 (Quick Start)

### 1. 编译项目
```bash
mkdir build
cd build
make