# my_memorypool

C++17 实现的高并发内存池，三层缓存架构 + 无锁并发优化。

## 架构

```
ThreadCache (thread_local, 无锁)
    ↓ 批量不足时
CentralCache (CAS 无锁入链 + atomic_flag 自旋锁出链)
    ↓ 无空闲 Span 时
PageCache (mmap 向 OS 申请, 页面合并回收)
```

- **ThreadCache**: `thread_local` 单例，每线程独立空闲链表，分配/释放无锁
- **CentralCache**: 全局共享，32768 个 size-class；`returnRange` 用 CAS 无锁入链（100 万次失败后降级为自旋锁），`fetchRange` 用 atomic_flag 自旋锁保护批量出链，临界区最小化
- **PageCache**: `mmap` 申请 4KB 页，Span 切分与相邻空闲 Span 合并回收

## 构建

```bash
cd version1
mkdir build && cd build

# 默认开启 Span 追踪 + 延迟回收
cmake .. && make

# 纯性能模式（关闭 Span 追踪，仅基准测试用）
cmake .. -DENABLE_SPAN_TRACKING=OFF && make

# 运行单元测试
make test

# 运行性能测试（5 轮取均值）
make perf
```

## 性能

测试环境：4 核 / 4GB / Linux，gcc 10.2.0 -O2

| 场景 | 较 new/delete |
|---|---|
| 小对象（≤256B）单线程 | +~60% |
| 小对象（≤256B）4 线程并发 | +~140% |
| 混合大小（8B~4KB）单线程 | +~45% |

> 每轮默认跑 5 次取均值，详见 `PerformanceTest` 输出。

## 技术要点

- **SizeClass**: 8 字节对齐，覆盖 8B ~ 256KB，共 32768 个 size-class
- **慢启动批量策略**: 小对象（≤64B）单次取 512 块，中对象（≤4KB）取 32 块，大对象取 4 块，减少 CentralCache 交互频率
- **ThreadCache 回收**: 自由链表超过 256 块时触发批量归还，保留 1/4 作为缓冲
- **Span 追踪（可选）**: 启用后延迟回收机制按 Span 聚合空闲块，全空闲时归还 PageCache
- **大对象穿透**: >256KB 的分配直接走 malloc/free，不经过缓存层
