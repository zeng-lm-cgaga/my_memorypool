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

    // PerformanceTest 仅保留 warmup，各测试场景由 wrapper 函数接管
};

// 多次运行统计，输出 min / avg / max，减少单次测试的偶然误差
struct BenchResult {
    double memPoolTime;
    double systemTime;
};

template<typename Func>
BenchResult runBench(Func testFunc, int iterations = 5) {
    BenchResult best = {1e9, 1e9};
    BenchResult worst = {0, 0};
    double sumMem = 0, sumSys = 0;
    for (int i = 0; i < iterations; ++i) {
        auto res = testFunc();
        sumMem += res.memPoolTime;
        sumSys += res.systemTime;
        if (res.memPoolTime < best.memPoolTime) best = res;
        if (res.systemTime > worst.systemTime) worst = res;
    }
    return {sumMem / iterations, sumSys / iterations};
}

// 改造各测试函数，返回耗时数据供统计使用
BenchResult testSmallAllocationWrapper() {
    double memTime, sysTime;
    {
        constexpr size_t NUM_ALLOCS = 50000;
        const size_t SIZES[] = {8, 16, 32, 64, 128, 256};
        const size_t NUM_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);

        // 内存池
        {
            Timer t;
            std::array<std::vector<std::pair<void*, size_t>>, NUM_SIZES> sizePtrs;
            for(auto& ptrs : sizePtrs) ptrs.reserve(NUM_ALLOCS / NUM_SIZES);
            for(size_t i = 0; i < NUM_ALLOCS; ++i) {
                size_t sizeIndex = i % NUM_SIZES;
                size_t size = SIZES[sizeIndex];
                void* ptr = MemoryPool::allocate(size);
                sizePtrs[sizeIndex].push_back({ptr, size});
                if(i % 4 == 0) {
                    size_t releaseIndex = rand() % NUM_SIZES;
                    auto& ptrs = sizePtrs[releaseIndex];
                    if(!ptrs.empty()) {
                        MemoryPool::deallocate(ptrs.back().first, ptrs.back().second);
                        ptrs.pop_back();
                    }
                }
            }
            for(auto& ptrs : sizePtrs)
                for(const auto& [ptr, size] : ptrs)
                    MemoryPool::deallocate(ptr, size);
            memTime = t.elapsed();
        }

        // new/delete
        {
            Timer t;
            std::array<std::vector<std::pair<void*, size_t>>, NUM_SIZES> sizePtrs;
            for(auto& ptrs : sizePtrs) ptrs.reserve(NUM_ALLOCS / NUM_SIZES);
            for(size_t i = 0; i < NUM_ALLOCS; ++i) {
                size_t sizeIndex = i % NUM_SIZES;
                size_t size = SIZES[sizeIndex];
                void* ptr = new char[size];
                sizePtrs[sizeIndex].push_back({ptr, size});
                if(i % 4 == 0) {
                    size_t releaseIndex = rand() % NUM_SIZES;
                    auto& ptrs = sizePtrs[releaseIndex];
                    if(!ptrs.empty()) {
                        delete[] static_cast<char*>(ptrs.back().first);
                        ptrs.pop_back();
                    }
                }
            }
            for(auto& ptrs : sizePtrs)
                for(const auto& [ptr, size] : ptrs)
                    delete[] static_cast<char*>(ptr);
            sysTime = t.elapsed();
        }
    }
    return {memTime, sysTime};
}

BenchResult testMultiThreadedWrapper() {
    constexpr size_t NUM_THREADS = 4;
    constexpr size_t ALLOCS_PER_THREAD = 25000;

    auto threadFunc = [ALLOCS_PER_THREAD](bool useMemoPool) {
        std::random_device rd;
        std::mt19937 gen(rd());
        const size_t SIZES[] = {8, 16, 32, 64, 128, 256};
        const size_t NUM_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);
        std::array<std::vector<std::pair<void*, size_t>>, NUM_SIZES> sizePtrs;
        for(auto& ptrs : sizePtrs) ptrs.reserve(ALLOCS_PER_THREAD / NUM_SIZES);
        for(size_t i = 0; i < ALLOCS_PER_THREAD; ++i) {
            size_t sizeIndex = i % NUM_SIZES;
            size_t size = SIZES[sizeIndex];
            void* ptr = useMemoPool ? MemoryPool::allocate(size) : new char[size];
            sizePtrs[sizeIndex].push_back({ptr, size});
            if(i % 100 == 0) {
                size_t releaseIndex = rand() % NUM_SIZES;
                auto& ptrs = sizePtrs[releaseIndex];
                if(!ptrs.empty()) {
                    size_t releaseCount = ptrs.size() * (20 + (rand() % 11)) / 100;
                    releaseCount = std::min(releaseCount, ptrs.size());
                    for(size_t j = 0; j < releaseCount; ++j) {
                        size_t index = rand() % ptrs.size();
                        if(useMemoPool) MemoryPool::deallocate(ptrs[index].first, ptrs[index].second);
                        else delete[] static_cast<char*>(ptrs[index].first);
                        ptrs[index] = ptrs.back();
                        ptrs.pop_back();
                    }
                }
            }
            if(i % 1000 == 0) {
                std::vector<std::pair<void*, size_t>> pressurePtrs;
                for(int j = 0; j < 50; ++j) {
                    size_t size = SIZES[rand() % NUM_SIZES];
                    void* ptr = useMemoPool ? MemoryPool::allocate(size) : new char[size];
                    pressurePtrs.push_back({ptr, size});
                }
                for(const auto& [ptr, size] : pressurePtrs) {
                    if(useMemoPool) MemoryPool::deallocate(ptr, size);
                    else delete[] static_cast<char*>(ptr);
                }
            }
        }
        for(const auto& ptrs : sizePtrs)
            for(const auto& [ptr, size] : ptrs)
                if(useMemoPool) MemoryPool::deallocate(ptr, size);
                else delete[] static_cast<char*>(ptr);
    };

    double memTime, sysTime;
    {
        Timer t;
        std::vector<std::thread> threads;
        for(size_t i = 0; i < NUM_THREADS; ++i)
            threads.emplace_back(threadFunc, true);
        for(auto& th : threads) th.join();
        memTime = t.elapsed();
    }
    {
        Timer t;
        std::vector<std::thread> threads;
        for(size_t i = 0; i < NUM_THREADS; ++i)
            threads.emplace_back(threadFunc, false);
        for(auto& th : threads) th.join();
        sysTime = t.elapsed();
    }
    return {memTime, sysTime};
}

BenchResult testMixedSizesWrapper() {
    constexpr size_t NUM_ALLOCS = 100000;
    const size_t SMALL_SIZES[] = {8, 16, 32, 64, 128};
    const size_t MEDIUM_SIZES[] = {256, 384, 512};
    const size_t LARGE_SIZES[] = {1024, 2048, 4096};
    const size_t NUM_SMALL = sizeof(SMALL_SIZES) / sizeof(SMALL_SIZES[0]);
    const size_t NUM_MEDIUM = sizeof(MEDIUM_SIZES) / sizeof(MEDIUM_SIZES[0]);
    const size_t NUM_LARGE = sizeof(LARGE_SIZES) / sizeof(LARGE_SIZES[0]);
    const size_t NUM_BUCKETS = NUM_SMALL + NUM_MEDIUM + NUM_LARGE;

    double memTime, sysTime;
    {
        Timer t;
        std::array<std::vector<std::pair<void*, size_t>>, NUM_BUCKETS> sizePtrs;
        for(auto& ptrs : sizePtrs) ptrs.reserve(NUM_ALLOCS / NUM_BUCKETS);
        for(size_t i = 0; i < NUM_ALLOCS; ++i) {
            size_t size;
            int category = i % 100;
            if(category < 60) { size_t idx = (i / 60) % NUM_SMALL; size = SMALL_SIZES[idx]; }
            else if(category < 90) { size_t idx = (i / 30) % NUM_MEDIUM; size = MEDIUM_SIZES[idx]; }
            else { size_t idx = (i / 10) % NUM_LARGE; size = LARGE_SIZES[idx]; }
            void* ptr = MemoryPool::allocate(size);
            size_t ptrIndex = (category < 60) ? (i / 60) % NUM_SMALL :
                              (category < 90) ? NUM_SMALL + (i / 30) % NUM_MEDIUM :
                              NUM_SMALL + NUM_MEDIUM + (i / 10) % NUM_LARGE;
            sizePtrs[ptrIndex].push_back({ptr, size});
            if(i % 50 == 0) {
                size_t releaseIndex = rand() % sizePtrs.size();
                auto& ptrs = sizePtrs[releaseIndex];
                if(!ptrs.empty()) {
                    size_t releaseCount = ptrs.size() * (20 + (rand() % 11)) / 100;
                    releaseCount = std::min(releaseCount, ptrs.size());
                    for(size_t j = 0; j < releaseCount; ++j) {
                        size_t index = rand() % ptrs.size();
                        MemoryPool::deallocate(ptrs[index].first, ptrs[index].second);
                        ptrs[index] = ptrs.back();
                        ptrs.pop_back();
                    }
                }
            }
        }
        for(auto& ptrs : sizePtrs)
            for(const auto& [ptr, size] : ptrs)
                MemoryPool::deallocate(ptr, size);
        memTime = t.elapsed();
    }
    {
        Timer t;
        std::array<std::vector<std::pair<void*, size_t>>, NUM_BUCKETS> sizePtrs;
        for(auto& ptrs : sizePtrs) ptrs.reserve(NUM_ALLOCS / NUM_BUCKETS);
        for(size_t i = 0; i < NUM_ALLOCS; ++i) {
            size_t size;
            int category = i % 100;
            if(category < 60) { size_t idx = (i / 60) % NUM_SMALL; size = SMALL_SIZES[idx]; }
            else if(category < 90) { size_t idx = (i / 30) % NUM_MEDIUM; size = MEDIUM_SIZES[idx]; }
            else { size_t idx = (i / 10) % NUM_LARGE; size = LARGE_SIZES[idx]; }
            void* ptr = new char[size];
            size_t ptrIndex = (category < 60) ? (i / 60) % NUM_SMALL :
                              (category < 90) ? NUM_SMALL + (i / 30) % NUM_MEDIUM :
                              NUM_SMALL + NUM_MEDIUM + (i / 10) % NUM_LARGE;
            sizePtrs[ptrIndex].push_back({ptr, size});
            if(i % 50 == 0) {
                size_t releaseIndex = rand() % sizePtrs.size();
                auto& ptrs = sizePtrs[releaseIndex];
                if(!ptrs.empty()) {
                    size_t releaseCount = ptrs.size() * (20 + (rand() % 11)) / 100;
                    releaseCount = std::min(releaseCount, ptrs.size());
                    for(size_t j = 0; j < releaseCount; ++j) {
                        size_t index = rand() % ptrs.size();
                        delete[] static_cast<char*>(ptrs[index].first);
                        ptrs[index] = ptrs.back();
                        ptrs.pop_back();
                    }
                }
            }
        }
        for(auto& ptrs : sizePtrs)
            for(const auto& [ptr, size] : ptrs)
                delete[] static_cast<char*>(ptr);
        sysTime = t.elapsed();
    }
    return {memTime, sysTime};
}

int main()
{
    constexpr int ITERATIONS = 5;
    std::cout << "Starting performance tests (" << ITERATIONS << " iterations each)..." << std::endl;
    PerformanceTest::warmup();

    // 小对象测试
    {
        auto res = runBench(testSmallAllocationWrapper, ITERATIONS);
        double speedup = (res.systemTime / res.memPoolTime - 1.0) * 100;
        std::cout << "\n[Small Objects " << ITERATIONS << "-run avg]\n"
                  << "  MemoryPool: " << std::fixed << std::setprecision(2) << res.memPoolTime << " ms\n"
                  << "  New/Delete: " << res.systemTime << " ms\n"
                  << "  Speedup:    +" << std::setprecision(1) << speedup << "%" << std::endl;
    }

    // 多线程测试
    {
        auto res = runBench(testMultiThreadedWrapper, ITERATIONS);
        double speedup = (res.systemTime / res.memPoolTime - 1.0) * 100;
        std::cout << "\n[Multi-Threaded " << ITERATIONS << "-run avg]\n"
                  << "  MemoryPool: " << std::fixed << std::setprecision(2) << res.memPoolTime << " ms\n"
                  << "  New/Delete: " << res.systemTime << " ms\n"
                  << "  Speedup:    +" << std::setprecision(1) << speedup << "%" << std::endl;
    }

    // 混合大小测试
    {
        auto res = runBench(testMixedSizesWrapper, ITERATIONS);
        double speedup = (res.systemTime / res.memPoolTime - 1.0) * 100;
        std::cout << "\n[Mixed Sizes " << ITERATIONS << "-run avg]\n"
                  << "  MemoryPool: " << std::fixed << std::setprecision(2) << res.memPoolTime << " ms\n"
                  << "  New/Delete: " << res.systemTime << " ms\n"
                  << "  Speedup:    +" << std::setprecision(1) << speedup << "%" << std::endl;
    }

    return 0;
}
// git快给我显示啊