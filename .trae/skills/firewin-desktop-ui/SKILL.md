---
name: firewin-desktop-ui
description: Desktop WPF UI for FireWin covering launcher routing, six-page wizard navigation, USB and ISO modes and thread-safe logging. Use when editing windows or events, not for image parsing or BCD commands.
---
# FireWin 桌面端 UI 架构

桌面端是单 exe 的 WPF 程序，负责入口分流、安装向导、U 盘制作与 ISO 生成三种界面的组织与线程调度。

## 适用范围

- 修改 `App.OnStartup` 的 GUI/CLI 双模式分流或命令行开关。
- 调整 `LauncherWindow` 的“一键安装系统 / 制作 PE”二选一入口。
- 修改 `MainWindow` 六页向导的页面切换、按钮状态与步骤指示器。
- 修改 `UsbWindow` 的 U 盘与 ISO 双模式、升级按钮联动与进度区。
- 调整 XAML 资源字典、控件命名、列表模板与深色主题。
- 排查 `AppendLog`、`SetBusy`、进度条在跨线程调用时的表现。

## 不适用范围

- ISO 挂载、WIM 索引枚举、分区枚举与文件复制实现，转到 firewin-image-deploy。
- `bcdedit` 顺序、`deploy.json` 字段与回滚，转到 firewin-bcd-boot。
- `pe/` 下 PE 原生界面与 `FireBoot` 工具，不在 WPF 范围。

## 关键文件与符号

| 路径 | 作用 | 关键类型/方法 |
|---|---|---|
| `src/FireWin/App.xaml` | 全局资源与关闭策略 | `ShutdownMode="OnMainWindowClose"`、`BgBrush`、`CardBrush`、`AccentBrush`、`PrimaryButton`、`SecondaryButton` |
| `src/FireWin/App.xaml.cs` | 入口分流与 CLI | `OnStartup`、`RunCli`、`NativeConsole.AttachToParent`、`ConsoleProgress` |
| `src/FireWin/FireWin.csproj` | 单文件发布、x64 平台与 payload 复制 | `PublishSingleFile`、`IncludeNativeLibrariesForSelfExtract`、`CopyBundledPayloadToPublish` |
| `src/FireWin/Services/BundledPayload.cs` | 入口与向导共用的 payload 校验 | `TryGetPePayload`、`PeDirectory` |
| `src/FireWin/Services/WimInitService.cs` | 版本页前置的 wimlib 初始化 | `EnsureInitialized`、`LoadedLibraryPath` |
| `src/FireWin/LauncherWindow.xaml` | 入口选择页 | `EntryCard`、`EntryIconBox`、`InstallButton_Click`、`PeButton_Click` |
| `src/FireWin/LauncherWindow.xaml.cs` | 页脚状态与窗口切换 | `BuildStatusText`、`Open` |
| `src/FireWin/MainWindow.xaml` | 安装向导与六页布局 | `PageWelcome`、`PageImage`、`PageVersion`、`PagePartition`、`PageConfirm`、`PageDone` |
| `src/FireWin/MainWindow.xaml.cs` | 导航、状态与部署编排 | `GoToPage`、`UpdateStepIndicator`、`SetBusy`、`AppendLog`、`DeployAsync` |
| `src/FireWin/UsbWindow.xaml` | 制作 PE 双模式布局 | `UsbModeRadio`、`IsoModeRadio`、`UsbPanel`、`IsoPanel`、`ProgressPanel` |
| `src/FireWin/UsbWindow.xaml.cs` | 制作流程与进度 | `SetMode`、`CreateUsbAsync`、`CreateIsoAsync`、`ReportBuildPercent` |
| `src/FireWin/Services/DeployService.cs` | 被 UI 调用的镜像与分区能力 | `MountIsoAsync`、`ListWimImages`、`GetFixedPartitions` |
| `src/FireWin/Services/UsbBootService.cs` | U 盘制作与升级能力 | `ListUsbDisks`、`CanUpgradeInPlace`、`CreateBootDiskAsync` |
| `src/FireWin/Services/IsoImageService.cs` | ISO 生成能力与 BCD 复用判断 | `CreatePeIsoAsync`、`HostBcdAvailable` |

## 核心流程

1. 入口分流：`App.OnStartup` 先判断 `e.Args.Length >= 2` 且 `e.Args[0]` 等于 `--demo-image`（忽略大小写），命中则创建 `MainWindow` 并调用 `DemoSelectImage(e.Args[1])`。
2. 其余带参数启动一律进入 `RunCli`，支持 `--wim-init-check`、`--adk-status`、`--adk-install`、`--version`，未知参数打印用法并返回 0。
3. 无参数启动创建 `LauncherWindow`；页脚 `FooterText` 由 `BuildStatusText` 生成“内置 PE 状态 + 管理员状态”，版本号取程序集 `Version.ToString(3)`。
4. `LauncherWindow.InstallButton_Click` 打开 `MainWindow`，`PeButton_Click` 打开 `UsbWindow`；二者都走 `Open`，先赋 `Application.Current.MainWindow = window`，再 `Show`，最后 `Close`。
5. `MainWindow` 页面索引 0 至 5 分别对应 `PageWelcome`、`PageImage`、`PageVersion`、`PagePartition`、`PageConfirm`、`PageDone`；`GoToPage` 同时更新可见性、`NextButton.Content`、返回按钮与步骤条。
6. 页面 0 点“开始部署”时校验 `IsAdministrator()` 与 `BundledPayload.TryGetPePayload(out missing)`，任一失败弹窗并停留。
7. 页面 1 点“识别镜像版本”要求 `_wimPath` 非空，调用 `EnumerateImagesAsync` 后进入页面 2；页面 2 选中项写入 `_selectedImage` 后进入页面 3。
8. 进入页面 3 时 `GoToPage` 调用 `LoadPartitions`，默认选中第一个非系统盘；页面 4 的 `FillConfirmSummary` 写入镜像、版本、目标分区、固件与 staging 路径。
9. 页面 4 点“开始部署”调用 `DeployAsync`：确认框 → `SetBusy(true)` → 展开日志 → 复制镜像 → `PreparePeBootAsync` → `GoToPage(5)`。
10. `UsbWindow` 构造时先填默认 ISO 路径 `AppContext.BaseDirectory\FireWinPE.iso`，再把 `UsbModeRadio.IsChecked` 设为 true 触发 `SetMode(MediaMode.Usb)`。
11. `SetMode` 切换 `UsbPanel` 与 `IsoPanel` 可见性、窗口标题文案、`CreateButton.Content` 与 `ImageHintText`，最后调用 `UpdateCreateButtonState`。
12. U 盘制作 `CreateUsbAsync(upgrade)` 依次校验选中盘、管理员、payload、二次确认，然后 `Task.Run` 调 `UpgradeBootDiskAsync` 或 `CreateBootDiskAsync`。
13. ISO 生成 `CreateIsoAsync` 先规范化输出路径（相对路径拼 `AppContext.BaseDirectory`、补 `.iso`），再校验管理员或 `IsoImageService.HostBcdAvailable`。
14. 线程约定：UI 更新只在 UI 线程；耗时服务调用放 `Task.Run`；日志用 `Progress<string>` 或 `AppendLog`，后者用 `Dispatcher.CheckAccess` 递归切回 UI 线程。
15. CLI 输出附着：`RunCli` 仅在 `!Console.IsOutputRedirected` 时调用 `AttachToParent`，结束时 `Flush` 并按需 `Detach`。
16. 完成页只提供两个动作：`RebootButton_Click` 二次确认后调 `BcdBootService.RestartComputerAsync`，`ExitButton_Click` 直接 `Close()`。
17. UsbWindow 的进度三件套：`BeginBuildProgress` 归零并显示 `ProgressPanel`，`ReportBuildProgress` 同步写日志与 `ProgressStepText`，`ReportBuildPercent` 更新进度条。
18. `SetBusy(false)` 不只是恢复按钮，还会按 `_page` 重新计算 `NextButton.IsEnabled`，页面 1 依赖 `_wimPath`、页面 2 依赖 `VersionList.SelectedItem`、页面 3 依赖 `PartitionList.SelectedItem`。

## 数据结构与契约

`MainWindow` 页面状态：

| 页面索引 | 控件 | `NextButton.Content` | 返回按钮 |
|---|---|---|---|
| 0 | `PageWelcome` | 开始部署 | 隐藏 |
| 1 | `PageImage` | 识别镜像版本 | 显示 |
| 2 | `PageVersion` | 下一步：选择分区 | 显示 |
| 3 | `PagePartition` | 下一步：确认部署 | 显示 |
| 4 | `PageConfirm` | 开始部署 | 显示 |
| 5 | `PageDone` | 隐藏 | 隐藏 |

关键字段与常量：

- `MainWindow` 字段：`_wim`（`WimInitService`）、`_isoPath`、`_wimPath`、`_images`、`_selectedImage`、`_targetPartition`、`_firmware`、`_page`、`_busy`。
- `UsbWindow` 私有枚举 `MediaMode { Usb, Iso }`，字段 `_mode` 默认 `Usb`，另有 `_optionalImagePath`、`_busy`。
- `WimImageInfo(int Index, string Name, string Description, ulong TotalBytes)`，列表模板 `VersionItemTemplate`。
- `PartitionInfo(string Drive, string VolumeLabel, ulong TotalBytes, ulong FreeBytes, bool IsSystemDrive)`，模板 `PartitionItemTemplate`。
- `UsbDiskInfo(string Drive, string VolumeLabel, ulong PartitionTotalBytes, ulong PartitionFreeBytes, int DiskNumber, ulong DiskTotalBytes)`。
- 步骤条映射：`page <= 0 => -1`，`1 => 0`，`2 => 1`，`3 => 2`，`>= 4 => 3`；控件为 `StepDot0..3`、`StepNum0..3`、`StepLabel0..3`。
- 窗口尺寸：Launcher 860x560，MainWindow 960x720，UsbWindow 720x680。
- 日志格式固定为 `[HH:mm:ss] message`，两个窗口的 `LogBox` 都在 `Expander` 内。
- 欢迎页状态控件固定为 `AdminValue`/`AdminIcon`、`PayloadValue`/`PayloadIcon`、`FirmwareValue`/`FirmwareIcon`，颜色取 `OkBrush`/`WarnBrush`。
- 确认页使用 `SummaryImagePath`、`SummaryImageVersion`、`SummaryTarget`、`SummaryFirmware`、`SummaryStaging`，样式 `SummaryLabel` 与 `SummaryValue`。
- 列表样式复用 `DarkListItem`；版本页模板 `VersionItemTemplate`，分区页模板 `PartitionItemTemplate`。
- UsbWindow 进度相关控件为 `ProgressPanel`、`BuildProgressBar`、`ProgressPercentText`、`ProgressStepText`。
- 主题色语义：`AccentBrush` 蓝用于激活步骤，`OkBrush` 绿表示就绪，`WarnBrush` 黄表示缺失或未知，`MutedBrush` 灰表示次要说明。

## 约束与坑

1. 现象：从 Launcher 进入子窗口后程序直接退出。原因：`ShutdownMode=OnMainWindowClose`，旧主窗口关闭即退出。正确做法：必须走 `Open`，先替换 `Application.Current.MainWindow` 再 `Close()`。
2. 现象：`UsbWindow` 启动时 `NullReferenceException`。原因：`UsbModeRadio.IsChecked = true` 会在 `InitializeComponent` 期间触发 `ModeRadio_Checked`。正确做法：保留 `if (_busy || IsoPanel is null) return;` 守卫。
3. 现象：选中 U 盘后界面崩溃或按钮状态错误。原因：`UsbList.SelectedItem` 可能是 null 或其他类型。正确做法：始终用 `is UsbDiskInfo disk` 模式匹配，不要直接强转。
4. 现象：部署时日志偶发跨线程异常。原因：`Progress<string>` 回调通常回到创建线程，但服务层也可能直接调用 `AppendLog`。正确做法：`AppendLog` 保持 `Dispatcher.CheckAccess` 判断并递归 `Dispatcher.Invoke`。
5. 现象：进度条回退或卡在旧百分比。原因：各阶段百分比映射可能回退。正确做法：`UsbWindow.ReportBuildPercent` 用 `Math.Clamp` 且只在大于当前值时更新。
6. 现象：忙时仍能改动列表选择。原因：`MainWindow.SetBusy(true)` 只禁用 `NextButton`、`BackButton`、`BrowseButton`，未禁用 `VersionList` 与 `PartitionList`。正确做法：依赖各 `SelectionChanged` 中的 `_busy` 判断，改动时不要移除该守卫。
7. 现象：关窗后制作任务仍在后台跑，甚至写盘完成。原因：`CloseButton_Click` 只在 `_busy` 时拒绝关闭，窗口标题栏关闭不受控，且服务没有取消令牌绑定。正确做法：制作期间不要关闭窗口；要支持取消需新增窗口关闭拦截与 `CancellationTokenSource`。
8. 现象：目标分区只剩系统盘时弹窗反复出现。原因：`LoadPartitions` 先选非系统盘，没有时回退选第一项；`PartitionList_SelectionChanged` 又把系统盘改回 `RemovedItems[0]` 或 null。正确做法：仅系统盘时保持空选并禁用下一步。
9. 现象：“升级启动盘”按钮亮着但升级失败。原因：按钮只依据 `CanUpgradeInPlace`，真正升级还要检查启动分区空间与格式。正确做法：失败提示按服务层异常展示，必要时引导用户“全新制作”。
10. 现象：`--demo-image` 失败时无任何提示。原因：`DemoSelectImage` 是 `async void` 且空 `catch`，只用于自动化截图。正确做法：不要把它当用户入口，正式验证走浏览按钮。
11. 现象：ISO 模式未以管理员运行却提示权限不足。原因：仅当 `!IsAdministrator() && !IsoImageService.HostBcdAvailable` 时才拦截，能复用宿主 BCD 时无需管理员。正确做法：先安装带 BCD 的 Windows 宿主，或改用管理员运行。
12. 现象：切到 ISO 模式后仍带着之前选的镜像。原因：`_optionalImagePath` 在模式切换时不清空，属于跨模式共享的可选镜像。正确做法：需要清空时重新浏览或重启窗口，不要假设模式切换会重置。
13. 现象：Launcher 页脚一直显示 payload 缺失。原因：`BuildStatusText` 在构造函数执行一次，不随文件变化刷新。正确做法：补齐 payload 后重新启动程序。

## 验证清单

- [ ] 无参数启动显示 Launcher，页脚同时显示内置 PE 大小与管理员状态。
- [ ] 点击两个入口分别打开 MainWindow 与 UsbWindow，主程序不退出。
- [ ] 向导六页可见性、`NextButton.Content` 与步骤条点亮的格数与页面一致。
- [ ] 页面 0 在非管理员或缺 payload 时阻止进入页面 1，并给出对应弹窗。
- [ ] 分区页默认选中非系统盘，手动点系统盘会被弹窗拒绝并回退。
- [ ] 忙时 `NextButton`、`BackButton`、`BrowseButton` 禁用，结束后按当前页恢复。
- [ ] UsbWindow 切换两种模式时标题、按钮文案、提示文案与面板可见性同步变化。
- [ ] 所有日志行带 `[HH:mm:ss]` 前缀并自动滚动到底部。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 双击 exe 没有窗口 | `OnStartup` 参数分支、`ShutdownMode` | 去掉参数；检查是否误入 CLI |
| 点入口后程序退出 | `Open` 是否先换 `MainWindow` | 恢复 `Application.Current.MainWindow = window` |
| 页面跳转但按钮文案不对 | `GoToPage` 的 switch 与 `_page` | 修正页面索引映射 |
| 日志不刷新 | `AppendLog` 的 `Dispatcher` 分支、`LogBox` 绑定 | 确保回调回到 UI 线程 |
| 进度条停住 | `BeginBuildProgress` 是否调用、百分比是否被 clamp | 检查服务层 `percentProgress` 上报 |
| 升级按钮始终灰色 | `CanUpgradeInPlace`、卷标、选中项类型 | 选择 FireWin 启动盘或全新制作 |
| ISO 输出路径为空 | `IsoPathBox.Text`、`CreateIsoAsync` 的 Trim | 重新浏览选择并以 `.iso` 结尾 |
| CLI 在终端无输出 | `Console.IsOutputRedirected`、`AttachToParent` | 交互终端运行，重定向时直接读标准输出 |
| 生成 ISO 时要求管理员 | `IsoImageService.HostBcdAvailable` | 安装带 BCD 的宿主，或用管理员运行 |
| 制作按钮无法点击 | `UpdateCreateButtonState` 的前置条件 | 选中 U 盘或填写 ISO 路径 |
| 制作中关窗后任务继续 | 标题栏关闭与 `_busy`、取消令牌 | 制作完成前不要关窗，后续补关闭拦截 |

## 相关 skill

- `firewin-overview`：提供仓库层次、构建与产物位置，本 skill 是其 UI 层展开。
- `firewin-image-deploy`：解释 UI 调用的 `ResolveImageAsync`、`EnumerateImagesAsync`、`DeployAsync` 内部逻辑。
- `firewin-bcd-boot`：解释 `PreparePeBootAsync` 在确认页与完成页之间的实际作用。
- `firewin-wpf-styling`：解释这些页面用到的色板、控件模板与进度/日志视觉实现。
- `firewin-cli-automation`：解释 `--demo-image` 如何跳过文件对话框直达镜像页。
