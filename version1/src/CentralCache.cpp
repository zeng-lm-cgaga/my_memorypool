#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>

// 添加CAS策略，减少线程持有锁的时间，提高了多线程测试时的性能

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

size_t CentralCache::fetchRange(void*& start, void*& end, size_t batchNum, size_t index)
{
    // 索引检查
    if(index >= FREE_LIST_SIZE ) return 0;

    start = nullptr;
    end = nullptr;
    size_t actualNum = 0;

    // 1. 尝试从 centralFreeList_ 中获取
    while(true)
    {
        void* head = centralFreeList_[index].load(std::memory_order_acquire);
        if(!head) break; // 若为空，转到 PageCache 申请

        while(locks_[index].test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield(); 
        }

        // 双重检查
        head = centralFreeList_[index].load(std::memory_order_relaxed);
        if (head == nullptr) {
            locks_[index].clear(std::memory_order_release);
            break; // 真的没空闲了
        }

        // 在锁保护下，我们可以安全地遍历
        start = head;
        end = head;
        actualNum = 1;

        while (actualNum < batchNum) {
            void* next = *reinterpret_cast<void**>(end);
            if (next == nullptr) break;
            end = next;
            actualNum++;
        }

        // 断开链表
        void* newHead = *reinterpret_cast<void**>(end);
        centralFreeList_[index].store(newHead, std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = nullptr;
        
        // 关键性能修正：将 SpanTracker 更新移出锁外！
        locks_[index].clear(std::memory_order_release);

#if ENABLE_SPAN_TRACKING
        // 更新这批块所属 Span 的引用计数 
        // 优化策略：聚合更新。因为链表中的块很可能属于同一个 Span。
        void* curr = start;
        SpanTracker* lastTracker = nullptr;
        size_t batchCount = 0;

        while(curr) {
            // 优化：只有当跨页或者 Span 改变时才去更新 Tracker
            // 这里为了简单且正确，我们用 getSpanTracker 检查，也可以比较页地址优化
            SpanTracker* tracker = getSpanTracker(curr);
            
            if (tracker != lastTracker) {
                // 提交之前的计数
                if (lastTracker && batchCount > 0) {
                    lastTracker->freeCount.fetch_sub(batchCount, std::memory_order_release);
                }
                lastTracker = tracker;
                batchCount = 0;
            }
            
            if (tracker) {
                batchCount++;
            }
            
            curr = *reinterpret_cast<void**>(curr);
        }
        
        // 提交最后一批
        if (lastTracker && batchCount > 0) {
            lastTracker->freeCount.fetch_sub(batchCount, std::memory_order_release);
        }
#endif

        return actualNum;
    }

    // 2. 如果 CentralCache 为空，向 PageCache 申请新 Span
    size_t size = (index + 1) * ALIGNMENT;

    // 动态计算 numPages：
    // 策略：保证至少能申请到 limit 个对象，且总大小不超过 MAX_SPAN_SIZE (例如 1MB)
    // 这样对于小对象保持 SPAN_PAGES(8页)，对于中大对象则增加页数以减少 mmap 次数
    size_t minObjects = 64; // 至少一次申请 64 个对象
    size_t targetBytes = minObjects * size;
    size_t minPages = (targetBytes + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (minPages < SPAN_PAGES) minPages = SPAN_PAGES;
    // 上限控制，比如单次 span 最大 512KB (128页)
    if (minPages > 128) minPages = 128;

  
    void* spanStart = nullptr;
    size_t numPages = minPages;
    
    spanStart = PageCache::getInstance().allocateSpan(numPages);
    if(!spanStart) return 0;
    
    // -----------------------------------------------------------

    // 切分新 Span
    char* base = static_cast<char*>(spanStart);
    size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

    if (blockNum == 0) return 0;

    // 链接所有块
    void* last = nullptr;
    for(size_t i = 0; i < blockNum; ++i) {
        void* current = base + i * size;
        void* next = (i < blockNum - 1) ? (base + (i+1)*size) : nullptr;
        *reinterpret_cast<void**>(current) = next;
        if (i == blockNum - 1) last = current;
    }

    // 3. 将一部分返回给 ThreadCache，剩下的放入 CentralCache
    
    // 我们至少能满足 min(batchNum, blockNum)
    actualNum = (batchNum < blockNum) ? batchNum : blockNum;
    
    start = base;
    
    // 找到切分点 (第 actualNum 个节点是 end)
    end = start;
    for (size_t i = 1; i < actualNum; ++i) {
        end = *reinterpret_cast<void**>(end);
    }
    
    void* remainStart = *reinterpret_cast<void**>(end);
    *reinterpret_cast<void**>(end) = nullptr; // 断开
    
    // 如果有剩余，放入 centralFreeList_ (使用无锁 CAS)
    if (remainStart) {
        void* remainEnd = last;
        while(true) {
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
    
#if ENABLE_SPAN_TRACKING
    // 注册 SpanTracker (略微简化原逻辑，只注册一次)
    size_t trackerIndex = spanCount_.fetch_add(1, std::memory_order_relaxed);
    if(trackerIndex < spanTrackers_.size())
    {
        spanTrackers_[trackerIndex].spanAddr.store(base, std::memory_order_release);
        spanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
        spanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
        spanTrackers_[trackerIndex].freeCount.store(blockNum - actualNum, std::memory_order_release);
    }
    else
    {
        // Tracker 满了，可能无法回收这块内存
        spanCount_.store(spanTrackers_.size(), std::memory_order_relaxed);
    }
#endif

    return actualNum;
} 


void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if(!start || index >= FREE_LIST_SIZE)
    {
        return;
    }

        size_t blockSize = (index + 1) * ALIGNMENT;
        size_t blockCount = size / blockSize;

        // 1) 仅定位链表尾部（benchmark 模式下不做 Span 追踪）
        void* end = start;
        size_t endCount = 1;
        while(*reinterpret_cast<void**>(end) != nullptr && endCount < blockCount) {
            end = *reinterpret_cast<void**>(end);
            endCount++;
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
