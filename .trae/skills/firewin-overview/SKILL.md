---
name: firewin-overview
description: Repository map for FireWin covering source to payload to dist chain, artifact sizes, prerequisites and build commands. Use when locating files or planning builds; not for UI or BCD details.
---
# FireWin 仓库总览与导航

FireWin 是“PE 启动 U 盘制作 + Windows 桌面端离线部署”的二合一仓库，本 skill 负责给出权威来源链、产物矩阵与命令入口。

## 适用范围

- 需要判断某个改动会影响哪一层（PE 原生端、payload、桌面端 exe、dist 分发物）。
- 需要定位源码、脚本、构建工具或已发布的二进制文件。
- 需要按顺序重建 PE、payload 与桌面端，并核对产物体积。
- 需要确认运行前置条件（管理员、x64、payload、ADK、MSVC/CMake）。
- 需要找到桌面端 CLI 开关或 PE 端启动参数。
- 需要给新成员解释仓库顶层目录职责。

## 不适用范围

- 具体窗口导航、按钮事件与线程约定，转到 firewin-desktop-ui。
- ISO 挂载、WIM 索引解析、分区枚举的实现细节，转到 firewin-image-deploy。
- bcdedit 命令序列、deploy.json 字段与回滚逻辑，转到 firewin-bcd-boot。

## 关键文件与符号

| 路径 | 作用 | 关键类型/方法 |
|---|---|---|
| `src/FireWin/FireWin.csproj` | 桌面端发布配置；把 payload 复制到输出与发布目录 | `CopyBundledPayloadToPublish`、`Content payload\**\*`、`ExcludeFromSingleFile` |
| `src/FireWin/App.xaml.cs` | 图形界面与命令行双入口 | `OnStartup`、`RunCli`、`NativeConsole.AttachToParent` |
| `src/FireWin/Services/BundledPayload.cs` | 校验随包成品 PE 是否齐全 | `Root`、`PeDirectory`、`TryGetPePayload` |
| `src/FireWin/Services/WimInitService.cs` | 定位并初始化 libwim-15.dll | `EnsureInitialized`、`ResolveNativeLibraryPath` |
| `src/FireWin/MainWindow.xaml.cs` | 六页安装向导与一键部署入口 | `GoToPage`、`ResolveImageAsync`、`DeployAsync` |
| `src/FireWin/UsbWindow.xaml.cs` | U 盘制作与 ISO 生成双模式界面 | `CreateUsbAsync`、`CreateIsoAsync`、`SetMode` |
| `src/FireWin/Services/DeployService.cs` | ISO 挂载、WIM 枚举、分区枚举、镜像复制 | `MountIsoAsync`、`ListWimImages`、`GetFixedPartitions` |
| `src/FireWin/Services/BcdBootService.cs` | 组装 PE 落地目录、写 deploy.json、写一次性引导 | `PreparePeBootAsync`、`PeBootSession` |
| `src/FireWin/Services/UsbBootService.cs` | U 盘枚举、分区、写 PE、独立 BCD | `ListUsbDisks`、`CreateBootDiskAsync`、`UpgradeBootDiskAsync` |
| `src/FireWin/Services/IsoImageService.cs` | 自研 ISO9660 + El Torito 写入器 | `CreatePeIsoAsync`、`HostBcdAvailable` |
| `pe/CMakeLists.txt` | PE 原生端构建，强制静态 CRT | `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` |
| `pe/main.cpp` | PE 桌面外壳、deploy.json 解析与部署 | `Deploy`、`LoadConfig`、`/autodeploy`、`/config:` |
| `pe/boottool.cpp` | FireBoot 独立引导工具 | `WritePeBoot`、`RemovePeBoot`、`RepairBoot` |
| `scripts/build-minipe.ps1` | 生成 boot.wim、ISO，并可组装 payload | `-Language`、`-OutputIso`、`-AssemblePayload`、`-AppsDirectory` |
| `scripts/install-adk.ps1` | 静默安装 ADK 主包与 WinPE 附加组件 | 两个独立安装程序 |
| `payload/pe/` | 随桌面端分发的成品 PE（唯一内置运行资源） | `boot.wim`、`boot.sdi`、`Tools\` |
| `tools/wimlib-imagex.exe` | PE 端应用镜像的命令行工具 | 与 `libwim-15.dll` 成对分发 |
| `dist/desktop/` | 已发布单文件 exe，同级带 `payload\pe\` | `FireWin.exe` |
| `.buildtools/` | 仓库内便携工具链，避免依赖全局安装 | `dotnet\dotnet.exe`、`cmake\cmake-4.5.0-windows-x86_64\bin\cmake.exe` |

## 核心流程

1. 先改权威来源：PE 行为改 `pe/main.cpp`、`pe/boottool.cpp`；PE 构建参数改 `pe/CMakeLists.txt`；PE 内容与驱动注入改 `scripts/build-minipe.ps1`。
2. 编译 PE 端（需 MSVC x64；仓库带便携 CMake）：
   `cmake -S pe -B pe\build -G "Visual Studio 17 2022" -A x64`
   `cmake --build pe\build --config Release`
   产物为 `pe\build\bin\FireWinPE.exe` 与 `pe\build\bin\FireBoot.exe`。
3. 用 `dumpbin /dependents pe\build\bin\FireWinPE.exe` 验收，不应出现 `VCRUNTIME140.dll`、`MSVCP140.dll`。
4. 重新制作 PE 并回填 payload（需先装好 ADK 主包 + WinPE 附加组件）：
   `.\scripts\install-adk.ps1`
   `.\scripts\build-minipe.ps1 -Language zh-cn -OutputIso .\dist\FireWinPE.iso -AssemblePayload`
5. 确认 payload 六件套齐全：`boot.wim`、`boot.sdi`、`Tools\FireWinPE.exe`、`Tools\wimlib-imagex.exe`、`Tools\libwim-15.dll`、`Tools\7za.exe`。
6. 发布桌面端（有系统 .NET SDK 时）：
   `dotnet publish src\FireWin\FireWin.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true`
   无 SDK 时用 `.buildtools\dotnet\dotnet.exe` 执行同一条命令。
7. 发布后检查 `dist\desktop\FireWin.exe` 与 `dist\desktop\payload\pe\` 同时存在；payload 由 csproj 的 `CopyBundledPayloadToPublish` 在 `AfterTargets="Publish"` 复制。
8. 用 CLI 自检：`FireWin.exe --version`、`FireWin.exe --wim-init-check`、`FireWin.exe --adk-status`（就绪返回 0，未就绪返回 2）。
9. 需要自动化界面验证时用内部演示参数：`FireWin.exe --demo-image <镜像路径>`，直接进入 MainWindow 镜像页并完成识别。
10. 需要验证 ADK 静默安装时用 `FireWin.exe --adk-install`（管理员），它与 `scripts/install-adk.ps1` 是两条等价入口。
11. 生成 ISO 走 `UsbWindow` 的“生成 ISO 文件”模式，默认输出到 exe 同级 `FireWinPE.iso`；只有宿主没有可复用 BCD 时才要求管理员。
12. 分发前核对 `dist\desktop\FireWin.exe`、`dist\desktop\payload\pe\`、`dist\FireWinPE.iso` 三类产物同时存在，再打包压缩。

## 数据结构与契约

路径常量：

- `BundledPayload.Root` = `AppContext.BaseDirectory\payload`。
- `BundledPayload.PeDirectory` = `AppContext.BaseDirectory\payload\pe`。
- `BcdBootService.DefaultStagingDirectory` = `C:\FireWinPE`。
- U 盘镜像目录名 `UsbBootService.ImageFolderName` = `FWIMG`；PE 端优先扫 `\FWIMG`，兼容 `\Images`。
- U 盘双分区卷标固定为 `FireWinPE`（FAT32 启动分区，写完后隐藏）与 `FireData`（exFAT 数据分区）。
- 启动分区下限 `MinBootPartitionMb` = 1536 MB，双分区时为数据分区预留 4096 MB。
- 桌面端默认 ISO 输出 = exe 同级的 `FireWinPE.iso`；ISO9660 单文件上限 4 GB。

产物矩阵（当前工作区实测字节数，用于回归核对）：

| 产物 | 路径 | 预期大小 |
|---|---|---|
| PE 镜像 | `payload\pe\boot.wim` | 1,116,400,802 字节（约 1.04 GB） |
| ramdisk 盘 | `payload\pe\boot.sdi` | 3,170,304 字节（约 3.0 MB） |
| PE 主程序 | `payload\pe\Tools\FireWinPE.exe` | 3,803,648 字节 |
| 独立引导工具 | `payload\pe\Tools\FireBoot.exe` | 688,640 字节 |
| wimlib 命令 | `payload\pe\Tools\wimlib-imagex.exe` | 137,230 字节 |
| wimlib 原生库 | `payload\pe\Tools\libwim-15.dll` | 483,854 字节 |
| 7-Zip 命令 | `payload\pe\Tools\7za.exe` | 1,334,784 字节 |
| 桌面端 exe | `dist\desktop\FireWin.exe` | 66,767,646 字节（约 63.7 MiB） |
| PE ISO | `dist\FireWinPE.iso` | 1,127,948,288 字节（约 1.05 GiB） |
| UDF 变体 ISO | `dist\FireWinPE.udf.iso` | 1,145,962,496 字节 |

## 约束与坑

1. 现象：发布的单文件 exe 里没有 PE，运行后提示 payload 缺失。原因：csproj 给 payload 设了 `ExcludeFromSingleFile=true`，不能用单文件承载几百 MB 成品。正确做法：payload 必须与 exe 同级保留目录，发布目标 `CopyBundledPayloadToPublish` 负责复制。
2. 现象：发布目录没有 `payload` 但构建没有报错。原因：`Content` 与复制目标都带 `Condition="Exists('...\payload')"`，仓库没有 payload 时静默跳过。正确做法：先执行 `build-minipe.ps1 -AssemblePayload`，再 publish。
3. 现象：`DllNotFoundException` 或 `BadImageFormatException`。原因：libwim-15.dll 只有 x64 版本，csproj 固定 `Platforms/PlatformTarget/RuntimeIdentifier` 为 x64，任何 32 位宿主都会失败。正确做法：保持 x64 发布，并用 `--wim-init-check` 查看实际加载路径。
4. 现象：单文件 exe 运行时找不到 libwim-15.dll。原因：单文件默认不落地原生库，csproj 注释明确要求 `IncludeNativeLibrariesForSelfExtract=true`。正确做法：不要关闭该属性；`WimInitService` 还会在 `NATIVE_DLL_SEARCH_DIRECTORIES` 自解压目录中兜底查找。
5. 现象：更新 PE 后重新部署，`C:\FireWinPE\boot.wim` 仍是旧文件。原因：`CopyPePayload` 以“文件长度相同”判定已复制。正确做法：PE 重新构建后比较字节数，必要时手工删除 staging 再部署。
6. 现象：传入任意参数后没有图形界面。原因：`App.OnStartup` 只在无参数时打开 `LauncherWindow`，任何非 `--demo-image` 参数都进入 `RunCli`。正确做法：验证 CLI 用明确的受支持开关，不要传未知参数。
7. 现象：脚本/管道捕获 `--version` 等输出为空。原因：WinExe 无控制台，且输出被重定向时不能再 `AttachConsole`。正确做法：重定向场景依赖标准输出；交互场景才允许附加父控制台。
8. 现象：系统装在 D 盘时部署报“落地目录不在系统分区”。原因：staging 固定为 `C:\FireWinPE`，而 `PreparePeBootAsync` 要求它与 `SystemDrive` 同分区。正确做法：部署前确认系统盘为 C:，或调整 staging 与 `SystemDrive` 保持一致。
9. 现象：重新打包 PE 时 DISM 报依赖缺失或中文显示方块。原因：WinPE-PowerShell 依赖 WinPE-Scripting、WinPE-NetFx 依赖 WinPE-WMI；zh-cn 还需 WinPE-FontSupport-ZH-CN。正确做法：按依赖顺序注入，并在 `-Language zh-cn` 时保留中文字体包。
10. 现象：ADK 检测说主包已装但仍无法 `copype`。原因：ADK 主包与 WinPE 附加组件是两个独立安装程序。正确做法：两个都安装，`--adk-status` 只有 `IsReady` 才返回 0。
11. 现象：ISO 生成成功但装不进大于 4 GB 的 install.wim。原因：`UsbWindow` 的 ISO 模式提示里写明 ISO 内单个文件上限 4 GB。正确做法：改用 `install.esd`，或直接做 U 盘并把镜像放进 `\FWIMG`。
12. 现象：“升级启动盘”按钮始终灰色。原因：`CanUpgradeInPlace` 只认卷标 `FireWinPE`/`FireData`，或根目录已有 `\sources\boot.wim` 的盘。正确做法：非本工具制作的盘只能“全新制作”。

## 验证清单

- [ ] `dotnet publish ... -p:PublishSingleFile=true` 成功，输出目录存在 `FireWin.exe` 与 `payload\pe\`。
- [ ] `FireWin.exe --version` 输出 `FireWin 1.0.0 (GPL-3.0-only)`。
- [ ] `FireWin.exe --wim-init-check` 输出 `[OK] wimlib 已初始化` 及原生库路径。
- [ ] `FireWin.exe --adk-status` 的退出码与就绪状态一致（0 或 2）。
- [ ] `BundledPayload.TryGetPePayload` 的六项文件全部存在，`LauncherWindow` 页脚显示“内置 PE 就绪”。
- [ ] `pe\build\bin\FireWinPE.exe` 的 `dumpbin /dependents` 输出无 VC 运行库。
- [ ] `payload\pe\boot.wim`、`dist\desktop\FireWin.exe`、`dist\FireWinPE.iso` 大小与产物矩阵一致。
- [ ] 改动 PE 源码后，`build-minipe.ps1 -AssemblePayload` 能同时刷新 `payload\pe\`。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 启动提示内置 PE 不完整 | `payload\pe\` 六项文件、`TryGetPePayload` 返回的缺项名 | 用完整分发包，或重建 payload |
| 桌面端双击无界面 | 启动参数、`OnStartup` 分支 | 去掉参数或改用受支持 CLI 开关 |
| wimlib 初始化失败 | 进程位数、`libwim-15.dll` 位置、环境变量 `OPENWININSTALLER_LIBWIM` | 改用 x64 发布，补齐原生库 |
| 发布目录缺 payload | csproj 的 `Condition`、源 `payload` 是否存在 | 先生成 payload 再 publish |
| PE 启动后仍进旧系统 | `C:\FireWinPE\boot.wim` 时间戳、BCD `/bootsequence` | 清理 staging 重新部署，检查 BCD 临时项 |
| U 盘启动无 Legacy 引导 | `bootsect.exe` 搜索路径、启动分区是否活动 | 安装 ADK 提供 bootsect，或只用 UEFI 验证 |
| ISO 生成后虚拟机不启动 | `IsoImageService` 的 El Torito 与 `sources\boot.wim` | 用支持的虚拟机做 UEFI 验证，必要时重生成 |
| U 盘升级按钮灰色 | `UsbBootService.CanUpgradeInPlace` 与卷标 | 改选 FireWin 启动盘，或执行全新制作 |

## 相关 skill

- `firewin-desktop-ui`：负责 L1 桌面端窗口、页面与事件；本 skill 只到文件与构建层。
- `firewin-image-deploy`：负责镜像解析、分区与复制；本 skill 的 `DeployService` 行可跳转过去。
- `firewin-bcd-boot`：负责 `BcdBootService` 的 bcdedit 序列与 deploy.json 契约。
- `firewin-wpf-styling`：负责色板、控件模板、进度与日志面板的视觉实现。
- `firewin-cli-automation`：负责桌面端与 PE 端命令行、退出码与无人值守部署。
