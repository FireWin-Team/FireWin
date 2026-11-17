---
name: firewin-image-deploy
description: Image parsing and deployment prep for FireWin with ISO mounting, WIM index enumeration, partition listing, staged copy and demo CLI. Use when changing image or partition logic, not for BCD or WPF.
---
# FireWin 镜像解析与部署

本 skill 覆盖从用户选择 ISO/WIM 到把镜像复制进 `C:\FireWinPE\install.wim` 的全过程，是部署链路中“数据准备”的一层。

## 适用范围

- 新增或修改 ISO 挂载、卸载与 `sources\install.*` 查找逻辑。
- 调整 WIM/ESD/SWM 的索引枚举、名称回退与体积显示。
- 修改固定磁盘分区枚举、系统盘标记与分区选择默认值。
- 调整镜像到本地 staging 的流式复制、进度文本与百分比。
- 排查 `--demo-image`、浏览对话框与文件过滤器的行为。
- 核对部署前的路径常量、记录类型与异常语义。

## 不适用范围

- BCD 临时项、`deploy.json` 写入、回滚与重启，转到 firewin-bcd-boot。
- 页面导航、按钮状态与 XAML 模板，转到 firewin-desktop-ui。
- PE 端的 `wimlib-imagex apply` 与永久引导修复，属于 `pe/main.cpp`，不在本 skill。

## 关键文件与符号

| 路径 | 作用 | 关键类型/方法 |
|---|---|---|
| `src/FireWin/Services/DeployService.cs` | 镜像与分区能力的唯一实现 | `MountIsoAsync`、`TryDismountIso`、`FindInstallWim` |
| `src/FireWin/Services/DeployService.cs` | WIM 版本枚举与体积解析 | `ListWimImages`、`WimImageInfo` |
| `src/FireWin/Services/DeployService.cs` | 固定分区枚举与系统盘标记 | `GetFixedPartitions`、`PartitionInfo` |
| `src/FireWin/Services/DeployService.cs` | 大文件流式复制与格式化 | `CopyFileWithProgressAsync`、`FormatBytes` |
| `src/FireWin/Services/DeployService.cs` | ISO 挂载结果 | `IsoMountResult`、`DeployException` |
| `src/FireWin/Services/WimInitService.cs` | 使用 ManagedWimLib 前的初始化 | `EnsureInitialized`、`LoadedLibraryPath` |
| `src/FireWin/MainWindow.xaml.cs` | UI 侧编排与状态保存 | `BrowseImage_Click`、`ResolveImageAsync` |
| `src/FireWin/MainWindow.xaml.cs` | 版本页与分区页装载 | `EnumerateImagesAsync`、`LoadPartitions` |
| `src/FireWin/MainWindow.xaml.cs` | 部署确认与 staging 复制 | `DeployAsync`、`DemoSelectImage` |
| `src/FireWin/FireWin.csproj` | WIM 读取依赖 | `ManagedWimLib 2.6.0`、`PlatformTarget=x64` |
| `src/FireWin/Services/BundledPayload.cs` | 提供 PE 源目录并校验 payload | `PeDirectory`、`TryGetPePayload` |
| `src/FireWin/UsbWindow.xaml.cs` | U 盘模式可复用同一镜像路径 | `_optionalImagePath`、`ImagePathBox` |
| `tools/wimlib-imagex.exe` | 与原生库配套的命令行工具 | 与 `libwim-15.dll` 同目录分发 |
| `pe/main.cpp` | 消费 staging 与 deploy.json 的下一跳 | `Deploy`、`ResolveVolumeToDriveLetter` |

## 核心流程

1. 浏览选择：`BrowseImage_Click` 打开 `OpenFileDialog`，过滤器为 `Windows 系统镜像|*.iso;*.wim;*.esd;*.swm|所有文件|*.*`，取消则直接返回，不改旧状态。
2. 路径分流：`ResolveImageAsync` 取小写扩展名；`.iso` 走挂载分支，其余按普通镜像文件处理。
3. ISO 挂载：`MountIsoAsync` 先用 `File.Exists` 校验，再用单引号转义（`'` 变 `''`）拼 PowerShell 命令 `$ErrorActionPreference='Stop'; $img = Mount-DiskImage -ImagePath '<path>' -PassThru; ($img | Get-Volume).DriveLetter`。
4. 盘符校验：结果 `Trim()` 后必须是非空单个 ASCII 字母（A-Z/a-z），否则抛“ISO 已挂载，但未能获取盘符”。
5. 查找安装镜像：`FindInstallWim` 依次尝试 `sources\install.wim`、`sources\install.esd`、`sources\install.swm`；都没有时再浅层枚举 `sources\install.*`，只接受 wim/esd/swm 扩展名。
6. 挂载成功后 `ResolveImageAsync` 记录 `_isoPath` 与 `_wimPath`，界面提示“ISO 已挂载到 X:，镜像 …\sources\install.wim”；直接选镜像则校验存在性并提示“已选择镜像”。
7. 枚举版本：`EnumerateImagesAsync` 要求 `_wimPath` 非空，`SetBusy(true)` 后在 `Task.Run` 内先 `EnsureInitialized()` 再 `ListWimImages(_wimPath)`。
8. `ListWimImages` 用 `Wim.OpenWim(path, OpenFlags.None)`，读取 `GetWimInfo().ImageCount`，索引从 1 到 count，名称空时回退为 `镜像 {index}`，再尝试从 XML 补体积。
9. 枚举成功且数量大于 0 时绑定 `VersionList.ItemsSource`、默认 `SelectedIndex=0`，再 `GoToPage(2)`；数量为 0 抛“镜像中未包含任何系统版本，无法部署”。
10. 分区装载：`LoadPartitions` 调 `GetFixedPartitions`，绑定后优先选中第一个 `IsSystemDrive == false` 的分区，没有则选第一项。
11. 部署前确认：`DeployAsync` 校验 `_wimPath`、`_selectedImage`、`_targetPartition` 均存在，弹确认框，用户取消则直接返回。
12. 复制阶段：staging 固定为 `BcdBootService.DefaultStagingDirectory`（`C:\FireWinPE`），目标文件名固定 `install.wim`，调用 `DeployService.CopyFileWithProgressAsync(_wimPath!, localWim, progress, default)`，其中 `progress = new Progress<string>(AppendLog)`，不传 `percentProgress`。
13. 复制完成后再构造 `PeBootRequest`，把 `WimPath=localWim`、`WimIndex=_selectedImage.Index`、`TargetDrive=_targetPartition.Drive`、`AutoReboot=AutoRebootCheck.IsChecked == true` 交给 `PreparePeBootAsync`。
14. `--demo-image <path>` 由 `App.OnStartup` 直接构造 `MainWindow` 后调用 `DemoSelectImage`，跳过欢迎页直达镜像页并执行 `ResolveImageAsync`。
15. 窗口关闭时 `Closed` 事件释放 `_wim` 并调用 `TryDismountIso(_isoPath)`，保证正常退出不残留挂载。
16. `UsbWindow` 的制作流程也会把已有 `_wimPath` 传给 `_optionalImagePath`，但镜像解析仍由本 skill 的方法负责，U 盘页只负责路径展示与传参。
17. 卸载实现：`TryDismountIso` 通过 `Dismount-DiskImage -ImagePath '<path>'` 执行，最多等待 15 秒，方法无返回值，任何异常都在内部吞掉。
18. PowerShell 调用统一走 `RunPowerShellAsync`，固定 `-NoProfile -ExecutionPolicy Bypass`，先读完 stdout 再读 stderr，退出码非 0 抛 `DeployException`。
19. 复制过程中的文本进度按 1% 变化上报，形如“复制 install.wim… N%（X / Y）”，结束时补报“复制完成”。

## 数据结构与契约

记录类型：

- `WimImageInfo(int Index, string Name, string Description, ulong TotalBytes)`，附 `TotalBytesText => DeployService.FormatBytes(TotalBytes)`。
- `PartitionInfo(string Drive, string VolumeLabel, ulong TotalBytes, ulong FreeBytes, bool IsSystemDrive)`，附 `SystemTagText` 与 `SummaryText`。
- `IsoMountResult(string DriveLetter, string IsoPath, string InstallWimPath)`。
- `DeployException : Exception`，用于所有可展示的部署前置错误。
- `PeBootRequest` 与 `DeployConfiguration` 的字段定义在 firewin-bcd-boot 中展开。

关键常量与实现细节：

- 复制缓冲区 `1 MiB`（`new byte[1024 * 1024]`），读使用 `FileOptions.Asynchronous | FileOptions.SequentialScan`。
- 源文件打开共享模式为 `FileShare.Read`，目标为 `FileMode.Create + FileShare.None`。
- 体积正则：`<IMAGE\s+INDEX="(?<index>\d+)"[^>]*>.*?<TOTALBYTES>(?<bytes>\d+)</TOTALBYTES>`，配合 `RegexOptions.Singleline | RegexOptions.IgnoreCase`。
- 系统盘根：`Path.GetPathRoot(Environment.SystemDirectory)`；盘符统一取 `RootDirectory.FullName.Substring(0, 2).ToUpperInvariant()`。
- 分区过滤条件：`DriveInfo.IsReady && DriveType.Fixed`，根路径长度至少 2。
- 无卷标统一显示为 `(无卷标)`；系统盘标签为 `（当前系统盘，不可作为安装目标）`。
- 支持的镜像扩展名：`.wim`、`.esd`、`.swm`、`.iso`；staging 目标固定 `.wim` 文件名。
- `FormatBytes` 对 ulong 先截断到 long 再格式化，单位自动取 B/KB/MB/GB，最多一位小数。
- CLI 相关开关：`--demo-image <path>`（仅演示）、`--wim-init-check`（wimlib 自检）、`--adk-status`、`--adk-install`、`--version`。
- `MainWindow` 保存 `_images`（版本集合）、`_partitions`（分区集合）、`_isoPath`、`_wimPath`、`_selectedImage`、`_targetPartition`。
- `_isoPath` 只在 ISO 分支赋值；直接选 WIM/ESD 时置空，避免关闭时误卸载无关盘。
- `TryDismountIso` 无参数校验，内部 `try/catch` 吞掉所有异常，因此对 null 路径也表现为幂等成功。
- `ListWimImages` 不负责初始化 wimlib，调用方必须先执行 `WimInitService.EnsureInitialized()`，UI 在 `Task.Run` 内完成。
- 枚举失败会原样抛出 `WimLibException` 或 `DeployException`，由 `EnumerateImagesAsync` 的 `finally` 恢复按钮状态。
- `PartitionInfo.SummaryText` 固定为 `总容量 X · 可用 Y`，确认页直接使用该字符串。
- 镜像路径以用户选择为准，不做 `Path.GetFullPath` 规范化；ISO 输出路径才在 `UsbWindow.CreateIsoAsync` 中规范化。

## 约束与坑

1. 现象：ISO 挂载后返回空盘符。原因：`Get-Volume` 对含引导分区的安装 ISO 可能返回多个卷或无盘符卷，命令直接取 `DriveLetter`。正确做法：校验失败立即提示重新挂载，必要时手工分配盘符或在磁盘管理确认。
2. 现象：挂载成功但重启进入 PE 后找不到镜像。原因：ISO 挂载是会话级，重启或注销即消失；代码也只在 staging 复制后才交给 PE。正确做法：部署前必须完成 `CopyFileWithProgressAsync`，不能让 PE 直接读挂载盘。
3. 现象：程序被杀后盘符一直存在。原因：`TryDismountIso` 只在 `Closed` 事件执行，进程异常退出不会走到。正确做法：重新登录或重启清理，演示环境可在磁盘管理手动卸载。
4. 现象：选择 `install.swm` 后版本列表只有部分或打不开。原因：`FindInstallWim` 只返回第一个 `.swm`，`ListWimImages` 也只打开该单文件，未拼接 `.2.swm`、`.3.swm`。正确做法：优先使用完整 WIM/ESD；必须用 SWM 时先合并成单 WIM。
5. 现象：版本列表能显示名称但没有体积。原因：体积依赖 XML 里的 `<TOTALBYTES>` 紧跟在 `<IMAGE INDEX>` 之后，布局不符时正则不命中且异常被忽略。正确做法：把体积视为可选展示，不要用它做空间判断。
6. 现象：脚本化调用绕过 UI 后把系统盘当目标。原因：`GetFixedPartitions` 只负责打 `IsSystemDrive` 标记，`DeployAsync` 没有再次拒绝系统盘，保护在 `PartitionList_SelectionChanged` 的 UI 层。正确做法：任何新增入口都要自行校验 `IsSystemDrive`。
7. 现象：复制到一半报磁盘不足。原因：代码在复制前不检查 `C:\FireWinPE` 所在卷的可用空间，staging 需要容纳约 1 GB 的 `boot.wim` 加整个 install 镜像。正确做法：部署前用 `PartitionInfo.FreeBytes` 或 `DriveInfo` 预检系统盘剩余空间。
8. 现象：系统盘不是 C: 时复制成功但 BCD 阶段失败。原因：`DeployAsync` 的 staging 固定 `C:\FireWinPE`，而 `SystemDrive` 取 `Environment.SystemDirectory` 的根，二者可能不同分区。正确做法：让 staging 跟随系统盘根，或在部署前拦截非 C: 系统盘。
9. 现象：替换镜像文件时报“文件被占用”。原因：复制期间源文件以 `FileShare.Read` 打开，只允许读不允许写/删除。正确做法：复制完成后再替换源镜像，或先关闭向导窗口。
10. 现象：浏览取消后页面仍显示上一次镜像。原因：取消时 `BrowseImage_Click` 在 `ShowDialog()` 返回 null 后直接 return，不重置状态。正确做法：这是预期行为，需要清空时退出向导重进。
11. 现象：`--demo-image` 传入不存在路径时界面停在镜像页且无报错。原因：`DemoSelectImage` 先判 `File.Exists`，异常也被空 `catch` 吞掉。正确做法：演示参数只用于已确认存在的镜像，正式使用走浏览按钮。
12. 现象：卸载后盘符仍在且无任何提示。原因：`TryDismountIso` 无返回值、不检查退出码，15 秒超时也静默结束。正确做法：以资源管理器盘符消失为准，超时后手工卸载或重启。
13. 现象：直接调用 `DeployService.ListWimImages` 抛初始化异常。原因：该方法内部不做 `EnsureInitialized`，初始化责任在 UI 的 `EnumerateImagesAsync`。正确做法：新增调用点先执行 `WimInitService.EnsureInitialized()`。
14. 现象：复制大镜像时没有百分比进度条。原因：`DeployAsync` 只传文本 `Progress<string>`，第 4 个参数 `percentProgress` 省略。正确做法：需要进度条时传入 `IProgress<int>` 并绑定到 UI 进度控件。

## 验证清单

- [ ] 选择 ISO 后提示挂载盘符，且 `_wimPath` 指向 `sources\install.wim` 或 `install.esd`。
- [ ] 选择 WIM/ESD 时不触发挂载，直接显示所选路径。
- [ ] 版本页列出从 1 开始的索引、名称与可用体积，默认选中第 1 项。
- [ ] 空镜像或无可用索引时给出“镜像中未包含任何系统版本，无法部署”而不是进入下一页。
- [ ] 分区页只出现固定磁盘，系统盘带“（当前系统盘，不可作为安装目标）”标签且被拒绝选中。
- [ ] staging 下生成 `C:\FireWinPE\install.wim`，大小与源文件一致。
- [ ] 复制进度文本包含已写/总量/百分比，结束时上报 100。
- [ ] 关闭窗口后 `Mount-DiskImage` 挂载被卸载，资源管理器中盘符消失。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 提示“ISO 未分配可用盘符” | `Get-Volume` 输出、ISO 卷结构 | 重新挂载或手工分配盘符 |
| 提示“未找到 install 镜像” | ISO 内 `sources\install.*` 是否存在 | 换完整 Windows ISO |
| WIM 打不开或初始化失败 | `EnsureInitialized`、`libwim-15.dll` 与位数 | 用 x64 发布并执行 `--wim-init-check` |
| 版本名为“镜像 1” | `GetImageName` 返回空、XML 元数据 | 属正常回退，核对 WIM 是否为多版本 |
| 分区列表为空 | `DriveType.Fixed`、磁盘是否在线 | 检查磁盘管理，避免用可移动盘当目标 |
| 复制进度不动 | `IProgress<int>` 是否为 null、文件是否超大 | 日志仍应按 MB 持续输出，检查源文件可读 |
| 复制后 BCD 报路径错误 | staging 与系统盘是否同分区 | 统一 staging 到系统盘根 |
| 卸载返回成功但盘符仍在 | `TryDismountIso` 的 15 秒等待 | 手工卸载或重启后再制作 |
| 直接调用枚举抛异常 | 是否执行 `EnsureInitialized` | 在调用前初始化 wimlib |
| 选择 SWM 后版本不全 | 是否缺少 `.2.swm` 等分卷 | 合并为单 WIM 后重试 |

## 相关 skill

- `firewin-overview`：给出 `payload`、`dist` 与构建命令，本 skill 只处理运行时镜像链路。
- `firewin-desktop-ui`：解释浏览、版本页、分区页与日志控件如何调用本层方法。
- `firewin-bcd-boot`：承接 `DeployAsync` 之后的 `PeBootRequest`、`deploy.json` 与临时引导。
- `firewin-cli-automation`：`--demo-image` 参数复用本层的 `ResolveImageAsync` 做无对话框验证。
