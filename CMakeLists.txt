cmake_minimum_required(VERSION 3.0)
project(TinyWebServer)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 14)

# 开启调试模式
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2 -Wall -pthread")

# 包含头文件路径
include_directories(${PROJECT_SOURCE_DIR}/src)

# 【重点修改这里】添加 src/log.cpp
add_executable(server_core 
    src/server_epoll.cpp 
    src/http_conn.cpp 
    src/sql_conn_pool.cpp
    src/log.cpp
)

# 链接 MySQL 库
target_link_libraries(server_core mysqlclient)

# 下面的 demo 测试可以保留
add_executable(demo_single demos/01_single_reactor.cpp)
add_executable(demo_multithread demos/02_multithread_reactor.cpp)
add_executable(demo_epoll_single demos/03_epoll_single.cpp src/http_conn.cpp src/sql_conn_pool.cpp src/log.cpp)
target_link_libraries(demo_epoll_single mysqlclient)

add_executable(test_client demos/client_test.cpp)