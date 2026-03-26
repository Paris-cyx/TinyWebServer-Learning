#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

// ============================================================
// ThreadPool：半同步/半异步线程池
//
// 调用链路中的位置（详见 docs/reactor-call-chain.md §四）：
//   主线程（Reactor）  →  pool.enqueue(task)
//                              ↓ 加锁，推入 tasks 队列
//                          condition.notify_one()
//                              ↓ 唤醒一个工作线程
//   工作线程（Worker）  →  task()  即  HttpConn::process()
//
// 设计要点：
//   · 主线程只做 IO 感知与任务派发，不阻塞在业务逻辑上。
//   · 工作线程只做 HTTP 解析、文件读取、响应构建，不直接操作 epoll。
//   · 两者通过带互斥锁的任务队列 + 条件变量解耦，实现高并发下的职责分离。
// ============================================================
class ThreadPool {
public:
    // 构造函数：创建 `threads` 个工作线程，每个线程循环等待并执行任务
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] {
                while(true) {
                    std::function<void()> task;
                    {
                        // 加锁后等待条件：有任务 or 线程池停止
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return; // 优雅退出
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    } // 锁在此释放，减少持锁时间
                    task(); // 执行任务（即 HttpConn::process()）
                }
            });
    }

    // 析构：通知所有线程停止，等待它们完成当前任务后退出
    ~ThreadPool() {
        { std::unique_lock<std::mutex> lock(queue_mutex); stop = true; }
        condition.notify_all();
        for(std::thread &worker: workers) worker.join();
    }

    // enqueue：主线程（Reactor）调用此函数将任务投入队列
    // 调用链：read_once() 成功 → pool.enqueue(lambda) → condition.notify_one() → 工作线程唤醒
    template<class F>
    void enqueue(F&& f) {
        { std::unique_lock<std::mutex> lock(queue_mutex); tasks.emplace(std::forward<F>(f)); }
        condition.notify_one(); // 唤醒一个阻塞中的工作线程
    }

private:
    std::vector<std::thread> workers;          // 工作线程组
    std::queue<std::function<void()>> tasks;   // 任务队列（FIFO）
    std::mutex queue_mutex;                    // 保护 tasks 队列的互斥锁
    std::condition_variable condition;         // 任务到来/停止时通知工作线程
    bool stop;                                 // 线程池停止标志
};
#endif