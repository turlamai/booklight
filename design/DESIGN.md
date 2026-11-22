# Booklight DESIGN.md — 视觉与交互契约（单一真源）

原型（`design/prototype/`）与三端实现（`macos-host/`、`windows/`、`linux/`）都遵循本文件；改视觉先改这里，再同步实现。
本文件承接并具体化 `docs/DESIGN.md`（视觉系统）与 `docs/UX-BENCHMARKS.md`（file-first 基线）；tokens 逐值取自 `macos-host/Sources/RivetHost/Theme.swift` 与 Linux/Windows EPUB 阅读 CSS（`linux/src/epub.cpp`、`windows/MainWindow.Epub.cpp`，两端逐字节一致），保证原型 → 原生实现零翻译成本。宿主用系统语义色（separator、secondaryLabel）之处，原型给出近似值并标注「推导」。

## Brief（2026-11-11）

- **目标**：非从零重设计——纸感画布、单一 terracotta accent、双平面层级已在 `docs/DESIGN.md` 定型。本契约做三件事：把 tokens 钉死到实现值（零翻译成本）；为**研究工作台**（跨书检索 + 可溯源引用）这个新的第二屏定信息架构；把批注与 AI 的进行态、失败恢复态、可回溯性钉死在原型层。
- **组合定位**：现有阅读器（PDF/EPUB 阅读 + 批注 + 轻编辑）免费；研究工作台为付费层新能力。阅读体验（正文排版、批注）是英雄屏，研究/检索是第二屏。（2026-11-18 更新：owner 决策取消商业化，研究工作台作为免费能力三端交付，见 #57 与 AGENTS.md；「付费层」定位仅为 Brief 历史记录。）
- **原型范围**（6 屏 × 亮/暗）：① 欢迎与最近打开（含空态/拖放态）② EPUB 阅读正文排版 + 划词批注（英雄屏，含保存进行态/失败恢复态）③ PDF 双页阅读 + 页面编辑（含未保存修改提示）④ AI 助手 inspector（含发送前核对、思考中态）⑤ 研究工作台（付费层，含检索进行态/无结果/空文档态）⑥ 设置。
- **Exit 条件**：6 屏关键状态齐（空态/进行态/失败恢复态）+ 交互逻辑定稿 + HIG 清单全过 + 用户评审通过。
- **不在范围**：付费/授权/checkout/entitlement UI——2026-11-18 owner 决策（AGENTS.md「quality-first」，#57）已取消全部商业化 gating，研究工作台免费交付；PRICING 文档已退役删除。书库/封面/阅读时长统计、路由与 `rivet-schema.json` RPC 契约变更、新配色体系。
- **已同步（2026-11-19）**：正文排版三端统一为 17 px × 用户缩放 × 1.9 行高（原型值）；Windows/Linux 原 16 px 已对齐，`docs/DESIGN.md` 的 Reading body 行已同步为同一契约。
- **已同步（2026-11-21，#75）**：owner 决策 UI 线 rivet → glaze（三原生腿删除、
  单一 web UI）；PDF 阅读面迁 pdf.js（**仅渲染**：canvas + text layer + 选区/批注
  叠层；页级手术仍归领域核心 `racket/booklight/pdfdoc.rkt`，编辑后字节重渲染）。
  家族设计真源修订 `2026-11-20-minimal-family` 已核验；产品起始 SHA
  `93341981d2637340d645f3a366b0a6302ad30ffa`。EPUB CSS 契约与阅读主题 Keep；
  不变式 2（PDF 文字永不被产品重排）由 pdf.js 保留页面自身排版继续满足。

## 设计原则（HIG 内核 + 本产品特化）

- **文档即界面**：chrome 退后，纸、字与选区承载层级；无持久装饰、渐变与玻璃。正文排版优先于一切装饰——版心、行高、serif 栈是产品资产，不许被界面改动挤压。
- **纸感优先**：画布是暖纸而非纯白；暗态页底 `#171412` 与 EPUB 页同值，翻页时页边不闪光。亮暗成对设计，不存在只有暗态或只有亮态的界面。
- **一个 accent**：terracotta 只给产品标记、主操作与选中导航；红只表失败，绿只表恢复。高亮三色（黄/蓝/绿）是**内容色**（用户划在正文上的墨），不是 UI 色，不得用于按钮或导航。
- **双平面，不是一张纸**：阅读画布（`bg`）与导航面（`sidebar`）色调分开；层级靠平面与 hairline，不靠阴影堆叠。
- **保存状态永不静默**：批注写盘有 进行中 / 已恢复待写入 / 失败 三态，失败必给「重试保存 / 导出批注…」出口；PDF 页级修改只存在于内存，另存提示必须出现，永不静默覆写原件。
- **AI 永不过界**：任何发送前必核对（操作 / 范围 / 精确字符数），只有「继续」才发请求；进行中可停止；回答里的引用可点回原文位置。
- **键盘与可达性是一等**：每个列表支持方向键遍历，每个图标控件有标签与帮助文本；⌘O / ⌘F / ⌘B / ⌘P / ⌘W / ⌘⇧S 语义三平台一致。

## Tokens（语义与推导依据；值不在此复制）

值类 token 真源 = 宿主实现（`macos-host/Sources/RivetHost/Theme.swift` 与 EPUB 阅读 CSS `linux/src/epub.cpp`、`windows/MainWindow.Epub.cpp`，两端逐字节一致）；原型 `design/prototype/shared.css` 与实现值一一对应，本表只记语义与推导依据。宿主用系统语义色（separator、secondaryLabel）之处，原型取近似值并标注「推导」。

| Token | 语义 | 来源 |
|---|---|---|
| `--bg --sidebar --panel --panel-alt` | 纸感画布 / 导航面 / 分组行与浮层（双平面层级） | 实现值 |
| `--ink --mute` | 主/次级文字 | 实现值（`--mute` 推导，需 ≥4.5:1） |
| `--line` | hairline 分隔 | 推导（宿主 separator @70% / hairline） |
| `--accent --accent-hover --accent-soft` | terracotta 品牌标记/主操作/选中导航/进行态 | 实现值 |
| `--danger --success` | 失败 / 恢复成功 | 推导（宿主 systemRed/systemGreen 暖化） |
| 高亮三色（黄/蓝/绿） | 用户批注墨色（`Theme.annotationColor`），是内容色不是 UI 色，检索命中共用黄 | 实现值 |

- **字体**：UI 用系统栈 `system-ui, -apple-system, "Segoe UI", "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei"`；**阅读正文** serif 栈 `Georgia, "Source Han Serif SC", "Noto Serif SC", "Songti SC", serif`，17 px × 用户缩放、行高 1.9、版心 ≈608 px 居中、两端对齐。PDF 页保留自身排版，永不被产品重排。
- **圆角**：控件 6 · 分组行 10 · 大面板/欢迎标记 14。**间距**：4/8/12/16/24/32。**动效**：120–180 ms ease-out，无弹跳无视差。
- **阅读主题**：纸张（`--bg` 本色）/ 深纸（`#EFE6D5`）/ 夜间（`#171412`）× 跟随应用——与窗口亮暗正交，是 EPUB 页自己的主题轴。
- **语言**：zh 默认，文案单源 `shared/i18n/{zh,en}.json`；界面文案只用词表现有项，新概念先补词表再上屏。

## 组件词汇

`WelcomePane`（品牌句 + 打开文档… + 拖放提示 + 隐私句）· `RecentRow`（类型图标/文件名/缩略路径/第 X 页 resume/chevron）· `SidebarViewSwitcher`（缩略图/目录/批注/表单 四模式）· `ReaderCanvas`（PDF 页面组 / EPUB reflow 版心）· `SelectionBar`（划词浮条：三色高亮/下划线/删除线/复制/AI 助手）· `AnnotationRow`（色点/页或章/摘句/笔记/溢出菜单：翻译·删除）· `SaveStateBanner`（进行中 spinner / 失败 + 重试保存 + 导出批注… / 已恢复待写入）· `PdfToolbar`（单页/双页/实际大小/适应宽度/缩放）· `PageEditMenu`（旋转本页/删除本页/插空白页/提取本页…/合并 PDF…/烘焙文字… + 操作对象：第 X 页）· `PendingEditBar`（PDF 修改尚未保存 + 另存为… / 放弃更改）· `AiInspector`（对话/总结/翻译 三 tab + 上下文范围 选区/当前页/全文）· `SendPreviewSheet`（核对即将发送的内容：操作/范围/各项字符数 + 取消/继续）· `ThinkingState`（思考中… + 停止）· `AiCiteChip`（回答内引用：摘句 + 《文档》第 X 页，可点回跳）· `ResearchScope`（检索范围：打开的文档/最近文档）· `ResultCard`（摘录 + 命中高亮 + 源头 chip + 跳转原文/复制引用/加入引用集）· `CitationDeck`（引用集：引文 + 源 + 笔记，可回溯）· `SettingsGroup`。

研究工作台已三端实现（免费交付）：原型期暂定键如下，实现键已按词表先行流程补入 `shared/i18n/{zh,en}.json`：`ui.research.title` 研究工作台 · `ui.research.scope` 检索范围 · `ui.research.opened` 打开的文档 · `ui.research.searching` 正在检索… · `ui.research.no-docs` 还没有可检索的文档 · `ui.research.jump` 跳转原文 · `ui.research.cite` 复制引用 · `ui.research.pin` 加入引用集 · `ui.research.deck` 引用集。`ui.research.pro` 付费层键按 2026-11-18 免费决策废弃：不上屏、未入词表。

## 不变式

1. `accent` 只给品牌标记、主操作、选中导航与进行态；高亮三色是内容墨色，红绿只表失败与恢复；任何界面不允许大面积 accent 色块。
2. 正文排版是产品资产：版心 ≈608 px、行高 1.9、serif 栈、两端对齐不许被界面挤压或替代；PDF 文字永不被产品重排。
3. 亮暗成对、双平面：`bg` / `sidebar` / `panel` 三层表面封顶，层级靠平面与 hairline，不用阴影堆叠；暗态页底恒为 `#171412`。
4. 一切写盘与外发动作有进行态且可回溯：批注保存三态可见、失败有出口；AI 发送前必核对、进行中可停止、引用可点回原文；引用集每条可跳回源头页/章。
5. 文案只能来自 `shared/i18n` 词表；新概念（如研究工作台）先补词表再上屏，平台副本逐字节一致。


## 平台映射与差异声明（预钉 · 宿主实现时升级为落点表）

统一层（三平台必须一致，`design/prototype/` 原型已锁定）：信息架构、核心流程、快捷键语义、
语义色与层级词汇。因地制宜层（按平台惯例落地，不预画像素）：

| 维度 | macOS | Windows | Linux |
|---|---|---|---|
| 修饰键 | ⌘ | Ctrl | Ctrl |
| 命令入口 | 原生菜单栏 | 菜单栏/命令行内 | 菜单栏 |
| 字体 | 系统栈 + PingFang SC | 系统栈 + Microsoft YaHei | 系统栈 + Noto Sans CJK |
| 原生控件 | SwiftUI 控件 | WinUI 3 控件 | GTK4 控件 |

差异声明纪律：宿主特例必须先写进本节才能实现；未声明的宿主差异按违约处理
（案例：taskly 漂移台账 D8）。

## 移动端视图（iOS，2026-11-22）

**定线声明（owner 2026-11-22）**：桌面 = rivet（macOS/Windows/Linux 三腿已落地）；移动端 = 原生
Swift iOS app（等 rivet 移动宿主定型后接入引擎绑定；绑定/同步架构随实现立项另定，本节与
`prototype/mobile-*.html` 锁定界面与交互契约）。研究工作台（第 6 屏）为桌面专属，移动端不承载。

**契约**：`prototype/mobile-{library,reading,pdf,ai,settings}.html` 五屏；纯 390px 内容画布居中，
不自绘手机外壳/状态栏；触控目标 ≥44×44；正文对比 ≥4.5:1（sepia 同验）。阅读排版契约在窄屏不变：
serif 栈、行高 1.7–1.9、字号 16–26 可调、版心随屏宽自适应。阅读主题三档（light/sepia/dark）与
应用亮暗主题独立并存，与桌面语义一致。划词浮条/高亮/下划线/AiCiteChip/引用回跳语义与桌面一致，
AI 面板 sheet 化（摘要/对话、发送前核对、进行中可停、草稿保留）。PDF 屏保留页码/旋转与
PendingEditBar（未保存编辑横幅 + 放弃需确认）语义。书库屏为移动端新入口：桌面开书靠拖放，
移动端无拖放，书库列表（最近在读 + 进度）是唯一开书路径；空态引导按欢迎页语义裁剪。

**Exit 条件**：移动关键屏齐（书库/EPUB 阅读/PDF/AI/设置）；导航返回与状态保持（阅读位置、主题、
字号、标注草稿）逻辑定稿；三主题真实可用；零外部依赖、相对引用无悬空；44 命中与家族 UI 清单
适用项过检。原生 iOS 宿主验证随实现切片另行记录。
