# `thread_local` 在内存池 Thread Cache 中的作用

> **文档定位**: 调研备忘（非规范，不承诺实现；结论供设计参考，与本仓库 ThreadCache 的关联见 [02-thread-cache.md](../02-thread-cache.md)）
> **调研目标**: 系统梳理 `thread_local` 在线程本地缓存的语义、收益与成本，为 ammalloc ThreadCache 的 TLS 设计提供背景依据
> **调研时间**: 2026-08-19

## 1. `thread_local` 的基本语义

`thread_local` 是 C++11 引入的线程存储期（thread storage duration）关键字。

其核心语义是：

> 每个线程都拥有该变量的一份独立实例。不同线程访问同名 `thread_local` 变量时，实际访问的是不同对象。

例如：

```cpp
thread_local int counter = 0;

void Foo() {
    ++counter;
}
```

如果线程 T1 和 T2 都调用 `Foo()`，它们操作的是各自独立的 `counter`：

```text
Thread 1 -> counter@T1
Thread 2 -> counter@T2
```

二者互不影响。

### 1.1 每线程独立实例

```cpp
thread_local ThreadCache cache;
```

表示：

```text
Process
├── Thread 1 -> ThreadCache #1
├── Thread 2 -> ThreadCache #2
└── Thread 3 -> ThreadCache #3
```

而普通全局变量：

```cpp
ThreadCache cache;
```

则表示：

```text
Thread 1 ─┐
Thread 2 ─┼──> 同一个 ThreadCache
Thread 3 ─┘
```

### 1.2 生命周期与线程绑定

`thread_local` 对象的生命周期与线程相关：

```text
线程开始
   │
   ├── 初始化该线程自己的 thread_local 对象
   │
   ├── 使用
   ├── 使用
   ├── 使用
   │
线程退出
   │
   └── 销毁该线程自己的 thread_local 对象
```

因此，`thread_local` 与普通 `static` 变量的本质区别在于：

- `static`：整个进程通常共享一个实例；
- `thread_local`：每个线程拥有一个独立实例。

---

# 2. 为什么内存池中的 Thread Cache 需要使用 `thread_local`

高性能内存分配器通常采用分层结构：

```text
                  malloc / free
                       │
                       ▼
              ┌─────────────────┐
              │   Thread Cache  │
              │    线程私有层     │
              └────────┬────────┘
                       │ miss / spill
                       ▼
              ┌─────────────────┐
              │  Central Cache  │
              │    线程共享层     │
              └────────┬────────┘
                       ▼
              ┌─────────────────┐
              │ Page / Span 层   │
              └────────┬────────┘
                       ▼
                      OS
                mmap / hugepage
```

其中，Thread Cache 的核心目标是：

> 让绝大多数小对象的 `malloc/free` 操作只访问当前线程自己的缓存，而不进入线程共享的数据结构。

因此，Thread Cache 天然适合使用 `thread_local`。

---

# 3. 作用一：消除分配快路径上的锁

假设所有线程共享一个 Thread Cache：

```cpp
ThreadCache global_cache;
```

多个线程会同时访问内部 FreeList：

```text
T1 ─┐
T2 ─┼──> global ThreadCache
T3 ─┤
T4 ─┘
```

例如：

```cpp
FreeList freelists[kNumSizeClasses];
```

如果两个线程同时执行：

```cpp
auto* p = freelist.Pop();
```

就会产生数据竞争。

为了保证正确性，只能加入锁：

```cpp
std::mutex mutex;

void* Allocate(std::size_t size) {
    std::lock_guard lock(mutex);
    ...
}
```

这样每次内存分配都会变成：

```text
malloc
  ↓
lock
  ↓
访问 ThreadCache
  ↓
unlock
```

而 `malloc/free` 是程序中的高频操作，这种全局同步会严重限制并发性能。

使用：

```cpp
thread_local ThreadCache thread_cache;
```

后，每个线程访问自己的缓存：

```text
Thread 1 -> ThreadCache 1
Thread 2 -> ThreadCache 2
Thread 3 -> ThreadCache 3
```

于是 Thread Cache 内部的 FreeList 通常可以使用普通指针操作：

```cpp
void* ThreadCache::Allocate(std::size_t size) {
    auto& list = freelists_[SizeClass(size)];

    if (!list.empty()) {
        return list.Pop();
    }

    return RefillFromCentralCache(size);
}
```

fast path 不需要：

- mutex；
- spinlock；
- atomic CAS；
- 跨线程同步。

这通常是 Thread Cache 最重要的性能收益。

---

# 4. 作用二：降低共享数据竞争和 Cache Line Bouncing

即使不使用互斥锁，而是把共享 FreeList 实现成无锁结构：

```cpp
std::atomic<Node*> head;
```

多个 CPU Core 仍然需要频繁修改同一个 cache line。

例如：

```text
Core 0                        Core 1
   │                            │
   └────── atomic head ─────────┘
```

当不同核心不断修改同一原子变量时，缓存一致性协议需要频繁转移 cache line 的所有权，形成：

> cache line bouncing / cache line ping-pong

因此：

> 无锁并不意味着无竞争。

使用线程私有的 Thread Cache 后：

```text
Core 0 -> ThreadCache 0
Core 1 -> ThreadCache 1
Core 2 -> ThreadCache 2
```

绝大多数元数据修改都发生在线程本地，从而显著减少：

- cache coherence 流量；
- 原子操作；
- cache line ownership 转移；
- 多核竞争。

---

# 5. 作用三：提高内存访问局部性

Thread Cache 还可以提高线程的时间局部性和缓存局部性。

例如：

```cpp
void* p = Alloc(64);
...
Free(p);

void* q = Alloc(64);
```

如果 `Free(p)` 将内存放回当前线程自己的 Thread Cache：

```text
Thread 1
   │
   ▼
ThreadCache
64B -> p
```

那么下一次分配 64B 对象时，很可能再次得到 `p`：

```cpp
q == p
```

这意味着：

- 对应内存页可能仍在当前 CPU 的缓存中；
- allocator 元数据可能仍然是热数据；
- TLB 映射也可能仍然有效。

因此线程本地复用通常可以提升：

```text
时间局部性
+
Cache Locality
+
TLB Locality
```

---

# 6. 作用四：建立“线程私有快路径 + 全局共享慢路径”

高性能 allocator 通常将不同层级的访问频率设计成：

```text
ThreadCache      ████████████████████

CentralCache     ███

PageAllocator    █

OS               ▏
```

绝大多数小对象操作应该停留在线程本地：

```text
Alloc(32)
   │
   ▼
ThreadCache::FreeList[32]
   │
   ├── hit  -> Pop() -> return
   │
   └── miss -> 从 CentralCache 批量补充
```

因此：

```cpp
void* Allocate(std::size_t size) {
    if (size <= kMaxSmallSize) {
        return CurrentThreadCache().Allocate(size);
    }

    return AllocateLarge(size);
}
```

其中：

```cpp
ThreadCache& CurrentThreadCache() {
    thread_local ThreadCache cache;
    return cache;
}
```

`thread_local` 使 allocator 可以天然获得当前线程的私有缓存，而不需要将 `ThreadCache*` 显式传递到所有调用路径中。

---

# 7. 为什么不能简单把 Thread Cache 放在线程栈上

理论上可以在线程入口定义：

```cpp
void Worker() {
    ThreadCache cache;
    ...
}
```

但 allocator 可能在任意调用深度被使用：

```text
Worker
  ↓
Foo
  ↓
Bar
  ↓
std::vector
  ↓
operator new
  ↓
Allocator
```

如果不用 TLS，就需要一路传递：

```cpp
Foo(ThreadCache* cache);
Bar(ThreadCache* cache);
Alloc(ThreadCache* cache, std::size_t size);
```

这会严重污染 API。

`thread_local` 则提供了类似：

```cpp
CurrentThreadCache()
```

的隐式线程上下文：

```cpp
ThreadCache& CurrentThreadCache() {
    thread_local ThreadCache cache;
    return cache;
}
```

这样 allocator 可以在任意调用层级直接找到当前线程自己的缓存。

---

# 8. `thread_local` 不意味着整个 allocator 不需要同步

Thread Cache 私有化，只能解决当前线程访问自己缓存时的同步问题。

典型层次如下：

| 层级 | 是否线程共享 | 是否需要同步 |
|---|---:|---:|
| Thread Cache | 否 | 通常不需要 |
| Central Cache | 是 | 需要 |
| Page / Span Cache | 是 | 需要 |
| OS 内存接口 | 是 | 由系统负责 |

因此：

```text
ThreadCache
    │
    └── miss
          │
          ▼
      CentralCache
```

一旦进入共享层，仍然需要使用：

- mutex；
- spinlock；
- atomic；
- lock-free 结构；
- sharding；
- per-size-class lock；
- NUMA 分片。

Thread Cache 的设计目标不是消除 allocator 中所有同步，而是：

> 尽量让高频快路径避开共享同步。

---

# 9. 跨线程 Free 是必须单独考虑的问题

Thread Cache 使用 TLS 后，还必须处理一个常见场景：

```text
Thread A:
p = Alloc(64)

Thread B:
Free(p)
```

即：

```text
allocation thread != deallocation thread
```

这种情况在 producer-consumer 模型中非常常见：

```text
Producer                      Consumer

Task* p = new Task;
    │
    └──────── queue ────────> process(p)
                               delete p
```

此时不能简单地让 Thread B 直接修改 Thread A 的 Thread Cache，否则又会重新引入跨线程同步。

常见设计包括：

1. 释放到当前线程自己的 Thread Cache；
2. 直接归还 Central Cache；
3. 使用 owner-thread remote free queue；
4. 使用 Span / Slab 级 remote free list；
5. 跨线程释放先进入无锁队列，再批量回收。

因此：

> `thread_local` 主要解决本地分配/释放快路径，而跨线程释放必须由 allocator 架构单独处理。

---

# 10. `thread_local` 本身也存在成本

TLS 访问并不是完全免费的。

在不同平台和 ABI 下，TLS 可能通过线程指针寄存器和固定偏移访问，例如在 x86-64 上通常会使用类似：

```text
FS / GS base + TLS offset
```

不同 TLS model 还包括：

```text
local-exec
initial-exec
local-dynamic
general-dynamic
```

其访问成本有所不同。

但是对于 allocator：

```text
TLS lookup
```

的成本通常远低于：

```text
mutex
atomic contention
cache line bouncing
CentralCache access
```

因此 Thread Cache 采用 TLS 通常具有明显收益。

---

# 11. Thread Cache 的线程退出与析构问题

如果直接定义：

```cpp
thread_local ThreadCache cache;
```

线程退出时通常会执行：

```cpp
~ThreadCache();
```

如果析构函数中需要：

```cpp
FlushToCentralCache();
```

就必须关注进程退出阶段的对象销毁顺序。

例如：

```text
GlobalAllocator 已析构
        ↑
        │
ThreadCache TLS 析构
```

就可能产生：

> static / TLS destruction order problem

因此高性能 allocator 常采用更谨慎的生命周期管理，例如：

- TLS 中只保存指针；
- ThreadCache 由 allocator 自己创建和回收；
- 使用 thread-exit hook；
- 允许部分全局 allocator 状态在进程退出时不主动析构；
- 在线程退出时批量归还缓存。

这属于 Thread Cache 工程实现中需要单独处理的生命周期问题。

---

# 12. 从内存分配器架构理解 Thread Cache

可以将 Thread Cache 类比为 allocator 的 L1 Cache：

```text
CPU Memory Hierarchy          Allocator Hierarchy

L1 Cache                <->   Thread Cache
L2/L3 Cache             <->   Central Cache
DRAM                    <->   Page / Span Cache
OS / Virtual Memory     <->   mmap / hugepage
```

Thread Cache 的主要价值是：

```text
线程局部性
+
时间局部性
+
减少共享状态
+
减少锁竞争
+
减少原子操作
+
减少 Cache Line Bouncing
```

所以 `thread_local` 并不只是一个语言层面的便利，而是高性能并发内存分配器的重要架构基础。

---

# 13. 典型实现形式

一个简化的实现可以是：

```cpp
class ThreadCache {
public:
    void* Allocate(std::size_t size);
    void Deallocate(void* ptr, std::size_t size);

private:
    FreeList freelists_[kNumSizeClasses];
};

ThreadCache& CurrentThreadCache() {
    thread_local ThreadCache cache;
    return cache;
}
```

分配路径：

```cpp
void* Alloc(std::size_t size) {
    return CurrentThreadCache().Allocate(size);
}
```

对应：

```text
Thread T1

Alloc(64)
   │
   ▼
CurrentThreadCache()
   │
   ▼
TLS
   │
   ▼
ThreadCache_T1
   │
   ▼
FreeList[64]
   │
   ├── 非空 -> Pop() -> return
   │
   └── 空   -> CentralCache 批量 refill
```

另一个线程执行同样的代码：

```text
Thread T2
   │
   ▼
CurrentThreadCache()
   │
   ▼
ThreadCache_T2
```

代码完全相同，但访问的是完全不同的 Thread Cache 实例。

---

# 14. 总结

在内存池中：

```cpp
thread_local ThreadCache thread_cache;
```

表达的是一个非常明确的所有权关系：

```text
Thread owns ThreadCache
```

其直接目标是让：

```text
malloc / free
     │
     ▼
当前线程自己的 Thread Cache
     │
     ▼
普通指针操作
```

尽可能避免：

```text
锁
原子竞争
跨核同步
Cache Line Bouncing
共享 Central Cache 访问
```

只有在线程本地缓存不足或需要回收时，才进入共享层：

```text
ThreadCache
    │
    │ batch refill / spill
    ▼
CentralCache
    │
    ▼
PageAllocator
    │
    ▼
OS
```

因此，`thread_local` 在 Thread Cache 中的作用可以概括为：

> **通过为每个线程建立独立的内存缓存实例，将高频内存分配路径从共享并发路径转化为线程私有路径，从而降低同步开销、减少缓存一致性竞争并提升内存访问局部性。**
