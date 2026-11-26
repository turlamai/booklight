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
- **已锁定（owner 2026-11-23）**：UI 线 rivet → glaze，不再评估回 rivet。当前 main 仍为
  rivet 三腿（0.12.0），按 #75 S1→S6 推进到单一 web UI。PDF 阅读面迁 pdf.js（**仅渲染**：canvas + text layer + 选区/批注
  叠层；页级手术仍归领域核心 `racket/booklight/pdfdoc.rkt`，编辑后字节重渲染）。
  家族设计真源修订 `2026-11-20-minimal-family` 已核验；产品起始 SHA
  `93341981d2637340d645f3a366b0a6302ad30ffa`。EPUB CSS 契约与阅读主题 Keep；
  不变式 2（PDF 文字永不被产品重排）由 pdf.js 保留页面自身排版继续满足。
- **已同步（2026-11-23，#75 S1）**：glaze 载体落地 `host/`（34 RPC 1:1 typed
  路由，rivet/bridge 进程内 RVT1 + schema codec，上游提取 turlamai/rivet#493 /
  glaze#159）；边界约定 Bytes=base64 字符串、enum=字符串、章节序号 1-based；
  `glaze-schema.json` 为 glaze 面基线，`scripts/check-rpc-surface.py` 门禁；
  S1 探针页仅 harness，产品 web UI 自 S2 起按原型渐进落地。
- **已同步（2026-11-23，#75 S2）**：产品 web UI 落地 `host/web/`（library/
  welcome + 文件优先 recents、reader chrome：标题栏/文档标签/侧栏目录·批注
  tabs/状态栏、EPUB 面：toc/章节正文/章导航/阅读主题+字号）。token 单一真源
  `design/prototype/shared.css` 由载体运行时装配 serve（不复制进仓）；正文
  排版对照 `reading.html`/.measure、library 对照 `welcome.html`/.rrow；
  文档标签条先落原型 `reading.html` 再实现。字符串走 `shared/i18n` 单源
  （`api/host/i18n`，zh 默认）；原生打开面板走 glaze `make-dialog-routes`；
  只读渲染字节 `api/host/file` 受命名 capability 约束。PDF 仅 pdf.js 只读
  渲染（S0 已验证），页手术与划词/待编辑条留 S3。真窗验收含 S1 探针 + 13 步
  S2 产品 journey。
- **已同步（2026-11-23，#75 S3）**：PDF 生产面落地——pdf.js text layer + 原生
  划词映射到 PDF 用户空间 rect（points、左下原点、未旋转，与 macOS PDFKit
  同空间）；SelectionBar（三色高亮/下划线/删除线/复制）经 kept 批注存储持久化
  （sidecar/central + recovery 重试）；PendingEditBar 页手术预览（旋转/删除/
  插空白/合并/水印）+ 撤销/放弃/显式另存，原件不被隐式覆写；extract 走另存
  导出。预览链经载体托管 scratch 文件串接 edit-* RPC，页手术唯一实现仍在
  `racket/booklight/pdfdoc.rkt`。原子写 `api/host/write` + 原生另存面板。
  划词浮条对照 `reading.html` .selbar、待编辑条对照 `pdf.html` notice（原型
  已有，无需改共享层）。真窗验收 +8 步 S3 journey + 定格像素证据。
- **已同步（2026-11-23，#75 S4）**：设置/研究工作台/AI inspector 落地——
  设置对照 `settings.html` 原生分组 Form（服务商预设/Base URL/模型+建议/
  API Key/测试连接/阅读主题/页面显示/目标语言/界面语言/批注 sidecar）；
  研究工作台对照 `research.html`（跨打开文档原位检索、命中卡 mark+溯源、
  跳转/复制引用/加入引用集，引用集以 `cite` 批注随文档保存）；AI inspector
  对照 `ai.html`（对话/总结/翻译 tabs、选区/当前页/全文 scope、发送前核对
  sheet 只有「继续」才发请求、流式回答+停止）。字符串复用 kept i18n 键
  （含 `ui.ai-preview.body` 整段核对文案），无新增键、无共享层改动。
  真窗验收 +8 步 S4 journey（29 步）+ 研究工作台定格像素证据。
- **已同步（2026-11-23，#75 S5）**：阅读质量像素证据——真窗定格 stage 逐项
  断言正文排版等于 kept 契约：版心 608px/上padding 40px、Georgia+Source Han
  Serif 栈 17px/1.9 两端对齐、citeblock 15px/1.8+3px 竖线 .88、paper/sepia/
  night 底色与墨色等于 shared.css reading tokens、字号千分比落到 computed
  px、高亮墨等于 --hl-yellow 且 opacity .45、下划线墨等于 --accent、逐行
  选区→墨 IoU≥0.8、跨章版心不漂移且无横向溢出、平滑滚动手感；终态留 sepia
  正文供 capture（booklight-host-s5.png）。welcome drop-mask 落地（ui.drop/
  ui.welcome.drop-open，拖入字节经 scratch 暂存后打开），定格像素证据
  booklight-host-drop.png。排版资产未被界面改动挤压（设计原则「文档即界面」
  的验收面）。

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

### 移动端视图（iOS，2026-11-22）

**桌面定线（owner 2026-11-23 锁死）**：桌面 = **glaze**。当前 main 仍为 rivet 三腿
（0.12.0），按 #75 S1→S6 推进到单一 web UI；不再评估回 rivet。PDF 面按 pdf.js 质量门禁
验收（长文档滚动/选区/内存/输入延迟），不过线只在该 surface 做原生例外，不回退整条线。
移动端按 owner 2026-11-23 确认走平台原生语言（iOS/iPadOS Swift；Android 需要时 Kotlin），
不复用 rivet 移动宿主，也不把 glaze 载体延伸到移动端。
原型包含移动端视图，界面与交互以本节和 `prototype/mobile-*.html` 为准。研究工作台
（第 6 屏）为桌面专属。数据同步边界随实现立项另定。

**契约**：`prototype/mobile-{library,reading,pdf,ai,settings}.html` 五屏；纯 390px 内容画布居中，
不自绘手机外壳/状态栏；触控目标 ≥44×44；正文对比 ≥4.5:1（sepia 同验）。阅读排版契约在窄屏不变：
serif 栈、1.9 行高、两端对齐、字号 16–26 可调、版心随屏宽自适应。阅读主题三档（light/sepia/dark）与
应用亮暗主题独立并存，与桌面语义一致。划词浮条/高亮/下划线/AiCiteChip/引用回跳语义与桌面一致，
AI 面板 sheet 化（摘要/对话、发送前核对、进行中可停、草稿保留）。PDF 屏保留页码/旋转与
PendingEditBar（未保存编辑横幅 + 放弃需确认）语义。书库屏为移动端新入口：桌面开书靠拖放，
移动端无拖放，书库列表（最近在读 + 进度）是唯一开书路径；空态引导按欢迎页语义裁剪。

**Exit 条件**：移动关键屏齐（书库/EPUB 阅读/PDF/AI/设置）；导航返回与状态保持（阅读位置、主题、
字号、标注草稿）逻辑定稿；三主题真实可用；零外部依赖、相对引用无悬空；44 命中与家族 UI 清单
适用项过检。原生 iOS 宿主验证随实现切片另行记录。

## 2026-11-26 原型对齐家族基线（`2026-11-20-minimal-family`）

起始 SHA `d2c86b781cb0482e8220b21663751318411be274`。只动 `design/`：token 值、强调色、信息架构、示例数据与产品行为不变；产品仍只读取 `shared.css`。

原型分层（自本次起）：`design/index.html` 是本地评审用导航页（按 `<title>` 列 12 页，链到本文件），不提交；`prototype/shared.css` 与产品共用的 token 与基础组件；`prototype/prototype.css` 只由桌面原型加载的层（reduced motion、可见焦点、交互式 app 外壳 `agent.html` + `app.js`，此前该页没有任何样式表）；`prototype/theme.js` 静态页与导航页共用的主题切换（`app.js` 保留自己的状态切换）。

**重建声明（待 owner 过目）**：`prototype/prototype.css` 的 app 外壳段是 2026-11-26 新写的重建，不是原始设计。仓库历史、产品源码（`host/web/app.css` 只覆盖壳类名的 12%，产品标记是另写的）与本机 OpenDesign 项目都没有这份样式表。重建只沿用现有 token 与静态页的几何，单独成文件，可整体删除。owner 接受之前，它只用于让 `agent.html` 可读，不作为实现契约。

| 检查 | 结果 | 证据 / 限制 |
|---|---|---|
| R1 主题切换 | 通过 | 6 个静态页、导航页与 app 顶栏：`◐` 文字字形 → 内联 SVG 线性图标（16px / 1.7），图标 = 当前主题，`aria-label` 与 `title` = 点击后切到的主题（切换深色主题 / 切换浅色主题）；`.iconbtn` 改为安静图标按钮（28、透明底与边、hover 表面色）。`pdf.html` 的按钮缺 `id`，脚本报错并使同页其余脚本失效，已修 |
| R2 12px 字号底线 | 通过，无例外 | 源扫描 sub-12px 声明 23 → 0；渲染 `small` 桌面 81→0、移动 10→0（每主题）。控件高度 ≥28（分段按钮、横幅与结果卡按钮、演示控件）；层级保持 12 / 13 / 14；`.m-cover` 放宽到 64×86、`.ficon` 38×46、设置「语言」下拉 150px 以容纳 12px |
| R3 reduced motion 与焦点 | 通过 | `prototype.css`：家族标准片段 + `:focus-visible` 2px `--accent`；移动页沿用既有 `.mobile` 规则。未进 `shared.css`：产品的 `host/web/app.js` journey 无条件断言平滑滚动 |
| R4 原型完整性 | 通过 | `design/index.html` 是本地评审用导航页，不提交（移动页的 `../index.html` 指向它，克隆后由 agent 本地生成）；本地 `href`/`src` 全部可解析；无远程资源。`agent.html` 亮暗均可读（暗色正文对比此前不可见）；`.dark mark` 深色墨保证暗色检索命中可读 |
| R5 可访问名称 | 通过 | 渲染 `nameless` 0；静态页图标按钮（侧栏、书签、缩放、关闭标签页）补 `aria-label`，装饰 SVG `aria-hidden` |
| R6 版式缺陷 | 通过（见开放项） | 1280×800：`overflowX` 0、`err`/`fail` 0。修正 `reading.html` 划词浮条压住所选段落首行、`research.html` 引用集出处「第 / 14 页」断行、演示控件遮挡 `ai.html` 发送按钮。app 外壳 6 屏 × 2 主题 × 1280 / 1024 / 768 无横向溢出与裁切 |
| R7 契约同步 | 通过 | 本节；token 表只记语义，无值变更 |
| R8 仓库门禁 | 通过（2 项未跑通，均与 `design/` 无关） | `node scripts/check-file-first.mjs`、`bash scripts/check-release-version.sh` 通过；`scripts/tests/test_release_signing.py` 在本机因系统 OpenSSL 不支持 ED25519 无法起跑（基线导出同样失败）；`racket host/verify.rkt`（真窗，运行时读取 `shared.css`）本次未跑，其断言的阅读排版规则本次未改 |

记录的例外：无。

仍开放（需产品决定，未改）：`--accent` 文字与主按钮白字对比 2.4–4.1:1（token 级，各页既有）；`research.html` 标题与「付费层」胶囊、`AGENT-INTERFACE.md` 的商业化表述已过期；`app.js` 阅读默认值 18px / 640px 与本契约 17px / 608px 不一致；静态页是固定 1180px 窗口示意，窄视口横向滚动，且视图切换、划词浮条为不可聚焦的 div/span；产品侧待同步（`host/web/app.css` 仍含 sub-12px 规则，`#themeBtn` 仍为 `◐` 文本，尚无 reduced motion / `:focus-visible`）。
