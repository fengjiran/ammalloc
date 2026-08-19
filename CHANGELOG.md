# Changelog

本文件记录 ammalloc 所有用户可见的行为变更，格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。机制层面的细节变更记录在 [docs/designs/](docs/designs/) 对应模块文档中，本文件只收录行为可见的变更。

## [Unreleased]

### Added

- 建立文档系统：新增 [docs/README.md](docs/README.md)（索引与术语表）、[docs/guides/documentation-guide.md](docs/guides/documentation-guide.md)（文档系统规范）、[docs/api/public-api.md](docs/api/public-api.md)（API 参考）、[docs/decisions/](docs/decisions/)（ADR）与 [docs/templates/](docs/templates/)（模板）。
- 文档目录重组：设计文档统一为 `docs/designs/NN-*.md` 编号命名；演进提案迁移至 `docs/improvement-plan/`；问题跟踪迁移至 `docs/issues.md`；调研备忘迁移至 `docs/designs/research/`。
