#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"
#include <cassert>

namespace my_memorypool
{

void* ThreadCache::allocate(size_t size)
{
    // 处理0大小的分配请求
    if(size == 0)
    {
        size = ALIGNMENT; // 至少分配一个对对齐大小
    }

    if(size > MAX_BYTES)
    {
        // 大对象直接从系统分配
        return malloc(size);
    }

    size_t index = SizeClass::getIndex(size);

    // 检查线程本地自由链表
    // 如果 freeList_[index] 不为，表示该链表中有可用内存块
    if(void* ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr); // 将freeList_[index]指向的内存块的下一个内存块地址（取决与内存块的实现）
        // 只有成功弹出时才减少计数
        if (freeListSize_[index] > 0)
        {
            freeListSize_[index]--;
        }
        return ptr;
    }

    // 如果线程本地自由链表为空，则从中心缓存获取一批内存
    return fetchFromCentralCache(index, size);
}

void ThreadCache::deallocate(void* ptr, size_t size)
{
    if(size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    // 插入到线程本地自由链表
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;

    // 更新对应自由链表的长度计数
    freeListSize_[index]++;

    // 判断是否需要将部分内存回收给中心缓存
    if(shouldReturnToCentralCache(index))
    {
        returnToCentralCache(freeList_[index], size);
    }
}

// 判断是否需要将部分内存回收给中心缓存
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    // 设定阈值
    size_t threadcnt = 256;
    return (freeListSize_[index] > threadcnt);
}

void* ThreadCache::fetchFromCentralCache(size_t index, size_t size)
{
    // 慢启动策略：
    // 如果需要的内存块较小，我们也不一次拿太多，避免浪费
    // 随着 freeListSize_[index] 增长，或者根据 MaxLimit 动态调整
    // 简单起见，我们根据 size 大小决定一次拿多少
    // 比如：小对象(<=64B)一次拿 512 个，中对象(<=4KB)一次拿 64 个
    
    // 计算 ThreadCache 最大容量限制 (这里先硬编码简单逻辑)
    size_t batchNum = 1;
    if (size <= 64) batchNum = 512;
    else if (size <= 512) batchNum = 128;
    else if (size <= 4096) batchNum = 32;
    else batchNum = 4; // 大块少拿点

    void* start = nullptr;
    void* end = nullptr;
    
    // 从中心缓存批量获取内存
    size_t actualNum = CentralCache::getInstance().fetchRange(start, end, batchNum, index);
    if(actualNum == 0) return nullptr;

    assert(start != nullptr);
    assert(end != nullptr);

    // 取一个返回给 threadAlloc
    void* result = start;
    if (actualNum == 1) {
        // 只有一个，没有剩余
    } else {
        // 将剩下的放入 freeList
        void* remainStart = *reinterpret_cast<void**>(result);
        // result 的 next 置空
        //*reinterpret_cast<void**>(result) = nullptr; // 实际上不需要，调用者会覆盖
        
        // 将链表[remainStart ... end] 插入 freeList_ 头部
        if (remainStart) {
             // 找到 end (实际上 fetchRange 自带了 end 指针，它指向的是这批链表的最后一个节点)
             // end 的 next 应该接到旧的 freeList_ 上
             *reinterpret_cast<void**>(end) = freeList_[index];
             freeList_[index] = remainStart;
             
             freeListSize_[index] += (actualNum - 1);
        }
    }

    return result;
}

void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    // 根据大小计算对应的索引
    size_t index = SizeClass::getIndex(size);

    // 获取对齐后的实际块大小
    size_t alignedSize = SizeClass::roundUp(size);

    //计算要归还的内存块数量
    size_t batchNum = freeListSize_[index];
    if(batchNum <= 1) return;

    // 保留一部分在ThreadCache中（比如保留1/4）
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 将内存块串成链表
    char* current = static_cast<char*>(start);
    // 使用对齐后的大小计算分割点
    char* splitNode = current;
    for(size_t i = 0 ; i < keepNum - 1; ++i)
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
        if(splitNode == nullptr)
        {
            // 如果链表提前结束，更新实际的返回数量
            returnNum = batchNum - (i + 1);
            break;
        }
    }

    if(splitNode != nullptr)
    {
        // 将要返回的部分和要保留的部分断开
        void* nextNode = *reinterpret_cast<void**>(splitNode);
        *reinterpret_cast<void**>(splitNode) = nullptr;

        // 更新ThreadCache的空闲链表
        freeList_[index] = start;

        // 更新自由链表大小
        freeListSize_[index] = keepNum;

        // 将剩下部分返回给CentralCache
        if(returnNum > 0 && nextNode != nullptr)
        {
            CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
        }
    }
}

}