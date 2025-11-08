#include <sys/mman.h>
#include "PageCache.h"
#include <cstring>

namespace my_memorypool
{

void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找合适的空闲span
    // lower_bound函数返回第一个大于等于numPages的迭代器
    auto it = freeSpans_.lower_bound(numPages);
    if(it != freeSpans_.end()) 
    {
        Span* span = it->second;

        //将取出的span从原来的空闲链表freeSpans_[it->first]中移除
        if(span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        //如果span大于需要的numPages则进行分割
        if(span->numPages > numPages ) 
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            //将超出部分放回Span*列表头部
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            span->numPages = numPages;
        }

        // 记录span信息用于回收
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的空闲span，想系统申请
    void* memory = systemAlloc(numPages);
    if(!memory) return nullptr;

    // 创建新的span
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录span信息用于回收
    spanMap_[memory] = span;
    return memory;
}

void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    //查找对应的span，没找到代表不是PageCache管理的内存，直接返回
    auto it = spanMap_.find(ptr);
    if(it == spanMap_.end()) return;

    Span* span = it->second;

    //尝试合并相邻的span
    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);
    if(nextIt != spanMap_.end()) 
    {
        Span* nextSpan = nextIt->second;

        //从空闲列表中移除下一个span
        auto& nextList = freeSpans_[nextSpan->numPages];
        if(nextList == nextSpan) //如果nextSpan是头节点
        { // 将nextSpan从链表freeSpans_[nextSpan->numPages]中移除
            nextList = nextSpan->next;
        }
        else // 如果nextSpan不是链表的头节点
        {
            Span* prev = nextList;
            while(prev->next != nextSpan );
                prev = prev->next;
            prev->next = nextSpan->next;
        }

        // 合并span
        span->numPages += nextSpan->numPages;
        spanMap_.erase(nextAddr);
        delete nextSpan;
    }

    // 将合并后的span插入空闲列表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}


void * PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

    //使用mmap分配内存
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1 , 0);
    if(ptr == MAP_FAILED) return nullptr;

    //清零内存
    memset(ptr, 0, size);
    return ptr;
}

}//namespace my_memorypool