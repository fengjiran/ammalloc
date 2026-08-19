# ammalloc 文档系统规范

> 本文件定义 ammalloc 文档系统的命名、交叉引用、质量标准与维护流程。所有文档贡献者必须遵循。
>
> 优先级：与 `AGENTS.md` 或已验证的仓库事实冲突时，以 `AGENTS.md` 与仓库事实为准。

## 1. 分类与职责边界

| 类型 | 目录 | 状态字段 |
|---|---|---|
| 架构总览 | `docs/designs/ammalloc_design.md` | Current |
| 模块设计 | `docs/designs/NN-*.md` | Current / Deprecated |
| 调研备忘 | `docs/designs/research/` | 无状态，头部标注日期与结论可信度 |
| 演进提案 | `docs/improvement-plan/` | Draft / In Progress / Implemented / Superseded |
| 开发指南 | `docs/guides/` | Current / Deprecated |
| API 参考 | `docs/api/` | Current |
| 决策记录 | `docs/decisions/` | Proposed / Accepted / Deprecated / Superseded |
| 问题跟踪 | `docs/issues.md` | 每条目 `[x]`/`[ ]` |
| 变更记录 | `CHANGELOG.md`（仓库根） | 无 |

**职责边界判定**：描述"代码里现在是什么" → `designs/`；"将来要做什么" → `improvement-plan/`；"曾经怎么决策的" → `decisions/`；"怎么干活" → `guides/`；"API 怎么用" → `api/`；"已知缺陷/待办" → `issues.md`。

**准入条件**：

- `designs/` 下所有文档只描述**已验证实现**；草稿/提案不得进入，统一放 `improvement-plan/` 或标记为调研。
- `decisions/` 每篇 ADR 必须包含至少 1 个被否定的备选方案。
- 调研备忘不承诺实现，禁止被后续文档当作事实引用（可作参考链接）。

## 2. 命名规则

| 对象 | 规则 | 示例 |
|---|---|---|
| 目录 | kebab-case | `improvement-plan/`、`decisions/` |
| 设计文档 | `NN-<kebab-name>.md`，NN=两位编号（推荐阅读顺序，沿数据流方向） | `04-page-cache.md` |
| ADR | `NNNN-<kebab-title>.md`，四位递增 | `0001-scavenger-startup-strategy.md` |
| 指南/其他文档 | `<kebab-name>.md` | `documentation-guide.md` |
| 章节编号 | 阿拉伯数字层级，作为稳定标识 | `ammalloc_design.md` 第 4 章 |

存量例外：`docs/designs/ammalloc_design.md` 与 `docs/guides/` 4 篇指南保留原名（避免破坏引用），在 `docs/README.md` 索引中标注。

**编号管理**：

- 设计文档编号全局唯一；新增模块设计时取当前最大编号 + 1（除非数据流顺序要求插入，插入时一并重排后续编号并更新引用）。
- ADR 编号只增不减；被否决或废弃的 ADR 改为 Deprecated/Superseded，不回收编号。
- 重命名或删除文档前，必须用 `grep` 校验并更新全部引用（含 `docs/README.md`、`ammalloc_design.md` 附录、`AGENTS.md`）。

## 3. 交叉引用规则

| 方向 | 规则 | 示例 |
|---|---|---|
| 文档 → 文档 | 具名相对链接；跨文档禁止"见上文/见前文"，必须给出文件链接 | `[PageCache 模块设计](../designs/04-page-cache.md)` |
| 同文档内 | 锚点引用 `#章节编号-锚点` | `[并发模型](#4-并发模型)` |
| 章节引用 | `<file>.md#<章节编号-锚点>`，编号是跨 PR/评审/ADR 引用的稳定标识 | `04-page-cache.md#4-并发模型` |
| 文档 → 代码 | 符号名反引号包裹 + 可点击路径链接；接口表与头文件符号完全同名 | ``PageCacheShard::AllocSpan``、`[src/page_cache.cpp](../../src/page_cache.cpp)` |
| 代码 → 文档 | 头文件 file-level 注释追加 `/// @see docs/designs/NN-*.md`；实现文件可用 `// See docs/...` | `/// @see docs/designs/04-page-cache.md` |

**单一事实源**：同一信息只在一个文档详述，其余文档引用链接。已确立的事实源：

- 性能基线：`docs/designs/ammalloc_design.md`
- 构建选项与运行配置：`README.md`
- API 语义：头文件 Doxygen 注释 与 `docs/api/public-api.md` 同源同步
- 术语：`docs/README.md` 术语表

## 4. 质量标准

| 维度 | 标准 | 检查方式 |
|---|---|---|
| 完整性 | 模板章节齐全；设计文档覆盖该模块全部公共头文件符号；issues 条目含背景/方案/状态 | 对照 `docs/templates/` + grep 头文件符号 |
| 准确性 | 只写已验证事实；状态字段正确（不把计划当已实现）；无过期数据 | 抽查代码 + 状态字段检查 |
| 可读性 | 单文档 ≤ 1000 行（超出拆分子节或子文档）；跨层流程用图；表格优先于长段落 | 行数检查 + 人工审读 |
| 一致性 | 术语与术语表一致；命名规范；模板遵循度；链接全部有效 | 脚本检查 + 人工审读 |

## 5. 评审流程

**文档变更 PR 检查清单**（新增或修改文档必过）：

- [ ] 遵循对应模板（`docs/templates/`）
- [ ] 头部状态与元数据（状态/日期/关联代码）正确
- [ ] `docs/README.md` 索引已同步（新增文档必须登记）
- [ ] 全部相对链接有效（可运行 `scripts/verify_docs.py`）
- [ ] 接口符号与头文件一致（grep 验证）
- [ ] 命名符合 §2 规则

**代码变更 PR 检查清单**：按 §6 触发矩阵核对是否命中，命中项必须在本 PR 或显式标注的跟进 PR 中更新对应文档（code_review_guide.md 快速门禁执行）。

**定期审计**：每季度或每次大版本发布前，对照触发矩阵执行漂移检查；过期文档立即更新或标记 Deprecated（不删除，保留历史与链接）。

## 6. 维护与更新流程

### 6.1 变更触发矩阵

| 变更类型 | 必须同步更新的文档 |
|---|---|
| 公共 API 变更（签名/语义） | `ammalloc.h` Doxygen、`api/public-api.md`、README.md API Semantics、CHANGELOG；重大决策另建 ADR |
| 模块内部实现变更 | 对应 `designs/NN-*.md`、`ammalloc_design.md` 相关章节 |
| 新增/删除模块 | `docs/README.md` 索引、`ammalloc_design.md` 附录、新建/删除模块设计文档 |
| 配置变更（环境变量/构建选项） | README.md Runtime Configuration/Build Options、相关模块文档配置章节、CHANGELOG |
| 并发契约/内存序/invariant 变更 | `ammalloc_design.md` 硬性约束节、模块文档并发模型节、ADR（按 improvement-plan README 既有规则联动检查路线图/风险矩阵） |
| 性能基线变化 | `ammalloc_design.md` 性能章节、CHANGELOG |
| 编码/注释/测试规范变更 | 对应 `guides/*` + AGENTS.md 双向同步 |
| 缺陷修复 | `issues.md` 勾选条目、CHANGELOG（行为可见时） |

### 6.2 防过期机制

1. **同 PR 同步原则**：文档更新与代码变更同一 PR 提交；无法同 PR 时必须显式记录跟进任务并关联。
2. **状态字段**：Deprecated/Superseded 明确标记而非删除，保留历史可追溯。
3. **代码事实优先**：文档与代码冲突时，以经过验证的仓库事实为准并立即修正文档。
4. **漂移检测**：`scripts/verify_docs.py` 校验相对链接有效性、设计文档接口符号在头文件中的存在性、索引覆盖完整性。

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.0 | 2026-08-19 | 初始版本：文档系统分类、命名、交叉引用、质量、维护规则成文 |
