#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <time.h>
#include "log.h" // 这一步要引入日志，方便看谁被踢了

class util_timer; // 前向声明

// 用户数据结构：把连接socket和定时器绑在一起
struct client_data
{
    sockaddr_in address; // 客户端地址
    int sockfd;          // socket文件描述符
    util_timer *timer;   // 指向该连接对应的定时器
};

// 定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire; // 任务的超时时间（绝对时间，比如 2026-01-29 12:00:00）
    
    // 回调函数指针：任务超时后，我们要执行哪个函数？(通常是关闭连接)
    void (*cb_func)(client_data *); 
    
    // 回调函数的参数：处理哪个用户？
    client_data *user_data;
    
    // 链表指针
    util_timer *prev;
    util_timer *next;
};

// 升序定时器链表：核心管理类
class sort_timer_lst
{
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    
    // 链表销毁时，释放所有定时器内存
    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    // 添加定时器：内部会保证链表是升序的
    void add_timer(util_timer *timer)
    {
        if (!timer) return;
        if (!head)
        {
            head = tail = timer;
            return;
        }
        
        // 如果新定时器的超时时间比头节点还小，插到头部
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        
        // 否则调用私有函数，找到合适位置插入
        add_timer(timer, head);
    }

    // 调整定时器：当某个连接有数据活动时，它的过期时间会延长，需要往后挪
    void adjust_timer(util_timer *timer)
    {
        if (!timer) return;
        util_timer *tmp = timer->next;
        
        // 如果是被挪到最后，或者挪动后时间依然小于下一个节点，那位置不用变
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        
        // 把定时器取出来
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    // 删除定时器：当用户主动关闭连接时，需要把定时器删掉
    void del_timer(util_timer *timer)
    {
        if (!timer) return;
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    // 【核心功能】心跳函数：每隔几秒钟执行一次，检测有没有超时的
    void tick()
    {
        if (!head) return;
        
        // 获取当前系统时间
        time_t cur = time(NULL);
        util_timer *tmp = head;
        
        // 从头开始遍历，直到遇到一个还没过期的定时器
        while (tmp)
        {
            // 如果当前时间 < 任务的超时时间，说明后面的更没过期，直接退出
            if (cur < tmp->expire)
            {
                break;
            }
            
            // 否则，说明这个任务过期了！执行回调函数（关闭连接）
            tmp->cb_func(tmp->user_data);
            
            // 从链表中移除并销毁
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    // 私有辅助函数：找到合适位置插入
    void add_timer(util_timer *timer, util_timer *lst_head)
    {
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;
        
        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        
        // 如果遍历完了都没找到比它大的，说明它是最大的，插到尾部
        if (!tmp)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

    util_timer *head; // 头节点
    util_timer *tail; // 尾节点
};

#endif