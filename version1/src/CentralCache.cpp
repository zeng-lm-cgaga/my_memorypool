#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>

namespace my_memorypool
{

const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};

// 每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;

CentralCache::CentralCache()
{
    for(auto& ptr : centralFreeList_)
    {
        ptr.store(nullptr,std::memory_order_relaxed);
    }
    for(auto& lock : locks_)
    {
        lock.clear();
    }
    // 初始化延迟归还相关的成员变量
    for(auto& count : delayCounts_)
    {
        count.store(0, std::memory_order_relaxed);
    }
    for(auto& time : lastReturnTimes_)
    {
        time = std::chrono::steady_clock::now();
    }
    spanCount_.store(0, std::memory_order_relaxed);
}

void* CentralCache::fetchRange(size_t index)
{
    // 索引检查， 当索引大于等于FREE_LIST_SIZE时，说明申请内存过大直接向系统申请
    if(index >= FREE_LIST_SIZE )
        return nullptr;

    // 自旋锁保护
    while(locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield(); // 添加线程让步，避免忙等待，避免过度地消耗CPU
    }

    void* result = nullptr;
    try
    {
        // 尝试从中心缓存获取内存块
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if(!result)
        {
            // 如果中心缓存为空，从页缓存获取新的内存块
            size_t size = (index + 1) * ALIGNMENT;
            result = fetchFromPageCache(size);

            if(!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            // 将获取到的内存块切分成小块
            char* start = static_cast<char*>(result);

            // 计算实际分配页数（size最大为（FREE_LIST_SIZE(32k) * ALIGNMENT(8) == MAX_SIZE(256K))）
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ?
                                SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            //使用实际页数计算块数
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            if(blockNum >  1)
            { // 确保至少有两块后再构建链表
                for(size_t i = 1 ; i < blockNum ; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                } 
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                // 保存result的下一个节点
                void* next = *reinterpret_cast<void**>(result);
                // 将result与链表断开
                *reinterpret_cast<void**>(result) = nullptr;         
                // 更新中心缓存
                centralFreeList_[index].store(
                    next,
                    std::memory_order_release
                );

                // 使用无锁方式记录span信息
                // 目的是为了将中心缓存多余内存块归还给页缓存做准备：
                // 1.CentralCache管理的是小块内存，这些内存可能不连续
                // 2.PageCache 的 deallocateSpan 要求归还连续的内存
                size_t trackerIndex = spanCount_++;
                if(trackerIndex < spanTrackers_.size())
                {
                    spanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                    spanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                    spanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
                    spanTrackers_[trackerIndex].freeCount.store(blockNum-1, std::memory_order_release);
                }
            }
        }
        else
        {
            // 保存result的下一个节点
            void* next = *reinterpret_cast<void**>(result);
            // 将result与链表断开
            *reinterpret_cast<void**>(result) = nullptr;

            // 更新中心缓存
            centralFreeList_[index].store(next,std::memory_order_release);

            // 更新span的空闲计数
            SpanTracker* tracker = getSpanTracker(result);
            if(tracker)
            {
                // 减少一个空闲块
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
            }
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    // 释放锁
    locks_[index].clear(std::memory_order_release);
    return result;
} 

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if(!start || index >= FREE_LIST_SIZE)
        return;

        size_t blockSize = (index + 1) * ALIGNMENT;
        size_t blockCount = size / blockSize;

        while(locks_[index].test_and_set(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        try
        {
            // 1. 将归还的链表连接到中心缓存
            void* end = start;
            size_t count = 1;
            while(*reinterpret_cast<void**>(end) != nullptr && count < blockCount) {
                end = *reinterpret_cast<void**>(end);
                count++;
            }
            void* current = centralFreeList_[index].load(std::memory_order_relaxed);
            *reinterpret_cast<void**>(end) = current; 头插法（将原有链表接在归还链表后边）
            centralFreeList_[index].store(start, std::memory_order_release);

            // 2. 更新延迟计数
            size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
            auto currentTime =std::chrono::steady_clock::now();

            // 3. 检查是否需要执行延迟归还
            if(shouldPerformDelayedReturn(index, currentCount, currentTime))
            {
                performDelayReturn(index);
            }
        }
        catch (...)
        {
            locks_[index].clear(std::memory_order_release);
            throw;
        }

        locks_[index].clear(std::memory_order_release);
}

void CentralCache::performDelayReturn(size_t index)
{
    // 重置延迟计数
    delayCounts_[index].store(0,std::memory_order_relaxed);
    // 更新最后归还时间
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    // 统计每个span的空闲块数
    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);

    while(currentBlock)
    {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if(tracker)
        {
            spanFreeCounts[tracker]++;
        }
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    for(const auto& [tracker, newFreeBlocks] : spanFreeCounts)
    {
        updateSpanFreeCount(tracker,newFreeBlocks,index);
    }

}
}