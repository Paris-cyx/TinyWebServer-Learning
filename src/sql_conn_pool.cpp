#include "sql_conn_pool.h"
#include <iostream>

using namespace std;

SqlConnPool::SqlConnPool() {
    m_use_count = 0;
    m_free_count = 0;
}

SqlConnPool* SqlConnPool::Instance() {
    static SqlConnPool connPool;
    return &connPool;
}

void SqlConnPool::init(const char* host, int port,
                       const char* user, const char* pwd, 
                       const char* dbName, int connSize) {
    m_MAX_CONN = connSize;
    m_free_count = 0;

    for(int i = 0; i < connSize; ++i) {
        MYSQL* con = nullptr;
        con = mysql_init(con);

        if(!con) {
            cout << "MySQL Error: mysql_init failed" << endl;
            continue;
        }

        con = mysql_real_connect(con, host, user, pwd, dbName, port, nullptr, 0);

        if(!con) {
            cout << "MySQL Error: " << mysql_error(con) << endl;
            continue;
        }
        
        connList.push_back(con);
        ++m_free_count;
    }

    // 初始化信号量，初始值为连接总数
    sem_init(&m_sem, 0, m_free_count);
    m_MAX_CONN = m_free_count; // 实际成功的连接数
}

MYSQL* SqlConnPool::GetConn() {
    MYSQL* sql = nullptr;

    // 等待信号量（如果没有空闲连接，这里会阻塞等待，直到有人归还）
    sem_wait(&m_sem);
    
    // 上锁操作链表
    {
        lock_guard<mutex> locker(m_mtx);
        sql = connList.front();
        connList.pop_front();
    } // 出作用域自动解锁

    return sql;
}

void SqlConnPool::FreeConn(MYSQL* conn) {
    if(!conn) return;

    {
        lock_guard<mutex> locker(m_mtx);
        connList.push_back(conn);
    }
    
    // 信号量 +1，通知其他等待的线程有连接可用了
    sem_post(&m_sem);
}

void SqlConnPool::ClosePool() {
    lock_guard<mutex> locker(m_mtx);
    for(auto item : connList) {
        mysql_close(item);
    }
    connList.clear();
}

int SqlConnPool::GetFreeConnCount() {
    lock_guard<mutex> locker(m_mtx);
    return connList.size();
}

SqlConnPool::~SqlConnPool() {
    ClosePool();
}

// ================= RAII 实现 =================
SqlConnRAII::SqlConnRAII(MYSQL** sql, SqlConnPool* connpool) {
    *sql = connpool->GetConn();
    sqlRAII = *sql;
    poolRAII = connpool;
}

SqlConnRAII::~SqlConnRAII() {
    if(sqlRAII) {
        poolRAII->FreeConn(sqlRAII);
    }
}