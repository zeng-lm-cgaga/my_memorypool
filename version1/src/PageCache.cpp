#include <sys.mman.h>
#include "PageCache.h"
#include <cstring>

namespace my_memorypool
{

void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找合适的空闲span
    // lower_bound函数返回第一个大于等于numPages的迭代器
    auot if = freeSpans_.lower_bound(numPages);
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



}