#ifndef SQL_CONN_POOL_H
#define SQL_CONN_POOL_H

#include <mysql/mysql.h>
#include <string>
#include <list>
#include <mutex>
#include <semaphore.h>
#include <thread>

using namespace std;

class SqlConnPool {
public:
    // 单例模式：保证整个服务器只有一个连接池
    static SqlConnPool* Instance();

    // 初始化连接池
    void init(const char* host, int port,
              const char* user, const char* pwd, 
              const char* dbName, int connSize = 10);

    // 获取一个空闲连接
    MYSQL* GetConn();

    // 释放连接（归还到池中）
    void FreeConn(MYSQL* conn);

    // 获取当前空闲连接数
    int GetFreeConnCount();

    // 销毁连接池
    void ClosePool();

private:
    SqlConnPool();
    ~SqlConnPool();

    int m_MAX_CONN;  // 最大连接数
    int m_use_count; // 已使用连接数
    int m_free_count;// 空闲连接数

    list<MYSQL*> connList; // 连接池（链表存放）
    
    sem_t m_sem;    // 信号量：记录还有多少个空闲连接
    mutex m_mtx;    // 互斥锁：保护 connList 的线程安全
};

// RAII机制：自动获取和释放连接
// 就像智能指针一样，出了作用域自动归还连接，防止忘记 Release
class SqlConnRAII {
public:
    SqlConnRAII(MYSQL** sql, SqlConnPool* connpool);
    ~SqlConnRAII();
    
private:
    MYSQL* sqlRAII;
    SqlConnPool* poolRAII;
};

#endif