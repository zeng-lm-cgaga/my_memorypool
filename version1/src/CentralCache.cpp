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
    for(auto& flag : returnBusy_)
    {
        flag.clear();
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

    // 先尝试使用CAS无锁弹出单个块（保持中心链表短小，避免大链返回给线程导致长遍历）
    size_t casAttempts = 0;
    while(true)
    {
        void* head = centralFreeList_[index].load(std::memory_order_acquire);
        if(!head) break; // 进入补给路径
        void* next = *reinterpret_cast<void**>(head);
        if(centralFreeList_[index].compare_exchange_weak(
                head,
                next,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            *reinterpret_cast<void**>(head) = nullptr;
            // 更新span的空闲计数
            SpanTracker* tracker = getSpanTracker(head);
            if(tracker)
            {
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
            }
            return head;
        }
        // 竞争失败重试
        std::this_thread::yield();
        if(++casAttempts > 1000000)
        {
            // 防御性回退：加锁弹出一个节点，顺便校验链表无环
            while(locks_[index].test_and_set(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            void* lockedHead = centralFreeList_[index].load(std::memory_order_relaxed);
            if(!lockedHead)
            {
                locks_[index].clear(std::memory_order_release);
                break;
            }
            void* lockedNext = *reinterpret_cast<void**>(lockedHead);
            centralFreeList_[index].store(lockedNext, std::memory_order_release);
            *reinterpret_cast<void**>(lockedHead) = nullptr;
            SpanTracker* tracker = getSpanTracker(lockedHead);
            if(tracker)
            {
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
            }
            locks_[index].clear(std::memory_order_release);
            return lockedHead;
        }
    }

    // 中心缓存为空：向PageCache补给（不持锁，以减少锁争用）
    size_t size = (index + 1) * ALIGNMENT;
    void* spanStart = fetchFromPageCache(size);
    if(!spanStart) return nullptr;

    // 构建链表并切分成小块
    char* start = static_cast<char*>(spanStart);
    size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ?
                        SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
    size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

    if(blockNum == 0)
    {
        return nullptr;
    }

    void* last = nullptr;
    for(size_t i = 1 ; i < blockNum ; ++i)
    {
        void* current = start + (i - 1) * size;
        void* next = start + i * size;
        *reinterpret_cast<void**>(current) = next;
    }
    last = start + (blockNum - 1) * size;
    *reinterpret_cast<void**>(last) = nullptr;

    // 取一块返回，其余推回中心链表
    void* result = start;
    void* remainStart = *reinterpret_cast<void**>(result);
    *reinterpret_cast<void**>(result) = nullptr;

    if(remainStart)
    {
        void* remainEnd = last;
        while(true)
        {
            void* head = centralFreeList_[index].load(std::memory_order_acquire);
            *reinterpret_cast<void**>(remainEnd) = head;
            if(centralFreeList_[index].compare_exchange_weak(
                    head,
                    remainStart,
                    std::memory_order_release,
                    std::memory_order_acquire))
            {
                break;
            }
            std::this_thread::yield();
        }
    }

    // 记录span信息供延迟归还判定
    size_t trackerIndex = spanCount_.fetch_add(1, std::memory_order_relaxed);
    if(trackerIndex < spanTrackers_.size())
    {
        spanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
        spanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
        spanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
        // 剩余块数（central里已有 blockNum-1 块）
        spanTrackers_[trackerIndex].freeCount.store((blockNum > 0) ? (blockNum - 1) : 0, std::memory_order_release);
    }
    else
    {
        spanCount_.store(spanTrackers_.size(), std::memory_order_relaxed);
    }

    return result;
} 

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if(!start || index >= FREE_LIST_SIZE)
    {
        return;
    }

        size_t blockSize = (index + 1) * ALIGNMENT;
        size_t blockCount = size / blockSize;

        // 1) 无锁将归还链表推入中心缓存（range push with CAS），并强制链尾截断避免循环
        void* end = start;
        size_t count = 1;
        while(*reinterpret_cast<void**>(end) != nullptr && count < blockCount) {
            end = *reinterpret_cast<void**>(end);
            count++;
        }
        // 强制截断来链，确保是以nullptr结束
        *reinterpret_cast<void**>(end) = nullptr;

        size_t casAttempts = 0;
        while(true)
        {
            void* head = centralFreeList_[index].load(std::memory_order_acquire);
            *reinterpret_cast<void**>(end) = head;
            if(centralFreeList_[index].compare_exchange_weak(
                    head,
                    start,
                    std::memory_order_release,
                    std::memory_order_acquire))
            {
                break;
            }
            std::this_thread::yield();
            if(++casAttempts > 1000000)
            {
                // 防御性回退：持锁推入，避免CAS饥饿
                while(locks_[index].test_and_set(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                void* headLocked = centralFreeList_[index].load(std::memory_order_relaxed);
                *reinterpret_cast<void**>(end) = headLocked;
                centralFreeList_[index].store(start, std::memory_order_release);
                locks_[index].clear(std::memory_order_release);
                break;
            }
        }

        // 2) 更新延迟计数与时间戳（不持锁）
        size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
        auto currentTime = std::chrono::steady_clock::now();

        // 3) 如触发延迟归还，仅在该阶段短暂持锁以进行遍历与移除；并限制同一大小类仅一个线程执行
        if(shouldPerformDelayedReturn(index, currentCount, currentTime))
        {
            if(!returnBusy_[index].test_and_set(std::memory_order_acquire))
            {
                while(locks_[index].test_and_set(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                try
                {
                    performDelayReturn(index);
                }
                catch(...)
                {
                    locks_[index].clear(std::memory_order_release);
                    returnBusy_[index].clear(std::memory_order_release);
                    throw;
                }
                locks_[index].clear(std::memory_order_release);
                returnBusy_[index].clear(std::memory_order_release);
            }
        }
}

bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount, 
        std::chrono::steady_clock::time_point currentTime)
{
    // 更保守：同时满足计数与时间间隔，减少频繁触发
    if(currentCount < MAX_DELAY_COUNT) return false;
    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL;
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
    size_t scanBudget = 1000000; // 防御性扫描上限，避免异常环导致长时间阻塞

    while(currentBlock && scanBudget--)
    {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if(tracker)
        {
            spanFreeCounts[tracker]++;
        }
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    if(scanBudget == 0)
    {
        // 遇到异常超长/循环链表，直接退出本次归还，等待后续周期再尝试
        return;
    }

    // 更新每个span的空闲计数并检查是否可以归还
    for(const auto& [tracker, newFreeBlocks] : spanFreeCounts)
    {
        updateSpanFreeCount(tracker,newFreeBlocks,index);
    }
}

void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index)
{
    // 直接更新为当前在链表中统计到的空闲块数（而非累加历史值）
    tracker->freeCount.store(newFreeBlocks, std::memory_order_release);
    size_t newFreeCount = newFreeBlocks;

    // 当所有块都空闲时，归还span
    if(newFreeCount == tracker->blockCount.load(std::memory_order_relaxed))
    {
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        // 从自由链表中移除这些块
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = head;
        void* prev = nullptr;
        void* current = head;

        while(current)
        {
            void* next = *reinterpret_cast<void**>(current);
            if(current >= spanAddr &&
                current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
            {
                if(prev)
                {
                    *reinterpret_cast<void**>(prev) = next;
                }
                else
                {
                    newHead = next;
                }
            }
            else
            {
                prev = current;
            }
            current = next;
        }

        centralFreeList_[index].store(newHead,std::memory_order_release);
        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
}

void* CentralCache::fetchFromPageCache(size_t size)
{
    // 实际需要的页数
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 决定分配策略
    if(size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        // 小于32KB的请求，固定使用8页
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        // 大于32KB的请求，按实际需求分配
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    // 避免越界：只遍历有效范围
    size_t limit = spanCount_.load(std::memory_order_relaxed);
    if(limit > spanTrackers_.size()) limit = spanTrackers_.size();

    for(size_t i = 0 ; i < limit; ++i)
    {
        void* spanAddr = spanTrackers_[i].spanAddr.load(std::memory_order_relaxed);
        size_t numPages = spanTrackers_[i].numPages.load(std::memory_order_relaxed);

        if(blockAddr >= spanAddr &&
            blockAddr < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
        {
            return &spanTrackers_[i];
        }
    }
    return nullptr;
}

}