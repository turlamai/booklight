# Booklight

> 打开文件，专心阅读，需要时再编辑。

[English](README.md) · **简体中文**

[发布版本](https://github.com/turlamai/booklight/releases) · [CI](https://github.com/turlamai/booklight/actions/workflows/ci.yml) · [许可证](LICENSE)

Booklight 是 macOS、Windows、Linux 上的 PDF / EPUB 阅读器与轻量 PDF 编辑器。它**不是 iBooks 式书库管理器**。文件留在原来的目录中；不建立应用私有书库、封面仓库或数据库，不记录阅读时长、目标和连续阅读天数。

## 从文件开始

用「文件 > 打开」、拖放或操作系统文件关联打开 PDF / EPUB。无文档窗口仅有清爽的打开入口；最近文件用于找回原路径和上次页码，不承担书籍编目。

- **PDF 阅读**：连续/双页、缩放、目录、缩略图、搜索、打印、书签。
- **EPUB 阅读**：重排章节、排版调节、章节目录、搜索、笔记。
- **批注**：高亮、笔记、书签；保存失败和恢复状态清晰可见。
- **轻量 PDF 编辑**：旋转、插页、删页、提取、合并、表单填写与导出；不静默覆盖原件。
- **可选 AI**：划词翻译、总结、文档对话；由用户选择的服务商处理其明确提交的文档内容。
- **跨文档研究**：本地检索打开与最近文件，跳回原文摘句，把可溯源引用钉入引用集。

## 文件始终属于用户

没有「导入书籍」阶段。新安装默认将批注写入原文件旁的 sidecar；已有设置不强行改变。少量偏好和最近路径保存在 `settings.json`，批注保存失败时保留恢复草稿。升级不会自动清理原来的 `library.json` 和封面目录，但它们不再作为产品的书库数据使用。

[文件与恢复约定](shared/spec/DATA-FORMAT.md) · [中文使用指南](docs/USER-GUIDE.zh-CN.md) · [English guide](docs/USER-GUIDE.md)

## 架构

一份 web UI（`host/web/`）跑在 [glaze](https://github.com/turlamai/glaze) 载体
（系统 WebView + 内嵌 Racket CS runtime）上，macOS、Windows、Linux 共用同一界面。
PDF 渲染用 pdf.js；页级手术留在自研 Racket 读写器（`racket/booklight/pdfdoc.rkt`）。

一份 Racket 领域核心经类型化 RPC（RVT1）连接载体。RPC 契约只有一套
（`glaze-schema.json`）：旧书库端点已移除，载体使用当前 schema。

```bash
raco glaze schema check glaze-schema.json --json
raco test racket/
raco glaze dev
racket host/verify.rkt
```

[文件优先产品决策与验收](docs/FILE-FIRST-DESIGN.md) · [工程指引](AGENTS.md)

## 安装

从 [Releases](https://github.com/turlamai/booklight/releases/latest) 下载。
资产统一按 `booklight-<version>-<os>-<arch>.<ext>` 命名：

| 平台 | 安装器 | 便携 zip（应用内更新） |
|---|---|---|
| macOS 14+（Apple silicon / Intel） | `booklight-<version>-macos-arm64.dmg` · `-x64.dmg` | `booklight-<version>-macos-arm64.zip` · `-x64.zip` |
| Windows 10+ x64 | `booklight-<version>-windows-x64.msi` | `booklight-<version>-windows-x64.zip` |
| Linux x64 / arm64 | `booklight-<version>-linux-<arch>.deb` · `.rpm` · `.AppImage` · `.flatpak`（app id `site.jrtx.booklight`；`flatpak --user install ./booklight-<version>-linux-<arch>.flatpak` 安装） | `booklight-<version>-linux-<arch>.tar.gz` |

每个版本都带校验过的 `SHA256SUMS`；应用内更新消费 Ed25519 签名的
`update-manifest.json` feed（key id `booklight-2026-11`；Sigstore 溯源：
`gh attestation verify <file> -R turlamai/booklight`）。

macOS 发布构建由发布流水线完成 Developer ID 签名与公证（缺少签名凭据时
tag 发布会直接失败）。本地开发构建仅有 ad-hoc 签名：首次启动右键应用选
「打开」即可清除 Gatekeeper 提示。

## 许可证

[AGPL-3.0](LICENSE)
