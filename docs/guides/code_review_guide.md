# 代码审查指南

> AetherMind 项目系统化代码审查方法 - 风险分级驱动版

---

## 快速导航

- [0. PR 风险分级](#0-pr-风险分级) - **第一步：确定审查深度**
- [1. 快速门禁](#1-快速门禁) - **必须通过的基础检查**
- [2. 维度检查清单](#2-维度检查清单) - **按风险级别选择检查项**
- [3. 深度审查剧本](#3-深度审查剧本) - **高风险 PR 专项审查**
- [4. 报告模板](#4-报告模板) - **标准化输出格式**
- [附录：实际案例](#附录实际案例)

---

## 0. PR 风险分级

**第一步：确定本次变更的风险等级，选择对应的审查深度**

| 级别 | 触发条件 | 审查时间 | 必需输出 |
|------|----------|----------|----------|
| **🟢 Quick** | 纯文档、注释、测试用例；无行为变更的重命名/格式化 | 5-10 min | 编译通过 + 现有测试通过 |
| **🟡 Standard** | 常规功能添加、bug 修复、非核心模块重构 | 30-60 min | + 静态分析无警告 + 新增测试覆盖 + 代码走读 |
| **🔴 Deep** | 分配器/并发/锁/原子操作、热路径优化、ABI/API 变更、安全敏感代码 | 2-4 hours | + Sanitizer 运行通过 + Benchmark 对比 + 并发安全性证明 |

### 自动分级检查清单

- [ ] **是否涉及多线程/并发？** → Deep
- [ ] **是否修改公共 API 接口？** → Deep
- [ ] **是否在热路径（am_malloc/am_free）？** → Deep
- [ ] **是否涉及内存模型/原子操作？** → Deep
- [ ] **是否处理外部输入（网络/文件/用户数据）？** → Standard 或 Deep
- [ ] **是否修改构建系统/依赖？** → Standard
- [ ] **纯文档/注释/测试？** → Quick

---

## 1. 快速门禁

**所有 PR 必须通过的基础检查（无论风险级别）**

### 1.1 自动化检查（5 分钟内完成）

```bash
# 1. 编译检查
rm -rf build && cmake -S . -B build -DBUILD_TESTS=ON
make --build build --target aethermind_unit_tests -j$(nproc)

# 2. 单元测试（聚焦相关模块）
./build/tests/unit/aethermind_unit_tests --gtest_filter="*SizeClass*:*Span*"

# 3. 格式化检查
clang-format -n --Werror $(git diff --name-only main | grep -E '\.(h|cpp)$')

# 4. 静态分析（如配置了 clang-tidy）
clang-tidy -p build $(git diff --name-only main | grep -E '\.(h|cpp)$')
```

**通过标准**：
- [ ] 编译零警告（`-Werror`）
- [ ] 单元测试 100% 通过
- [ ] 格式化无差异
- [ ] 静态分析无高优先级警告

### 1.2 基础代码检查（5 分钟）

- [ ] **变更范围合理**：单行 PR < 50 行，功能 PR < 300 行
- [ ] **提交信息规范**：`type(scope): description` 格式
- [ ] **无敏感信息泄露**：无密钥、密码、内部 IP
- [ ] **无意外文件**：无 `.o`, `.exe`, `*.log` 等构建产物

---

## 2. 维度检查清单

**根据风险级别选择执行的检查维度**

### 维度覆盖矩阵

| 维度 | Quick | Standard | Deep |
|------|-------|----------|------|
| 2.1 功能性 | ⏭️ | ✅ | ✅ |
| 2.2 设计/架构 | ⏭️ | ✅ | ✅ |
| 2.3 性能 | ⏭️ | ⚠️ | ✅ |
| 2.4 代码质量/风格 | ✅ | ✅ | ✅ |
| 2.5 可维护性 | ⏭️ | ✅ | ✅ |
| 2.6 安全性 | ⏭️ | ✅ | ✅ |
| 2.7 并发与内存模型 | ⏭️ | ⏭️ | ✅ |
| 2.8 兼容性与可移植性 | ⏭️ | ⚠️ | ✅ |
| 2.9 测试 | ⏭️ | ✅ | ✅ |
| 2.10 可观测性 | ⏭️ | ⏭️ | ✅ |
| 2.11 文档 | ✅ | ✅ | ✅ |
| 2.12 构建/依赖卫生 | ⏭️ | ✅ | ✅ |

> ⏭️ = 跳过, ✅ = 必须检查, ⚠️ = 快速检查

---

### 2.1 功能性 (Functionality)

**核心问题：代码是否正确实现了需求？**

#### 🔴 必须修复（P0）

| 检查项 | 证据要求 | 工具/方法 |
|--------|----------|-----------|
| **算法正确性** | 边界测试通过，数学推导验证 | 单元测试 + 代码走读 |
| **空值处理** | 所有指针参数有 null 检查 | `grep -n "if (!ptr)"` |
| **数组越界** | 访问索引有范围验证 | 静态分析 + ASan |
| **整数溢出** | 关键计算有溢出检查 | UBSan + 代码审查 |

#### 🟡 应该修复（P1）

- [ ] 错误处理路径覆盖完整
- [ ] 无未处理返回值（nodiscard 检查）
- [ ] 资源泄漏防护（RAII 验证）

#### 证据模板

```markdown
**算法验证**: [测试用例文件名:行号]
- 输入: [边界值]
- 输出: [预期结果]
- 状态: ✅ 通过
```

---

### 2.2 设计/架构 (Design/Architecture)

**核心问题：代码结构是否合理、可扩展？**

#### SOLID 原则检查

| 原则 | 检查项 | 证据要求 | 工具 |
|------|--------|----------|------|
| **S**ingle Responsibility | 函数 < 50 行，类 < 300 行 | `wc -l <file>` | 人工审查 |
| **O**pen/Closed | 新增功能无需修改现有代码 | 对比基线 | 架构图 |
| **L**iskov Substitution | 派生类不违反基类契约 | 继承层次检查 | 编译器 |
| **I**nterface Segregation | 接口方法数量 < 10 | `grep -c "virtual"` | 人工审查 |
| **D**ependency Inversion | 依赖抽象而非具体 | 检查 include 依赖 | include-what-you-use |

#### C++ 设计模式红榜

```cpp
// ❌ Bad: 裸指针所有权不明
void Process(Node* node);

// ✅ Good: 所有权明确
void Process(std::unique_ptr<Node> node);
void Process(const Node& node);  // 不获取所有权

// ❌ Bad: 隐式类型转换
void SetTimeout(int ms);
SetTimeout(3.5);  // 编译通过但语义错误

// ✅ Good: explicit 禁止隐式转换
explicit Timeout(int ms);
```

---

### 2.3 性能 (Performance)

**核心问题：代码是否足够高效？**

#### Standard 级别检查（快速）

| 检查项 | 通过标准 | 工具 |
|--------|----------|------|
| **算法复杂度** | 无 O(n²) 以上嵌套循环 | 代码走读 |
| **拷贝消除** | 返回值使用移动语义 | `clang-tidy performance-*` |
| **动态分配** | 热路径无频繁 new/delete | 代码走读 |

#### Deep 级别检查（详细）

```bash
# 1. Benchmark 对比（与基线分支）
./build/tests/benchmark/aethermind_benchmark --benchmark_filter="*Malloc*"
# 要求：性能下降 < 5%

# 2. Profiling（热点识别）
perf record ./build/tests/unit/aethermind_unit_tests
perf report

# 3. Cache Miss 分析
valgrind --tool=cachegrind ./test_binary
cg_annotate cachegrind.out.*
```

| 指标 | 通过标准 | 证据 |
|------|----------|------|
| 热路径延迟 | 无回归 | benchmark 对比截图 |
| 缓存命中率 | L1 > 95% | cachegrind 报告 |
| 内存带宽 | 无异常峰值 | perf 报告 |

#### ammalloc 专项性能检查

- [ ] **分支预测标注**：热路径使用 `AM_LIKELY`/`AM_UNLIKELY`
- [ ] **缓存行对齐**：高频结构 `alignas(64)`
- [ ] **无锁设计**：ThreadCache 无锁访问
- [ ] **编译期计算**：配置使用 `constexpr`/`consteval`

---

### 2.4 代码质量/风格 (Code Quality/Style)

**所有级别必须检查**

#### AetherMind 命名规范

| 类型 | 规范 | 示例 |
|------|------|------|
| 类型 | PascalCase | `SizeClass`, `RadixNode` |
| 函数（核心 API） | snake_case | `am_malloc`, `get_span` |
| 函数（内部） | camelCase | `calculateIndex`, `allocObject` |
| 枚举 | kPrefix | `kCPU`, `kCUDA` |
| 宏 | AM_ + 大写下划线 | `AM_LIKELY`, `AM_CHECK` |
| 私有成员 | 下划线后缀 | `size_table_`, `mutex_` |

#### 现代 C++ 检查清单

| 特性 | 使用场景 | 禁用场景 |
|------|----------|----------|
| `auto` | 类型明显冗长时 | 降低可读性时 |
| `nullptr` | 所有空指针 | 禁用 `NULL`/`0` |
| `std::span` | 数组参数 | 非拥有性视图 |
| `std::string_view` | 字符串参数 | 需要修改时 |
| `[[nodiscard]]` | 返回值必须检查 | - |
| `[[maybe_unused]]` | 故意不使用 | - |

#### 代码坏味道红榜

```cpp
// ❌ Bad: 深层嵌套
if (a) {
    if (b) {
        if (c) {
            // 逻辑
        }
    }
}

// ✅ Good: 提前返回
if (!a) return;
if (!b) return;
if (!c) return;
// 逻辑

// ❌ Bad: 魔法数字
if (size > 32768) { ... }

// ✅ Good: 命名常量
constexpr size_t kMaxTcSize = 32 * 1024;
if (size > kMaxTcSize) { ... }
```

---

### 2.5 可维护性 (Maintainability)

**核心问题：代码是否易于理解和修改？**

| 检查项 | 通过标准 | 工具 |
|--------|----------|------|
| **圈复杂度** | 函数 < 10 | `lizard` 工具 |
| **注释比例** | 10%-30% | `cloc` 统计 |
| **TODO 跟踪** | 所有 TODO 有 Issue 链接 | `grep -r "TODO"` |
| **重复代码** | 无 > 10 行重复 | `jscpd` 或 `simian` |

#### 注释质量检查

```cpp
// ❌ Bad: 废话注释
// Increment i
i++;

// ✅ Good: 意图注释
// Advance to next span, skipping empty ones to reduce lock contention
i++;

// ❌ Bad: 过时注释
// This function returns -1 on error (实际返回 nullptr)

// ✅ Good: API 文档
/**
 * @brief Maps a requested memory size to its corresponding size class index.
 * @param size The requested allocation size in bytes. Must be > 0.
 * @return The zero-based index of the size class, or SIZE_MAX if size exceeds limit.
 * @note This is a hot path; implementation uses branch prediction hints.
 */
```

---

### 2.6 安全性 (Security)

**核心问题：代码是否存在安全漏洞？**

| 检查项 | 风险 | 修复方案 | 验证工具 |
|--------|------|----------|----------|
| **缓冲区溢出** | 高危 | 使用标准容器 | ASan |
| **整数溢出** | 高危 | 检查乘法/加法溢出 | UBSan |
| **use-after-free** | 高危 | 智能指针/RAII | ASan |
| **double-free** | 高危 | 所有权明确 | 代码审查 |
| **数据竞争** | 高危 | 原子操作/锁 | TSan |
| **未初始化读** | 中危 | 初始化列表 | MSan |
| **格式字符串** | 中危 | 使用 `std::format` | 编译器警告 |

#### ammalloc 安全红线

- [ ] **无递归 malloc**：分配器内部不使用 STL 堆容器
- [ ] **自举安全**：`PageAllocator::SystemAlloc` 不触发递归
- [ ] **元数据完整性**：Span 元数据有校验机制

---

### 2.7 并发与内存模型 (Concurrency & Memory Model)

**🔴 Deep 级别必须详细检查**

#### 锁策略检查

| 检查项 | 通过标准 | 证据 |
|--------|----------|------|
| **锁顺序** | 全局一致的加锁顺序 | 文档化锁层级图 |
| **死锁风险** | 无循环依赖 | 代码走读 + 工具 |
| **锁粒度** | 临界区最小化 | 测量持有时间 |
| **双锁等待** | 使用 `std::lock` 或层次锁 | 代码审查 |

#### 原子操作检查

```cpp
// ❌ Bad: 默认内存序
std::atomic<int> counter;
counter++;  // seq_cst，性能损失

// ✅ Good: 显式内存序
counter.fetch_add(1, std::memory_order_relaxed);  // 仅计数

counter.store(1, std::memory_order_release);  // 发布数据
counter.load(std::memory_order_acquire);      // 获取数据
```

| 场景 | 推荐内存序 | 理由 |
|------|------------|------|
| 纯计数器 | `relaxed` | 无同步需求 |
| 标志位设置 | `release`/`acquire` | 可见性保证 |
| 状态机转换 | `seq_cst` | 全局顺序一致性 |

#### 内存模型验证

```bash
# 线程安全检查
./build-tsan/tests/unit/aethermind_unit_tests

# 检查数据竞争报告
# 要求：零数据竞争报告
```

---

### 2.8 兼容性与可移植性 (Compatibility & Portability)

| 检查项 | Quick | Standard | Deep |
|--------|-------|----------|------|
| **API 兼容性** | - | ABI 检查 | 版本化 API |
| **编译器兼容** | GCC | GCC + Clang | GCC + Clang + MSVC |
| **平台 UB** | Linux x86_64 | + ARM64 | + macOS + Windows |
| **特性检测** | - | `__has_feature` | 运行时检测 |

#### C++ 标准检查

```cpp
// 使用特性宏确保可移植性
#ifdef __cpp_lib_constexpr_string
    constexpr std::string_view kVersion = "1.0.0";
#else
    const std::string_view kVersion = "1.0.0";
#endif
```

---

### 2.9 测试 (Testing)

| 级别 | 覆盖率要求 | 测试类型 |
|------|------------|----------|
| Quick | N/A | 现有测试通过 |
| Standard | 新增代码 > 70% | 单元测试 + 边界测试 |
| Deep | 关键路径 > 90% | + 压力测试 + 模糊测试 |

#### 测试检查清单

- [ ] **边界值测试**：最小值、最大值、空值、零值
- [ ] **错误路径**：异常输入、资源耗尽、系统调用失败
- [ ] **并发测试**：多线程访问、竞态条件
- [ ] **性能测试**：基准对比、回归检测

#### 测试质量验证

```bash
# 覆盖率检查
gcovr --filter='ammalloc/' --html-details coverage.html

# 要求：
# - 行覆盖率 > 70%（Standard）或 > 90%（Deep）
# - 分支覆盖率 > 60%
```

---

### 2.10 可观测性 (Observability)

**🔴 Deep 级别必须检查**

| 检查项 | 通过标准 | 示例 |
|--------|----------|------|
| **错误日志** | 关键路径有结构化日志 | `spdlog::error("malloc failed: {}", size)` |
| **性能指标** | 可导出关键指标 | 分配速率、碎片率、缓存命中率 |
| **调试信息** | 调试构建有详细检查 | `AM_DCHECK(ptr != nullptr)` |
| **故障诊断** | 崩溃时可获取堆栈 | `libbacktrace` 集成 |

#### 日志级别规范

```cpp
spdlog::trace("Detailed debug info");        // 开发调试
spdlog::debug("Internal state: {}", value);  // 调试构建
spdlog::info("Scavenger released {} MB", mb); // 关键事件
spdlog::warn("High fragmentation detected");  // 警告
spdlog::error("SystemAlloc failed: {}", errno); // 错误
```

---

### 2.11 文档 (Documentation)

**所有级别必须检查**

| 检查项 | Quick | Standard | Deep |
|--------|-------|----------|------|
| **API 文档** | N/A | Doxygen 注释 | 完整示例 |
| **设计文档** | N/A | 架构图 | 详细设计文档 |
| **变更日志** | N/A | CHANGELOG 条目 | 迁移指南 |

#### 文档质量检查

- [ ] 公共 API 有完整的 Doxygen 注释
- [ ] 复杂算法有数学推导或流程图
- [ ] 非直观代码有 "为什么" 注释
- [ ] TODO/FIXME 关联 Issue 编号

---

### 2.12 构建/依赖卫生 (Build/Dependency Hygiene)

| 检查项 | 通过标准 | 工具 |
|--------|----------|------|
| **第三方依赖** | 仅使用必要依赖 | `cmake --graphviz=deps.dot` |
| **编译时间** | 增量构建 < 30 秒 | `time ninja` |
| **头文件依赖** | 无传递性包含爆炸 | `include-what-you-use` |
| **警告清洁** | 零警告编译 | `-Werror` |

---

## 3. 深度审查剧本

**🔴 Deep 级别 PR 专项审查流程**

### 剧本 A：并发代码审查（2-4 小时）

#### Phase 1: 锁分析（30 min）

```bash
# 1. 提取所有锁操作
grep -rn "std::mutex\|std::lock_guard\|std::unique_lock" ammalloc/

# 2. 绘制锁依赖图（手动或使用工具）
# 检查是否存在循环依赖
```

#### Phase 2: 内存序审查（1 小时）

```cpp
// 检查清单：所有原子操作必须有显式内存序
std::atomic<T> var;

// ✅ Good
var.store(value, std::memory_order_release);
var.load(std::memory_order_acquire);

// ❌ Bad（默认 seq_cst，性能差且意图不明）
var.store(value);
var.load();
```

#### Phase 3: 数据竞争测试（30 min）

```bash
# TSan 构建
cmake -S . -B build-tsan -DENABLE_TSAN=ON
make --build build-tsan --target aethermind_unit_tests -j

# 运行并发测试
./build-tsan/tests/unit/aethermind_unit_tests --gtest_filter="*Thread*:*Concurrent*"

# 要求：零数据竞争报告
```

#### Phase 4: 死锁压力测试（30 min）

```cpp
// 编写压力测试：大量线程随机分配/释放
TEST(ConcurrencyStress, DeadlockDetection) {
    constexpr int kNumThreads = 100;
    constexpr int kOpsPerThread = 10000;
    
    std::vector<std::thread> threads;
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([]() {
            for (int j = 0; j < kOpsPerThread; ++j) {
                void* p = am_malloc(random_size());
                am_free(p);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();  // 如果死锁，这里会卡住
    }
}
```

---

### 剧本 B：分配器代码审查（2-4 小时）

#### Phase 1: 自举安全验证（1 小时）

检查清单：
- [ ] 分配器内部不使用 STL 堆容器（`std::vector`, `std::string`, `std::map`）
- [ ] `SystemAlloc` 路径不触发递归 `malloc`
- [ ] 元数据分配使用 `ObjectPool` 或静态存储

```bash
# 检查 STL 容器使用
grep -rn "std::vector\|std::string\|std::map" ammalloc/src/

# 应该只出现在测试文件中，不出现在核心代码
```

#### Phase 2: 热路径性能审查（1 小时）

```cpp
// 检查清单：am_malloc 热路径必须无锁、无系统调用
void* am_malloc(size_t size) {
    // ✅ Good: TLS 访问（无锁）
    auto* tc = pTLSThreadCache;
    
    // ✅ Good: 本地链表操作（无锁）
    if (!tc->free_list.empty()) {
        return tc->free_list.pop();
    }
    
    // ❌ Bad: 热路径持有锁
    std::lock_guard<std::mutex> lock(global_mutex);
}
```

#### Phase 3: 碎片率测试（30 min）

```cpp
TEST(Allocator, Fragmentation) {
    // 模拟真实负载
    std::vector<void*> ptrs;
    for (int i = 0; i < 10000; ++i) {
        ptrs.push_back(am_malloc(random_size()));
    }
    
    // 随机释放 50%
    for (size_t i = 0; i < ptrs.size(); i += 2) {
        am_free(ptrs[i]);
    }
    
    // 检查碎片率
    double fragmentation = GetFragmentationRatio();
    EXPECT_LT(fragmentation, 0.15);  // < 15%
}
```

#### Phase 4: 内存泄漏检测（30 min）

```bash
# Valgrind 检查
valgrind --leak-check=full --show-leak-kinds=all \
    ./build/tests/unit/aethermind_unit_tests

# 要求：
# - 无 definitely lost
# - 无 indirectly lost
# - possibly lost 需要人工审查
```

---

## 4. 报告模板

### 4.1 分级报告模板

```markdown
# 代码审查报告

## 基本信息
- **审查日期**: 2026-03-07
- **审查人**: [姓名]
- **PR**: [#123](link)
- **风险级别**: 🔴 Deep / 🟡 Standard / 🟢 Quick
- **审查耗时**: [X 小时]

## 快速门禁结果
- [x] 编译通过（零警告）
- [x] 单元测试 100% 通过
- [x] 格式化检查通过
- [x] 静态分析无高优先级警告

## 维度审查结果

### P0 严重问题（必须修复）
| # | 问题 | 位置 | 证据 | 修复建议 |
|---|------|------|------|----------|
| 1 | 数据竞争 | `page_cache.cpp:89` | TSan 报告：`Read of size 8...` | 添加 `std::memory_order_acquire` |
| 2 | 整数溢出 | `span.cpp:45` | UBSan 报告：`signed integer overflow` | 使用 `checked_add` |

### P1 中等问题（建议修复）
| # | 问题 | 建议 |
|---|------|------|
| 1 | 复杂度过高 | 将函数拆分为 3 个子函数 |
| 2 | 缺少边界测试 | 添加 size=0 和 size=MAX 测试用例 |

### P2 轻微问题（后续优化）
| # | 问题 | 建议 |
|---|------|------|
| 1 | 注释不清晰 | 添加算法说明 |

## 证据附件
- [TSan 报告](tsan.log)
- [Benchmark 对比](benchmark_diff.md)
- [覆盖率报告](coverage.html)

## 结论
- **状态**: 🟡 有条件通过（修复 P0 后可合并）
- **下次审查**: 修复后需要重新跑 TSan
```

### 4.2 严重级别定义

| 级别 | 定义 | 处理时限 | 示例 |
|------|------|----------|------|
| **P0** | 阻塞性问题，合并前必须修复 | 24 小时 | 数据竞争、内存泄漏、崩溃 |
| **P1** | 重要问题，强烈建议修复 | 1 周内 | 性能回归、边界情况未处理 |
| **P2** | 轻微问题，可以后续优化 | 下次迭代 | 代码风格、注释缺失 |

---

## 附录：实际案例

### 案例 1：所有权 Bug（P0）

**问题代码**:
```cpp
// span.h
Span* GetSpan(void* ptr) {
    auto* span = page_map_.Get(ptr);
    if (span) {
        span->Ref();  // 引用计数 +1
    }
    return span;  // ❌ 调用者负责释放？不释放？
}
```

**审查发现**:
- 调用者不清楚是否需要 `Unref()`
- 已有 3 处调用，2 处释放，1 处未释放

**修复方案**:
```cpp
// 明确所有权语义
[[nodiscard]] ObjectPtr<Span> GetSpan(void* ptr);  // 转移所有权
Span* GetSpanRaw(void* ptr);  // 借用，不拥有
```

**教训**: API 必须明确所有权，使用智能指针或命名约定。

---

### 案例 2：内存序 Bug（P0）

**问题代码**:
```cpp
// page_cache.cpp
std::atomic<bool> initialized_{false};
RadixNode* root_;

void Init() {
    root_ = new RadixNode();  // 1. 写数据
    initialized_ = true;       // 2. 默认 seq_cst，但...
}

RadixNode* GetRoot() {
    if (initialized_.load()) {  // 3. 可能看到 true，但 root_ 未初始化
        return root_;           // 4. ❌ 返回空悬指针
    }
}
```

**审查发现**:
- 缺少 `release`/`acquire` 配对
- 在 ARM 上可能看到 `initialized_=true` 但 `root_` 未写入

**修复方案**:
```cpp
void Init() {
    root_ = new RadixNode();
    initialized_.store(true, std::memory_order_release);  // ✅ 发布
}

RadixNode* GetRoot() {
    if (initialized_.load(std::memory_order_acquire)) {   // ✅ 获取
        return root_;  // 保证看到初始化后的 root_
    }
}
```

**教训**: 多线程发布数据必须用 `release`/`acquire`。

---

### 案例 3：性能回归 Bug（P1）

**问题代码**:
```cpp
// 优化前的热路径
void* ThreadCache::Allocate(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);  // ❌ 热路径持锁
    return free_list_.pop();
}
```

**性能影响**:
- 延迟从 ~5ns 增加到 ~50ns（10 倍）
- 多线程扩展性急剧下降

**修复方案**:
```cpp
// 使用无锁数据结构
void* ThreadCache::Allocate(size_t size) {
    // ✅ 无锁，纯原子操作
    return free_list_.pop();  // intrusive list，无锁
}
```

**教训**: 热路径必须无锁，使用原子操作或线程本地存储。

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v2.0 | 2026-03-07 | 重构为风险分级驱动，添加 12 维度，添加深度剧本和案例 |
| v1.0 | 2026-03-07 | 初始版本，8 维度基础框架 |

---

**适用范围**: AetherMind 项目  
**维护者**: 开发团队  
**更新频率**: 每季度审查更新
