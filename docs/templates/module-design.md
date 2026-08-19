# <模块名> 模块设计

> 复制本模板新建模块设计文档，存放于 `docs/designs/`，按 [文档系统规范](../guides/documentation-guide.md) 命名与维护。

- **状态**: Current（描述已验证实现；只写仓库事实）
- **版本**: 1.0
- **日期**: YYYY-MM-DD
- **关联代码**: [include/ammalloc/<module>.h](../../include/ammalloc/<module>.h) / [src/<module>.cpp](../../src/<module>.cpp)
- **上游依赖**: <本模块调用哪些模块>
- **下游消费者**: <哪些模块调用本模块>
- **关联测试**: [tests/unit/test_<module>.cpp](../../tests/unit/test_<module>.cpp)
- **架构总览**: [ammalloc_design.md](ammalloc_design.md)<对应章节>

## 1. 背景与目标

- 为什么存在、服务谁（上游请求者）。
- 量化目标：延迟 / 吞吐 / 碎片率 / 内存占用等（与性能基线一致）。

## 2. 职责与边界

- 与其他模块的接口契约：本模块提供什么、向谁请求什么。
- 所有权模型：谁持有 Span / 对象，谁负责释放，生命周期归属。

## 3. 关键数据结构

| 成员 | 含义 | 同步机制 / 备注 |
|---|---|---|
| <member> | <含义> | <mutex / atomic / TLS / 对齐要求> |

- 标注伪共享防护（缓存行对齐）、布局动机（缓存局部性）。

## 4. 并发模型

- 锁：类型 / 粒度 / 加锁顺序（若涉及多锁）。
- 内存序：逐个列出 `std::atomic` 操作的语义与 release/acquire 配对关系。
- 线程局部性：TLS 使用范围。
- 生命周期与析构顺序：模块间 teardown 依赖。

## 5. 接口定义

| 接口 | 签名 | 语义要点 | Hot path |
|---|---|---|---|
| <Name> | <signature> | <@pre/@return/@note 要点，与头文件 Doxygen 一致> | ✅/❌ |

## 6. 算法与流程

- 核心流程（分配 / 释放 / 回收 / 批量流转）时序图（mermaid 或 ASCII）。
- 复杂度声明，禁止热路径隐藏 O(N²)。

## 7. 边界条件与错误处理

- size=0、nullptr、对齐边界、整数溢出、系统调用失败、资源耗尽。

## 8. 风险与权衡

- 已知限制、性能 vs 复杂度取舍、被否定的替代方案（关联 ADR）。

## 9. 测试要点

- 关联测试套件与关键用例名（对应 [tests/unit/test_<module>.cpp](../../tests/unit/test_<module>.cpp)）。

## 10. 变更记录

| 日期 | 变更 | 原因 | 关联 PR / ADR |
|---|---|---|---|
| YYYY-MM-DD | <变更摘要> | <原因> | <PR/ADR 链接> |
