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
| 语义 | 只接受 `am_malloc` 返回的原始存活指针；与系统 `free` 混用、释放内部指针、double-free 均不支持；PageMap 无法识别的指针被忽略（对应 `@note`） |
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

## 2. 语义约束

- `am_malloc` 返回的指针**只能**传给 `am_free`，不能与系统 `free`/`delete` 混用。
- 当前实现不替换全局 `new`/`delete`，也不拦截 libc 分配函数。
- 大对象（> 32 KiB）按整页向上取整后直接由后端分配，地址记录于 PageMap，`am_free` 经 PageMap 在 O(1) 时间定位归属 Span。

## 3. 运行配置与构建选项

- 环境变量（`AM_USE_MAP_POPULATE`、`AM_ENABLE_SCAVENGER`、`AM_TC_SIZE`）：见 [README.md "Runtime Configuration"](../../README.md)。
- 构建选项（`BUILD_TESTS`、`BUILD_BENCHMARKS`、`USE_57BIT_VA`）：见 [README.md "Build Options"](../../README.md)。
- 环境变量在进程首次初始化时读取并保持不可变；必须在首次调用 `am_malloc` 前设置。

## 4. 相关文档

- 头文件：[include/ammalloc/ammalloc.h](../../include/ammalloc/ammalloc.h)（Doxygen 注释为语义事实源）
- 架构总览：[docs/designs/ammalloc_design.md](../designs/ammalloc_design.md)
- 改进路线（未来 API 演进方向）：[docs/improvement-plan/03-correctness-bootstrap-and-abi.md](../improvement-plan/03-correctness-bootstrap-and-abi.md)
- 注释规范：[docs/guides/cpp_comment_guidelines.md](../guides/cpp_comment_guidelines.md)
