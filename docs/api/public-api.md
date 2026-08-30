# ammalloc 公共 API 参考

> 本文件汇总 ammalloc 公共 API 的语义，与 `include/ammalloc/ammalloc.h` 的 Doxygen 注释**同源同步**：修改头文件注释必须同步本文件，反之亦然。环境变量与构建选项以 [README.md](../../README.md) 为单一事实源，本文件不复制其表格。

## 1. 分配与释放

### `void* ammalloc::am_malloc(size_t original_size)`

| 项 | 内容 |
|---|---|
| 简介 | 分配至少 `original_size` 字节的存储（对应 `@brief`） |
| 参数 | `original_size`：请求字节数；零大小请求映射到分配器最小尺寸类别（对应 `@param`） |
| 返回值 | 指向已分配存储的指针；分配失败时返回 `nullptr`（对应 `@return`） |
| 语义 | 线程安全；`am_malloc(0)` 不立即返回 null，而是从最小尺寸类别分配（对应 `@note`） |
| 前置条件 | 无（对应 `@pre`） |

### `void ammalloc::am_free(void* ptr)`

| 项 | 内容 |
|---|---|
| 简介 | 将先前由 `am_malloc` 分配的存储归还分配器（对应 `@brief`） |
| 参数 | `ptr`：`am_malloc` 返回的指针；`nullptr` 作为 no-op 接受（对应 `@param`） |
| 返回值 | 无 |
| 语义 | 只接受 `am_malloc` 返回的原始存活指针；与系统 `free` 混用、释放内部指针、double-free 均不支持；PageMap 无法识别的指针被忽略。Hardened 构建可拒绝部分非法释放，但不改变这些输入未定义的 API 契约（对应 `@note`） |
| 前置条件 | `ptr` 必须是 `am_malloc` 返回且尚未释放的指针（对应 `@pre`） |

### 示例

```cpp
#include <ammalloc/ammalloc.h>

#include <cstddef>

int main() {
    constexpr std::size_t size = 1024;
    void* ptr = ammalloc::am_malloc(size);
    if (ptr == nullptr) {
        return 1;
    }

    // Use the storage at ptr, which is at least size bytes long.

    ammalloc::am_free(ptr);
    return 0;
}
```

## 2. ThreadCache 回收控制

### `void ammalloc::am_thread_cache_trim() noexcept`

仅对**调用线程**已存在的 ThreadCache 执行 soft trim；不会为了 trim 创建 TLS
cache。它从大尺寸类开始扫描，保留至多一个现有 batch 的 warm objects，其余对象照常
归还 CentralCache 的 TransferCache 以供跨线程复用。该操作是 owner-thread safepoint，
不保证将所有对象返回 Span bitmap。

### `void ammalloc::am_thread_cache_purge() noexcept`

仅清空调用线程的 ThreadCache，并令本线程对象绕过 TransferCache 直接归还 Span bitmap；
随后 drain 调用期间观察到的全局 TransferCache snapshot。由此变空的 Span 会归还 PageCache，并在正常
Scavenger idle 策略下成为 `MADV_DONTNEED` 候选。它是显式、较重的控制面操作，不进入
`am_malloc`/`am_free` 常见快路径。另注意：purge 会将各类配额收缩到实际驻留量（空类回到
一个对象的慢启动地板），purge 后的分配需求将重新经历慢启动增长；延迟敏感的安全点
优先使用 `am_thread_cache_trim()`。

### `void ammalloc::am_request_thread_cache_trim() noexcept` / `am_request_thread_cache_purge() noexcept`

发布 process-wide cooperative request，不会访问或修改其他线程的 TLS FreeList。每个
owner thread 只在自己的 refill、overflow trim 或显式 `am_thread_cache_trim()` safepoint
观察该 epoch；存在两类观察盲区——完全 idle 的线程，以及稳定工作集、收支平衡的稳态线程
（缓存永不清空且永不过限，不进入慢路径）——均需由其 scheduler/event loop 主动调用前两个
API 才能立即回收。`am_request_thread_cache_purge()` 同时对 TransferCache 做有界 drain。

这些 API 管理的是 allocator cached-object retention，不承诺固定的 RSS 降幅：一个缓存
对象可能钉住整个 Span，而 `madvise` 的实际物理页回收仍由 PageCache/Scavenger 决定。

## 3. 语义约束

- `am_malloc` 返回的指针**只能**传给 `am_free`，不能与系统 `free`/`delete` 混用。
- 当前实现不替换全局 `new`/`delete`，也不拦截 libc 分配函数。
- 大对象（> 32 KiB）按整页向上取整后直接由后端分配，地址记录于 PageMap，`am_free` 经 PageMap 在 O(1) 时间定位归属 Span。

## 4. 运行配置与构建选项

- 环境变量（`AM_USE_MAP_POPULATE`、`AM_ENABLE_SCAVENGER`、`AM_TC_SIZE`）：见 [README.md "Runtime Configuration"](../../README.md)。
- 构建选项（`BUILD_TESTS`、`BUILD_BENCHMARKS`、`USE_57BIT_VA`）：见 [README.md "Build Options"](../../README.md)。
- 环境变量在进程首次初始化时读取并保持不可变；必须在首次调用 `am_malloc` 前设置。

## 5. 相关文档

- 头文件：[include/ammalloc/ammalloc.h](../../include/ammalloc/ammalloc.h)（Doxygen 注释为语义事实源）
- 架构总览：[docs/designs/ammalloc_design.md](../designs/ammalloc_design.md)
- 改进路线（未来 API 演进方向）：[docs/improvement-plan/03-correctness-bootstrap-and-abi.md](../improvement-plan/03-correctness-bootstrap-and-abi.md)
- 注释规范：[docs/guides/cpp_comment_guidelines.md](../guides/cpp_comment_guidelines.md)
