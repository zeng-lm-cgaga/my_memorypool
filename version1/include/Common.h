#pragma once
#include <cstddef>
#include <atomic>
#include <array>

// 定义对齐数和最大内存池大小
namespace  my_memorypool
{
constexpr std::size_t ALIGNMENT = 8;
constexpr std::size_t MAX_SIZE = 256 * 1024; // 256KB
constexpr std::size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT等于指针void*的大小

// 内存块头部信息
struct BlockHeader
{
    size_t size; //内存块大小
    bool inUse; // 内存块是否被占用
    BlockHeader* next; // 指向下一个内存块的指针
};

// 内存块管理（大小）类
class SizeClass
{
public:
    static size_t roundUp(size_t bytes)
    {
        // 向上取整到最接近的对齐边界（ALIGNMENT的倍数）
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    static size_t getIndex(size_t bytes)
    {
        // 确保bytes大于等于ALIGNMENT
        bytes = std::max(bytes, ALIGNMENT);
        // 向上取整后-1，计算得到对应索引
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};

}