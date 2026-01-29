cmake_minimum_required(VERSION 3.10)
project(pool_qt_demo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 强制开启 O3 优化
add_compile_options(-O3 -Wall)

# 尝试查找 Qt6，如果找不到则查找 Qt5
find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets REQUIRED)

# 设置包含目录
include_directories(${CMAKE_SOURCE_DIR}/../include)

# 源文件
set(POOL_SOURCES
    ${CMAKE_SOURCE_DIR}/../src/CentralCache.cpp
    ${CMAKE_SOURCE_DIR}/../src/PageCache.cpp
    ${CMAKE_SOURCE_DIR}/../src/ThreadCache.cpp
)

set(QT_SOURCES
    main.cpp
    MainWindow.cpp
)

# 启用 MOC, UIC, RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_executable(pool_qt_demo
    ${QT_SOURCES}
    ${POOL_SOURCES}
)

# 链接 Qt 和 线程库 (自动适配版本)
target_link_libraries(pool_qt_demo PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Threads::Threads)
