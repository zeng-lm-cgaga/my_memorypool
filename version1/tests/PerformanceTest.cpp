#include "../include/MemoryPool.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <iomanip>
#include <thread>
#include <array>

using namespace my_memorypool;
using namespace std::chrono;

//计时器类
class Timer
{
    high_resolution_clock::time_point start;
public:
    Timer() : start(high_resolution_clock::now()) {}

    double elapsed()
    {
        auto end = high_resolution_clock::now();
        return duration_cast<microseconds>(end - start).count() / 1000.0; // 转换为毫秒

    }
};

// 性能测试类
class PerformanceTest
{
private:
    // 测试统计信息
    struct TestStats
    {
        double memPoolTime{0.0};
        double systemTime{0.0};
        size_t totalAllocs{0};
        size_t totalBytes{0};
    };

public:
    // 1. 系统预热
    static void warmup()
    {
        std::cout << "Warming up memory systems...\n";
        // 使用 pair 来存储指针和对应的大小
        std::vector<std::pair<void*, size_t>> warmupPtrs;

        // 预热内存池
        for(int i = 0; i < 1000; ++i)
        {
            for(size_t size : {8, 16, 32, 64, 128, 256, 512, 1024}) {
                void* p = MemoryPool::allocate(size);
                warmupPtrs.emplace_back(p, size); // 存储指针和对应大小
            }
        }

        // 释放预热内存
        for(const auto& [ptr, size] : warmupPtrs)
        {
            MemoryPool::deallocate(ptr, size); // 使用实际分配的大小进行释放
        }

        std::cout << "Warmup complete. \n\n";
    }

    // 2. 小对象分配测试
    static void testSmallAllocation()
    {
        constexpr size_t NUM_ALLOCS = 50000;
        // 使用固定的几个小对象大小，这些大小都是内存池最优化的大小
        const size_t SIZE[] = {8, 16, 32, 64, 128, 256};

    }
}