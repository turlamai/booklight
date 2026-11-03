# Booklight

> 打开文件，专心阅读，需要时再编辑。

[English](README.md) · **简体中文**

[发布版本](https://github.com/turlamai/booklight/releases) · [CI](https://github.com/turlamai/booklight/actions/workflows/ci.yml) · [许可证](LICENSE)

Booklight 是 macOS、Windows、Linux 原生 PDF / EPUB 阅读器与轻量 PDF 编辑器。它**不是 iBooks 式书库管理器**。文件留在原来的目录中；不建立应用私有书库、封面仓库或数据库，不记录阅读时长、目标和连续阅读天数。

## 从文件开始

用「文件 > 打开」、拖放或操作系统文件关联打开 PDF / EPUB。无文档窗口仅有清爽的打开入口；最近文件用于找回原路径和上次页码，不承担书籍编目。

- **PDF 阅读**：连续/双页、缩放、目录、缩略图、搜索、打印、书签。
- **EPUB 阅读**：重排章节、排版调节、章节目录、搜索、笔记。
- **批注**：高亮、笔记、书签；保存失败和恢复状态清晰可见。
- **轻量 PDF 编辑**：旋转、插页、删页、提取、合并、表单填写与导出；不静默覆盖原件。
- **可选 AI**：划词翻译、总结、文档对话；由用户选择的服务商处理其明确提交的文档内容。

## 文件始终属于用户

没有「导入书籍」阶段。新安装默认将批注写入原文件旁的 sidecar；已有设置不强行改变。少量偏好和最近路径保存在 `settings.json`，批注保存失败时保留恢复草稿。升级不会自动清理原来的 `library.json` 和封面目录，但它们不再作为产品的书库数据使用。

[文件与恢复约定](shared/spec/DATA-FORMAT.md) · [中文使用指南](docs/USER-GUIDE.zh-CN.md) · [English guide](docs/USER-GUIDE.md)

## 原生架构

| 平台 | 技术栈 |
|---|---|
| macOS | SwiftUI、PDFKit |
| Windows | WinUI 3、PDFium |
| Linux | GTK4、PDFium、WebKitGTK |

一份 Racket 领域核心经 [Rivet](https://github.com/turlamai/rivet) 类型化 RPC 连接三端原生宿主。书库 RPC 暂作为**无持久化的旧客户端兼容占位**；目前仍在重构，不能把 PR 源码修改当成已发布、已通过三端实测。

```bash
raco rivet doctor --json
raco rivet schema check rivet-schema.json --json
raco test racket/
raco rivet build
raco rivet acceptance --json
```

[文件优先产品决策与验收](docs/FILE-FIRST-DESIGN.md) · [工程指引](AGENTS.md)

## 许可证

[AGPL-3.0](LICENSE)
