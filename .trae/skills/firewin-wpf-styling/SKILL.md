---
name: firewin-wpf-styling
description: WPF styling for FireWin covering brush tokens, control templates, page visibility, list themes, DPI and log controls. Use when changing colors, layouts or controls, not for deployment logic.
---
# FireWin 桌面端样式与视觉规范

桌面端是手写样式的深色 WPF 界面：没有第三方 UI 库、没有资源字典合并、没有自定义依赖属性，全部样式集中在 `App.xaml` 与三个窗口的 `Window.Resources` 里。

## 适用范围

- 调整配色、圆角、间距、字号等视觉参数。
- 新增按钮、列表、进度条、警告条等控件样式。
- 修改步骤指示器、日志面板、进度面板的显示逻辑。
- 排查“禁用状态没反馈”“进度条不动”“选中背景错位”“1px 边框发虚”等样式问题。
- 处理高分屏（PerMonitorV2）下的缩放与字体问题。

## 不适用范围

- 页面导航、按钮事件、异步流程与线程约定，转到 firewin-desktop-ui。
- 业务服务（挂载 ISO、写 BCD、分区）的实现，转到 firewin-image-deploy 与 firewin-bcd-boot。
- 桌面端命令行参数与退出码，转到 firewin-cli-automation。

## 关键文件与符号

| 路径 | 作用 | 关键资源/符号 |
|---|---|---|
| `src/FireWin/App.xaml` | 唯一全局资源入口 | 10 个画刷键、`PrimaryButton`、`SecondaryButton`、`ShutdownMode="OnMainWindowClose"` |
| `src/FireWin/LauncherWindow.xaml` | 启动二选一页，860×560 | `EntryCard`、`EntryIconBox`、`FooterText`、`VersionText` |
| `src/FireWin/MainWindow.xaml` | 六页部署向导，960×720 | `DarkListItem`、`VersionItemTemplate`、`PartitionItemTemplate`、`FeatureCard`、`StepDot0..3` |
| `src/FireWin/UsbWindow.xaml` | U 盘/ISO 双模式，720×680 | `DarkListItem`（重复定义）、`UsbItemTemplate`、`ModeRadio`、`Win11ProgressBar` |
| `src/FireWin/MainWindow.xaml.cs` | 步骤指示器与日志 | `GoToPage`、`UpdateStepIndicator`、`SetBusy`、`AppendLog`、画刷属性 |
| `src/FireWin/UsbWindow.xaml.cs` | 进度面板与模式切换 | `SetMode`、`BeginBuildProgress`、`ReportBuildProgress`、`ReportBuildPercent`、`FailBuildProgress` |
| `src/FireWin/LauncherWindow.xaml.cs` | 主窗口切换 | `Open(Window)` 先改 `Application.Current.MainWindow` 再 `Close()` |
| `src/FireWin/app.manifest` | DPI 与 UAC | `dpiAware=true/pm`、`dpiAwareness=PerMonitorV2`、`requireAdministrator` |

## 设计令牌

`App.xaml` 的 `Application.Resources` 是唯一色板，窗口内只允许用 `{StaticResource <键>}` 引用。

| 键 | 色值 | 用途 |
|---|---|---|
| `BgBrush` | `#1B1B1F` | 窗口背景 |
| `CardBrush` | `#242429` | 卡片、列表项、面板底色 |
| `BorderBrush` | `#33333A` | 所有 1px 边框 |
| `TextBrush` | `#ECECF1` | 主文本 |
| `MutedBrush` | `#9B9BA7` | 说明文字、标签 |
| `AccentBrush` | `#4C8DFF` | 主按钮、选中态、强调数字 |
| `OkBrush` | `#5BD48B` | 成功、就绪图标 |
| `WarnBrush` | `#F0B95A` | 警告条、系统盘标记 |
| `ErrorBrush` | `#F06A6A` | 错误卡片 |
| `LogBrush` | `#141418` | 日志区、只读输入框底色 |

未提取为资源的硬编码色（改主题时必须全局搜索）：

| 色值 | 出现位置 | 含义 |
|---|---|---|
| `#2E2E36` | 次按钮背景、图标方块、步骤圆点未激活 | 中性容器 |
| `#2C2C33` | 列表项/卡片 hover | 悬浮态 |
| `#22345A` | 选中态背景、图标方块强调底 | 选中/强调容器 |
| `#2A2A31` | 进度条轨道 | 进度底色 |
| `#C9D1D9` | 日志文字 | 日志前景 |
| `#2E2A1E` | 警告条背景 | 警告底 |

## 核心流程

### 新增或修改一个控件样式

1. 若需要新颜色，先在 [App.xaml](file:///e:/openwininstall/src/FireWin/App.xaml) 的 `Application.Resources` 里加 `<SolidColorBrush x:Key="...">`，不要在窗口里写死色值。
2. 通用交互控件（按钮、输入框）样式放 `App.xaml`；只在本窗口出现的样式放该窗口的 `Window.Resources`。
3. 重写 `Template` 时必须保留 `ContentPresenter`（按钮/列表项）或 `PART_Track` + `PART_Indicator`（进度条），否则内容不显示或值不更新。
4. 至少补齐三个触发器：`IsMouseOver`、`IsPressed`（按钮）或 `IsSelected`（列表项）、`IsEnabled=False`。
5. 应用后检查其他窗口是否也定义了同名样式（`DarkListItem` 在 MainWindow 与 UsbWindow 各有一份，改一处不会同步）。

### 修改步骤指示器

1. 视觉元素在 [MainWindow.xaml](file:///e:/openwininstall/src/FireWin/MainWindow.xaml) 的 `UniformGrid Columns="4"` 内，`StepDot0..3`、`StepNum0..3`、`StepLabel0..3`。
2. 状态计算在 `UpdateStepIndicator(int page)`：`page<=0` 为未开始，`1..3` 映射步骤 0..2，`>=4` 统一为步骤 3。
3. 激活色用 `AccentBrush`，未激活圆点固定 `Color.FromRgb(0x2E,0x2E,0x36)`，文字用 `Brushes.White` / `MutedBrush` / `TextBrush`。
4. 改步骤数量时必须同步 `GoToPage` 的页面显隐与 `NextButton.Content` 的 `switch`。

### 修改进度面板

1. 进度条是自定义模板 `Win11ProgressBar`：轨道 `PART_Track` 圆角 5，填充 `PART_Indicator` 左对齐、圆角 5，高度固定 10。
2. 百分比只前进不后退：`ReportBuildPercent` 用 `Math.Clamp` 并只在 `value > BuildProgressBar.Value` 时赋值。
3. 步骤文本与日志共用 `ReportBuildProgress`，失败用 `FailBuildProgress` 只改文本不归零。
4. 开始任务前调用 `BeginBuildProgress` 显示 `ProgressPanel` 并归零。

### 修改日志区

1. 日志控件是只读 `TextBox`（`IsReadOnly="True"`、`AcceptsReturn="True"`、`TextWrapping="Wrap"`），外面套 `Expander`（`LogExpander`）与 `Border`（高度 118 / 110）。
2. 写入口只有一个：`AppendLog`，内部先 `Dispatcher.CheckAccess()` 再递归 `Dispatcher.Invoke`，保证后台线程安全。
3. 不要换成 `RichTextBox`：当前实现依赖 `AppendText` + `ScrollToEnd` 的低开销路径。
4. 时间戳格式固定 `HH:mm:ss`，换行用 `Environment.NewLine`。

### 高分屏与窗口尺寸

1. DPI 感知只在 [app.manifest](file:///e:/openwininstall/src/FireWin/app.manifest) 声明，XAML 里不要再加 `dpiAware` 相关设置。
2. 三个窗口都设 `UseLayoutRounding="True"`，去掉它会让 1px 边框和圆角发虚。
3. 固定尺寸：启动页 860×560（最小 760×520）、向导 960×720（最小 860×620）、制作页 720×680（最小 640×560）。
4. 字体统一 `Microsoft YaHei UI`；非中文系统会回退到默认字体，不要假设字宽。

## 约束与坑

1. 现象：按钮点了没反应、内容空白。原因：重写 `ControlTemplate` 时漏掉 `ContentPresenter`。正确做法：模板里必须保留 `ContentPresenter`，边距用 `TemplateBinding Padding`。
2. 现象：按钮禁用后外观不变，用户重复点击。原因：只写了 hover 触发器。正确做法：补 `IsEnabled=False` 触发器（现有实现是 `Opacity=0.35` + `Cursor=Arrow`）。
3. 现象：进度条 `Value` 改了但 UI 不动。原因：模板缺少名为 `PART_Track` / `PART_Indicator` 的元素。正确做法：两个名字必须完全一致，否则 ProgressBar 找不到注入点。
4. 现象：只在一个窗口改了列表样式，另一个窗口没变。原因：`DarkListItem` 在两个窗口各复制了一份。正确做法：改样式时同步两个 XAML，或抽到 `App.xaml` 统一引用。
5. 现象：选中项背景溢出卡片圆角。原因：`ListBoxItem` 的 `Padding` 与外层 `Border` 圆角冲突。正确做法：`ListBoxItem` 设 `Padding=0`，内层 `Border` 承担 `CornerRadius` 与 `Padding`。
6. 现象：调色后部分控件的 `StaticResource` 找不到。原因：`StaticResource` 在解析时立即查找，窗口级资源不能反向被 `App.xaml` 引用。正确做法：全局色板只放 `App.xaml`，窗口样式只引用它。
7. 现象：关闭启动页后程序直接退出。原因：`ShutdownMode="OnMainWindowClose"`，`Close()` 的是当前主窗口。正确做法：沿用 `LauncherWindow.Open`，先赋 `Application.Current.MainWindow` 再 `Show()` 最后 `Close()`。
8. 现象：日志偶发乱序或跨线程异常。原因：后台 `Progress` 直接改 UI。正确做法：所有 UI 写操作走 `AppendLog` 或 `Dispatcher.Invoke`。
9. 现象：窗口在 125%/150% 缩放下文字被裁切。原因：写死了像素高度且未开布局取整。正确做法：保持 `UseLayoutRounding`，文本容器用 `TextWrapping="Wrap"`，避免给 `TextBlock` 设死 `Height`。
10. 现象：改主题只改了一半。原因：`#2E2E36`、`#2C2C33`、`#22345A` 等色值直接写在 XAML 和 `UpdateStepIndicator` 的 C# 里。正确做法：全局搜索这些色值，XAML 与 C# 同步改，必要时先提取为资源键。

## 验证清单

- [ ] 新增/修改的色值只出现在 `App.xaml`，窗口内全部通过 `{StaticResource ...}` 引用。
- [ ] 新按钮样式在正常、hover、按下、禁用四种状态下都有可见反馈。
- [ ] `UpgradeButton` / `NextButton` 等禁用态改回启用后样式恢复，不残留半透明。
- [ ] 步骤指示器在 page 0/1/2/3/4/5 下圆点数量与高亮位置正确。
- [ ] 进度条从 0 走到 100 无回退，`ProgressPercentText` 与条体同步。
- [ ] 日志在后台线程写入时不抛 `InvalidOperationException`，且自动滚到底部。
- [ ] 125%、150%、200% 缩放下三个窗口无文字裁切、无边框发虚。
- [ ] 关闭启动页进入向导/制作页后进程不退出。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 按钮内容不显示 | 模板是否保留 `ContentPresenter` | 补回 `ContentPresenter` |
| 禁用按钮仍可点 | `SetBusy` 是否覆盖该控件、触发器是否完整 | 补 `IsEnabled` 触发器并纳入 `SetBusy` |
| 进度条不动 | `PART_Track` / `PART_Indicator` 命名 | 改为标准命名 |
| 列表选中背景错位 | `ListBoxItem.Padding` 与内层 `Border` | `Padding=0`，圆角交给内层 |
| `StaticResource` 找不到 | 资源定义在哪个字典、是否跨窗口引用 | 全局资源移到 `App.xaml` |
| 关闭子页后程序退出 | `MainWindow` 是否已切换 | 用 `LauncherWindow.Open` 的顺序 |
| 高 DPI 下模糊 | `UseLayoutRounding`、是否重复声明 dpiAware | 保留布局取整，DPI 只在 manifest 声明 |
| 日志跨线程报错 | 是否绕过 `AppendLog` | 统一走 `AppendLog` |

## 相关 skill

- `firewin-desktop-ui`：页面导航、按钮事件与异步流程，本 skill 只负责“长什么样”。
- `firewin-cli-automation`：命令行入口与退出码，样式改动后仍需保证 CLI 分支可用。
- `firewin-overview`：仓库层次与构建方式，改了 XAML 后重新 `dotnet publish` 验证。
