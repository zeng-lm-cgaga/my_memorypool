#pragma once
#include <cstddef>
#include <atomic>
#include <array>

//定义对齐数和最大内存池大小
namespace  my_memorypool
{
constexpr std::size_t ALIGNMENT = 8;
constexpr std::size_t MAX_SIZE = 256 * 1024; // 256KB
constexpr std::size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // 64个空闲块
}
