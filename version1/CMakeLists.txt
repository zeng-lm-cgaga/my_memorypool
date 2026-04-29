cmake_minimum_required(VERSION 3.10)
project(my_memorypool)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Span 追踪选项：开启后启用延迟回收机制（默认 ON），-DENABLE_SPAN_TRACKING=OFF 可关闭
option(ENABLE_SPAN_TRACKING "Enable span-level tracking for delayed page return" ON)
if(ENABLE_SPAN_TRACKING)
    add_compile_definitions(ENABLE_SPAN_TRACKING=1)
else()
    add_compile_definitions(ENABLE_SPAN_TRACKING=0)
endif()

# 编译选项（Release 默认开启优化；Debug 保留运行时检查）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall)
if(MSVC)
    add_compile_options(/W4 /O2)
else()
    add_compile_options(-O2)
endif()

# 查找pthread库
find_package(Threads REQUIRED)

# 设置目录
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_SOURCE_DIR}/include)
set(TEST_DIR ${CMAKE_SOURCE_DIR}/tests)

# 指明源文件
file(GLOB SOURCES "${SRC_DIR}/*.cpp")

# 添加头文件目录
include_directories(${INC_DIR})

# 创建单元测试可执行文件
add_executable(unit_test 
    ${SOURCES}
    ${TEST_DIR}/UnitTest.cpp
)

# 创建性能测试可执行文件
add_executable(perf_test
    ${SOURCES}
    ${TEST_DIR}/PerformanceTest.cpp
)

# 链接pthread库
target_link_libraries(unit_test PRIVATE Threads::Threads)
target_link_libraries(perf_test PRIVATE Threads::Threads)

# 添加测试命令
add_custom_target(test
    COMMAND ./unit_test
    DEPENDS unit_test
)

add_custom_target(perf
    COMMAND ./perf_test
    DEPENDS perf_test
)