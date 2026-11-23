# AGENTS.md

## 家族设计（必读入口）

- 设计/UI 任务先加载 `tlai-product`，必读 `references/policy.md` 与 `references/design-standard.md`（理念、决策规则、UI 验收）。技能缺失时读取 ZCode 真源：macOS/Linux `~/Sync/ZCode/skills/tlai-product/`；Windows `D:/ZCode/skills/tlai-product/`。无法读取须报告规范未核验，不凭记忆跳过。
- 家族规范管共性；本仓现有设计、行为与 token 真源管具体值和产品差异。例外记回原契约，不复制家族规则或新增数值表；已有界面按下一次相关切片渐进对齐。
- 改界面先改原型、落账后实现；按家族 UI 清单与本仓当前技术线的既有门禁验收，原型对照和真实交互证据都要保留；对外宣传使用验收后的实物截图。

## 实现路由（agent 按任务读真源）

- 实现界面以 `design/prototype/` 为准：对照 HTML/CSS/JS 与实物截图验收，不依赖散文复述。
- Token/几何值真源 = `design/prototype/shared.css`（亮暗成对；无 shared.css 的仓 = design/prototype/ 各页样式）；`design/DESIGN.md` 只记语义名、用途与决策。
- 行为边界与跨平台一致性以 `design/DESIGN.md` 的不变式与平台差异节为准。
- 改界面先改原型再改实现（tlai-product）；动共享层（design/、shared/）先 issue 声明，同 commit 落契约。

## Current owner decision — 2026-11-18 quality-first

This is a **non-commercial development product**. The previous 2026-10 authorization, pricing, trial, Pro and Permit/Commerce rollout decisions are canceled. Do not implement or restore checkout, activation, subscription, paid feature gates, trial expiry or update-entitlement windows. Keep source licenses, release-signature verification, privacy protections and tested data migration intact. Focus only on real product functionality, native platform parity, recovery, clear bilingual READMEs and honest website status. Read the [current family policy](https://github.com/turlamai/turlamai.github.io/blob/master/PORTFOLIO.md) before every development slice.

指引给 AI agent（及开发者）：如何理解、构建、运行、改动 Booklight。

> UI 线已锁定 glaze（owner 2026-11-23），#75 S6 已完成换线：三平台原生宿主
> （macOS SwiftUI / Windows WinUI3 / Linux GTK4）已退役，单一 web UI 跑在
> glaze 载体（系统 WebView）上，版本线自 0.1.0 重启。
> v1 是已归档的 Tauri 2 + PDF.js 实现，仅存于 git 历史；
> v1 的行为/文案语义已由领域核心承接。

## 这是什么

Booklight 的领域核心是 Racket，经类型化 RPC（RVT1）对外；UI 是单一 web UI
（`host/web/`），跑在 glaze 载体（系统 WebView + 内嵌 Racket CS runtime）上，
三平台同一份界面与交互。

| 平台 | 技术栈 | 入口 | 状态 |
|---|---|---|---|
| macOS / Windows / Linux | glaze 载体（系统 WebView）+ 单一 web UI | `host/main.rkt` | 可跑（阅读/AI/批注/页编辑/研究工作台全量） |
| agent CLI | — | — | 不适用 |

## 快速命令

```bash
# 前置：Racket CS 9.x，glaze 以 link 方式安装（rivet 仍为领域核心测试依赖）
# cd ../glaze && raco pkg install --auto --no-docs --name glaze --link file://$PWD

raco glaze schema check glaze-schema.json --json # RPC 兼容性门禁
raco glaze dev             # 开发循环（载体 + web UI）
raco test racket/          # 领域核心测试（项目测试）
raco test racket/tests/backend-test.rkt # 真实 RVT1 wire test
racket host/verify.rkt     # 真实窗口验收（boot/e2e/s2–s5/drop 像素证据）
```

## 完成定义

- `racket host/verify.rkt` 是产品完成门禁：后端契约、真实 RVT1、载体打包、
  真实窗口操作与像素证据必须全部通过（report 落 `.glaze/verify/`）。
- GUI 工作不能只凭代码审查、单元测试、构建成功或截图宣称完成。默认验收会
  用打包后的应用打开 `racket/tests/fixtures/sample.epub`，并确认后端解析出的
  `The Sample Book` 与 `First Chapter` 出现在真实窗口。
- 修改公共交互只有一份 web UI，三平台天然一致；不得把业务逻辑复制到 UI 绕开
  RVT1。
- 关键控件的 accessibility / automation 标识属于测试契约。修改布局或文案时
  不得无意删除；新增关键流程时同步扩展 `host/verify.rkt` 的 journey。
- 不得用 `continue-on-error`、空测试、跳过 UI 或强杀进程作为成功证据。

## 契约（不要破坏）

- **文件优先数据契约**：打开原路径；新安装默认写 <file>.booklight.json 批注 sidecar，旧安装尊重已有的 central/sidecar 选择；settings.json 仅保存必要偏好与最近文件；不会创建 library.json/covers 或自动复制旧配置树。详见 shared/spec/DATA-FORMAT.md
- **i18n 单源**：`shared/i18n/{zh,en}.json`，web UI 直接消费单源；zh 为默认
- **RPC 面**：`app/backend.rkt` 的 define-rpc 是唯一数据通道，`host/main.rkt`
  的 typed routes 是载体接线，`glaze-schema.json` 是基线；改签名 = 后端 +
  载体路由 + 基线同步改（`raco glaze schema check` 门禁）
- **宿主只做渲染与交互**：业务一律走 RPC；定时器/调度/HTTP/存储/PDF 手术都在 Racket 后端
- **PDF 页级操作走领域核心**：`racket/booklight/pdfdoc.rkt` 是自研最小读写器，不引第三方 PDF 库；编辑全在内存，宿主负责落盘
- **BYOK 密钥**：走 `racket/booklight/secure-store.rkt`（macOS Keychain / Linux Secret Service / Windows DPAPI，失败时仅本人可读文件回退）；不得写入 `settings.json`，错误与诊断必须经 `llm.rkt` 的 `redact-secret` 脱敏
- **损坏输入预算**：EPUB 与 PDF 都必须在领域核心内先校验资源预算（`EPUB_RESOURCE_LIMIT[...]` / `PDF_RESOURCE_LIMIT[...]`），失败即报错，不得先分配再失败
- **视觉契约**：`design/DESIGN.md` 是设计系统的单一说明；三平台共享同一份
  web UI（`host/web/`）与 `design/prototype/shared.css` token，不得各做一套交互
- **上游改动走上游**：缺能力先提 issue/PR 到 turlamai/glaze（载体）或
  turlamai/rivet（领域核心依赖），不本地 hack

## 项目结构

```
├── VERSION             发布版本（与 app/version.rkt / CHANGELOG 对齐）
├── glaze.rktd          glaze 应用清单（schema-module/binding/baseline）
├── glaze-schema.json   RPC 面基线
├── app/backend.rkt     类型化 RPC 后端入口（装配领域层）
├── app/updater.rkt     在线更新（签名 manifest 校验 + 下载 mechanics）
├── host/main.rkt       glaze 载体入口（typed routes + 运行时装配 public/）
├── host/web/           单一 web UI（index.html / app.css / app.js）
├── host/verify.rkt     真实窗口验收（boot/e2e/s2–s5/drop 像素证据）
├── racket/             Racket 领域核心（含 pdfdoc PDF 读写器）+ tests/
├── shared/i18n/        zh.json / en.json 单源
├── design/             图标与品牌源文件 + prototype/（token 真源）
├── docs/               产品主页（GitHub Pages）+ USER-GUIDE
├── flatpak/            Linux Flatpak bundle manifest + wrapper + desktop
└── scripts/            release/qualification 工具（make-update-manifest / check-release-version / locate-zlib）
```

## 产品优先级与交付

- **P0**：PDF/EPUB 在原位打开；三平台同一 web UI 的欢迎页、拖放、最近文件、多文档、关闭/重开。
- **P0**：批注保存/重试/恢复、PDF 轻编辑与明确导出，错误时不覆写原件。对只读文件和不存在路径给出真实错误。
- **P1**：AI 发送范围可预览/取消，密钥安全、无隐式网络上传。
- **P1**：所有平台真实打包验收；测试和文档与 file-first 设计一致。
- 研究工作台保持可用的非收费功能方向；以现有功能 Issue 与实际验收为准。
- 旧 `library-*` RPC 已在 0.12.0 移除（三端客户端同步与打包验收完成后执行，issue #63）；旧客户端调用只会收到 unknown RPC 错误，不会重建任何书库数据。旧 `library.json` 与用户文件不可自动删除。
- [Issue #20](https://github.com/turlamai/booklight/issues/20) 继续承载批注与 AI 的保存/隐私质量问题；此 PR 的方向与旧“书架保留”决策冲突时，以 2026-10-29 文件优先决策为准。

## Documentation languages

Public default documentation is English; maintain complete Chinese user paths in `*.zh-CN.md`, with reciprocal links. Update both languages when prices, quotas, installation, recovery, privacy or supported platforms change. API/protocol identifiers and commands remain unchanged; do not maintain a second normative protocol or implementation. Follow the [family language policy](https://github.com/turlamai/turlamai.github.io/blob/master/docs/DOCUMENTATION-LANGUAGES.md). Existing internal notes are not a claim that every document is bilingual.

## Public product documentation

The paired `docs/USER-GUIDE.md` / `docs/USER-GUIDE.zh-CN.md` own the public installation, first-use, data/recovery, privacy and availability path. Keep the homepage and both README entry points aligned with them when behavior changes. Detailed technical contracts and the current implementation queue retain their existing owners. Do not publish target prices, a purchase link or a ready/secure/platform claim before its release evidence exists.
