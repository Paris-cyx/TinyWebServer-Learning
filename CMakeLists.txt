cmake_minimum_required(VERSION 3.10)
project(TinyWebServer)

set(CMAKE_CXX_STANDARD 11)

# 自动查找系统线程库
find_package(Threads REQUIRED)

# ==========================================
# 学习阶段代码 (存放在 demos 文件夹中)
# ==========================================

# 1. 归档：单线程 Server (如果文件不存在，cmake 会跳过)
if(EXISTS "${CMAKE_SOURCE_DIR}/demos/01_single_reactor.cpp")
    add_executable(demo_single demos/01_single_reactor.cpp)
endif()

# 2. 归档：多线程 Server (我们刚才写的那个)
add_executable(demo_multithread demos/02_multithread_reactor.cpp)
target_link_libraries(demo_multithread Threads::Threads)

# 3. 工具：测试用的客户端
add_executable(test_client demos/client_test.cpp)
target_link_libraries(test_client Threads::Threads)

# ==========================================
# 核心代码 (src)
# ==========================================
# ==========================================
# 核心代码 (src)
# ==========================================

# 1. 归档：原来的 Epoll 单线程版 (放到 demos 文件夹后)
if(EXISTS "${CMAKE_SOURCE_DIR}/demos/03_epoll_single.cpp")
    add_executable(demo_epoll_single demos/03_epoll_single.cpp)
endif()

# 2. 核心：Epoll + ThreadPool (最终版)
add_executable(server_core src/server_epoll.cpp)
target_link_libraries(server_core Threads::Threads) # 这次必须链接线程库！