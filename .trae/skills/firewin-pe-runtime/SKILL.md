---
name: firewin-pe-runtime
description: Troubleshoot FireWin PE runtime modes, display selection, drvload loading, wimlib apply, bcdboot and FireBoot BCD entries. Use for main.cpp and boottool.cpp behavior. Not for image builds.
---

# FireWin PE 运行时与部署主程序

main.cpp 是 PE 内唯一入口，负责桌面 / 经典启动器 / 无人值守三种模式、加载页、驱动预加载、系统镜像应用与一次性引导清理。

## 适用范围

- 修改或排查 `wmain` 的启动模式、未知参数行为、deploy.json 定位顺序。
- 调整加载页自动分辨率候选、驱动加载进度、跳过逻辑。
- 排查 PE 缺少磁盘 / 触控板 / 虚拟机网卡驱动时的 `drvload` 路径。
- 排查「安装系统」镜像扫描、版本枚举、目标与引导分区预选。
- 排查 `wimlib-imagex apply` 退出码、12-88% 进度映射、`bcdboot` 写引导失败。
- 维护 FireBoot 写入 / 删除 PE 引导项、修复系统引导、bootentry.txt。

## 不适用范围

- 构建 boot.wim、注入驱动、打包内置工具，见 firewin-driver-injection 与 firewin-pe-apps。
- 桌面端 C# 四步向导、U 盘写入与 ISO 生成，见 firewin-usb-boot。

## 关键文件与符号

| 路径 | 作用 | 关键符号 |
|---|---|---|
| pe\main.cpp | 模式分发与部署主流程 | `wmain`、`RunDesktopShell`、`RunLauncher`、`Deploy`、`ShowResult` |
| pe\main.cpp | 加载页与分辨率 | `RunSplashStage`、`InitializePeBackground`、`ApplyBestDisplayMode`、`TryApplyDisplayMode`、`EnumerateDisplayModes` |
| pe\main.cpp | 驱动加载 | `EnumerateDriverStorePackages`、`CollectInboxDriverInfs`、`CollectBootDriverInfs`、`CollectInfFiles`、`FindInfFilesOnVolumes` |
| pe\main.cpp | 工具扫描 | `ScanAppTools`、`PickAppExe`、`IsX64Executable`、`kKnownApps`、`HasStaticButtonFor` |
| pe\main.cpp | 安装向导 | `FindImagesOnVolumes`、`DetectFirmwareKind`、`RunInstallWizard`、`WizardRunDeploy` |
| pe\main.cpp | 配置与卷解析 | `LocateConfig`、`LoadConfig`、`ResolveVolumeToDriveLetter`、`IsUsableDrive` |
| pe\boottool.cpp | FireBoot 引导工具 | `DetectFirmwareType`、`FindPeSources`、`WritePeBoot`、`RemovePeBoot`、`RepairBoot`、`SaveGuid` |
| pe\CMakeLists.txt | 两个 x64 目标、静态 CRT | `FireWinPE`（`/SUBSYSTEM:CONSOLE`）、`FireBoot`（`/SUBSYSTEM:WINDOWS`）、`/MT` |
| pe\res\app.rc | 加载页与桌面位图 | `IDB_BACKGROUND`、`IDB_LOGO` |
| pe\res\icon.rc | 程序图标 | `IDI_APPICON` |
| scripts\build-minipe.ps1 | 生成 mini PE 并组装 payload | `copype`、`dism /Add-Driver`、`-AssemblePayload` |

## 核心流程

1. `wmain` 逐项解析参数：`/autodeploy`、`-autodeploy`、`/shell`、`/config:<路径>`、`/?`、`/help`、`-h`。`/?` 弹 MessageBox 后返回 0。
2. 若 `shellMode` 为真调用 `RunLauncher`；若 `argc <= 1` 调用 `RunDesktopShell`；否则进入自动部署分支。
3. 自动部署先 `LocateConfig(configPath)`，顺序为显式路径、exe 目录 `deploy.json`、`C:\FireWinPE\deploy.json`、`C:` 到 `Z:` 各分区 `\FireWinPE\deploy.json`。为空返回 2；读配置失败返回 3。
4. 日志默认 `config.logPath`，为空用 exe 目录 `deploy.log`；`Deploy` 成功后返回 0，失败返回 1。
5. 桌面模式先 `RunSplashStage`，随后后台 `InitializePeBackground`：6% 调 `ApplyBestDisplayMode`，14% 起加载驱动，100% 完成，总超时 `kSplashTimeoutMs = 60000`。
6. 自动分辨率顺序固定为 1920x1080、2560x1440、1600x900、1366x768、1280x720；都不存在时选面积最大且不低于 1024x600、32 位色的模式；最后兜底 1024x768、800x600。每次切换前必须 `EnumDisplaySettingsW` 枚举到真实模式。
7. 驱动分两批：`CollectInboxDriverInfs(hypervisor)` 从 `X:\Windows\System32\DriverStore\FileRepository` 按 token 选 INF，顺序为存储、触控板、虚拟机网卡；再拼接 `CollectBootDriverInfs()` 的 `X:\Tools\Drivers`、各盘 `\Drivers` 与 `\FWIMG\Drivers`，排序去重。
8. 对每个 INF 调用 `X:\Windows\System32\drvload.exe "<inf>"`；退出码 0 计入成功数；每个包后进度为 `14 + (i+1)*80/n`，跳过或超时立即 break，仅记 `%TEMP%\FireWinPE-boot.log`。日志不进入 deploy.log，卡在加载页时优先看它。
9. 开机驱动顺序即优先级，存储必须先于磁盘枚举，否则 VMware 虚拟盘在「安装系统」里不可见：

| 批次 | 匹配 token | 生效条件 |
|---|---|---|
| 存储控制器 | `pvscsii.inf`、`lsi_sas.inf`、`lsi_sas2i`、`lsi_sas3i`、`stornvme.inf`、`nvmedisk.inf`、`iastorav.inf`、`amdsata.inf` | 总是加载 |
| 触控板链 | `iagpio.inf`、`ialpss2i_gpio2`、`ialpssi_gpio.inf`、`amdgpio2.inf`、`ialpss2i_i2c`、`iai2c.inf`、`amdi2c.inf`、`hidi2c.inf`、`hidspi_km.inf`、`hidinterrupt.inf`、`c_hidclass.inf` | 总是加载 |
| 虚拟机网卡 | `vmxnet3`、`nete1e3e.inf`、`netefe3e.inf`、`nete1g3e.inf`、`net1ix64.inf` | 仅非物理机 |
| 外置补充 | `X:\Tools\Drivers`、各盘 `\Drivers`、各盘 `\FWIMG\Drivers` 下所有 INF | 排序去重后追加 |
10. 桌面扫描 `toolsDir\Apps`（`toolsDir = GetExeDirectory()`）：`kKnownApps` 固定顺序生成按钮，未知目录取第一个 x64 exe 归入「其它工具」；`7-Zip`、`Notepad++`、`Explorer++`、`SDelete` 已有静态按钮，不重复扫描。
11. 「安装系统」调用 `FindImagesOnVolumes`，每盘先 `\FWIMG` 再 `\Images`，接受 `.wim`、`.esd`、`.swm`，按完整路径去重；向导列出固定分区，UEFI 下引导分区预选 ESP，BIOS 下预选目标分区。
12. `WizardRunDeploy` 写 exe 目录 `FireWinPE-install.log`，`Deploy` 再按 `deploy.json` 执行：优先 `wimVolumePath`，不存在回退 `wimPath`；目标盘优先 `targetVolume`，回退 `targetDrive`。
13. 应用镜像命令为 `wimlib-imagex.exe apply "<wim>" <index> "<目标盘>\" --check`，解析输出 `(NN%)` 映射为 `12 + p*76/100`，失败返回 wimlib 退出码。
14. 引导命令为 `bcdboot "<目标盘>\Windows" /s <引导盘> /f UEFI|BIOS`；`firmware` 除 `bios`（不区分大小写）外一律按 UEFI。
15. 成功后若 `bootEntryGuid` 非空执行 `bcdedit /delete <guid> /f`，失败仅 Warn；`autoReboot` 默认 true 时执行 `shutdown /r /t 15`。
16. FireBoot 扫描 `exe 目录`、`exe 目录\payload\pe`、每个固定分区 `\FireWinPE` 与 `\payload\pe`，目录内必须同时有 boot.wim 与 boot.sdi。写入序列见下表。

| FireBoot 步骤 | 命令 |
|---|---|
| 创建 ramdisk 选项 | `bcdedit /create {ramdiskoptions} /d "FireWin RAMDisk Options"` |
| 设置 SDI | `/set {ramdiskoptions} ramdisksdidevice partition=<盘符>:` 与 `ramdisksdipath \FireWinPE\boot.sdi` |
| 创建启动项 | `bcdedit /create /d "FireWin PE" /application osloader` |
| 指向 WIM | `device` 与 `osdevice` 均为 `ramdisk=[<盘符>:]\FireWinPE\boot.wim,{ramdiskoptions}` |
| 加载器 | UEFI 用 `\Windows\System32\boot\winload.efi`，BIOS 用 `\Windows\System32\winload.exe`；再设 `systemroot`、`winpe yes`，BIOS 加 `detecthal yes` |
| 一次性启动 | `bcdedit /bootsequence <guid>`，随后写 `bootentry.txt` |
| 删除 | `/delete <guid> /f`、`/delete {ramdiskoptions} /f`、`/deletevalue {bootmgr} bootsequence` |
| 修复引导 | `bcdboot "<Windows 分区>\Windows" /s <引导分区> /f ALL` |

## 参数或清单表

| 参数 | 行为 | 备注 |
|---|---|---|
| 无参数 | 进入桌面外壳 | 壁纸、图标、任务栏、开始菜单 |
| `/shell` 或 `-shell` | 经典单窗口启动器 | 桌面异常时的兜底入口 |
| `/autodeploy` 或 `-autodeploy` | 仅把 `autoMode` 写进日志 | 不是部署开关，见坑 3 |
| `/config:<路径>` | 指定 deploy.json | 路径不存在时直接失败，不回退 |
| `/?`、`/help`、`-h` | 显示用法并返回 0 | MessageBox，无控制台也可见 |

| deploy.json 键 | 含义 | 默认 |
|---|---|---|
| `wimVolumePath` / `wimPath` | 镜像卷 GUID 路径 / 盘符路径 | 前者优先 |
| `wimIndex` | 要应用的映像索引 | 小于等于 0 时取 1 |
| `targetVolume` / `targetDrive` | 系统分区 | 卷 GUID 优先 |
| `systemVolume` / `systemDrive` | 引导分区（ESP 或活动分区） | 卷 GUID 优先 |
| `firmware` | `UEFI` 或 `BIOS` | 空值按 `UEFI` |
| `bootEntryGuid` | 桌面端写入的一次性启动项 | 空则跳过清理 |
| `logPath` | 部署日志 | exe 目录 `deploy.log` |
| `autoReboot` | 部署后重启 | `true` |

| 输出 | 位置 | 关键内容 |
|---|---|---|
| 加载页日志 | `%TEMP%\FireWinPE-boot.log` | 虚拟机类型、最终分辨率、驱动加载 x/y 成功 |
| 安装向导日志 | exe 目录 `FireWinPE-install.log` | 镜像枚举、分区选择、部署步骤 |
| 自动部署日志 | `logPath` 或 exe 目录 `deploy.log` | wimlib 与 bcdboot 原始输出、退出码 |
| FireBoot 日志 | exe 目录 `boottool.log` | 每条 bcdedit 命令、写入或删除结果 |
| 进程退出码 | 0 成功 / 1 部署失败 / 2 未找到配置 / 3 配置解析失败 | 无参数桌面模式不经过该返回路径 |

## 约束与坑

1. 现象：`FireWinPE.exe /foo` 也进入部署并提示找不到 deploy.json。原因：`wmain` 只识别已知参数，`argc > 1` 且非 `/shell` 就落到自动部署分支。正确做法：startnet.cmd 只传受支持参数，未知参数当错误处理。
2. 现象：显式 `/config:D:\x\deploy.json` 不存在时，即使 C 盘有配置也报错返回 2。原因：`LocateConfig` 对显式路径只做 `FileExists`，失败直接返回空。正确做法：确认桌面端写入的绝对路径，或去掉 `/config:` 让程序自动搜索。
3. 现象：只传 `/autodeploy` 与不传它都会部署；反之传 `/config:` 不加 `/autodeploy` 也会部署。原因：`autoMode` 只被 `log.Info` 使用，部署由是否存在配置决定。正确做法：排查时不要依赖 `/autodeploy` 控制流程。
4. 现象：PE 里盘符与桌面端不同，按盘符部署报「目标分区不可用」。原因：配置盘符会漂移，代码优先卷 GUID 路径。正确做法：deploy.json 必须写 `wimVolumePath`、`targetVolume`、`systemVolume`；盘符字段仅作回退。
5. 现象：目标盘和引导盘选反后 apply 成功但无法启动。原因：`targetVolume` 是系统分区，`systemVolume` 是 ESP/引导分区，`bcdboot /s` 写入后者。正确做法：UEFI 选 FAT32 ESP，BIOS 选活动分区；隐藏无盘符的 ESP 需先分配盘符，`IsUsableDrive` 只认带盘符路径。
6. 现象：进度条停在 88% 很久或直接失败。原因：`apply` 输出百分比只映射到 12-88，且命令固定带 `--check`，镜像损坏会在校验阶段失败。正确做法：看 deploy.log 中 wimlib 原始输出与退出码，不要按进度判断卡死。
7. 现象：bcdboot 返回非 0，提示写引导失败。原因：目标盘 `Windows` 目录未生成、引导分区不可写或 `firmware` 写错。正确做法：确认 apply 已完成、引导分区有盘符且为 FAT32/活动分区；`firmware` 只能写 `UEFI` 或 `BIOS`。
8. 现象：部署成功后重启仍进 PE，或残留一次启动项。原因：`bcdedit /delete <guid> /f` 失败只记 Warn；或 bootentry.txt 被后一次写入覆盖，旧 GUID 丢失。正确做法：进系统后手动 `bcdedit /delete <guid> /f`，写入前先执行一次「删除 PE 引导项」。
9. 现象：加载页 60 秒后自动进入桌面，后段驱动未加载。原因：`deadline` 超时或用户按 Esc/点击跳过，循环直接 break。正确做法：用启动器「加载驱动…」补载，或精简驱动包。
10. 现象：手动「加载驱动」看不到 `\FWIMG\Drivers` 里的驱动。原因：手动入口只扫描 `X:\Drivers` 和各盘 `\Drivers`，不扫 `\FWIMG\Drivers`，也不扫内置 DriverStore。正确做法：把补充驱动放 `\Drivers`；`\FWIMG\Drivers` 只服务开机自动加载。
11. 现象：FireBoot 下拉框找不到 U 盘上的 PE。原因：`FindPeSources` 只枚举 `DRIVE_FIXED`。正确做法：把 PE 源放固定盘，或在 U 盘盘符下直接运行 FireBoot 并用 exe 目录候选。
12. 现象：BIOS 机器写入后无法启动。原因：`DetectFirmwareType` 失败返回 0，`WritePeBoot` 按 `uefi != 1` 当成 UEFI，写 `winload.efi`；而 `RepairBoot` 用 `/f ALL` 不受影响。正确做法：先修好 BCD 让 `bcdedit /enum {current}` 可读，或改用「修复系统引导」。
13. 现象：桌面有工具目录却没有按钮。原因：`PickAppExe` 只接受 `IMAGE_FILE_MACHINE_AMD64`，PE 无 WoW64，32 位 exe 被静默跳过；目录名属于静态按钮列表时也不生成动态按钮。正确做法：换 x64 版本，见 firewin-pe-apps。
14. 现象：`\FWIMG` 放了 ISO 但「安装系统」列表为空。原因：`FindImagesOnVolumes` 只识别 wim/esd/swm。正确做法：先挂载或解包 ISO 取 `sources\install.wim`，见 firewin-usb-boot 的 ISO 说明。

## 验证清单

- [ ] `cmake -S pe -B pe\build -A x64` 与 MSBuild 均成功，`dumpbin /dependents` 不出现 `VCRUNTIME140.dll`。
- [ ] `FireWinPE.exe /?` 弹出用法，进程退出码为 0。
- [ ] 无参数启动进入桌面，`/shell` 进入经典启动器，二者都能看到「安装系统」。
- [ ] PE 内 `%TEMP%\FireWinPE-boot.log` 含运行环境、分辨率、驱动加载 x/y 成功三行。
- [ ] 放好 deploy.json 后自动部署，deploy.log 记录配置路径、镜像、目标分区、引导写入。
- [ ] wimlib 进度从 12% 起、最高 88%，完成后进入 90% 写引导。
- [ ] FireBoot 写入后 `bcdedit /enum {ramdiskoptions}` 与 `bcdedit /enum {bootmgr}` 的 bootsequence 正确。
- [ ] FireBoot 删除后 GUID 项、`{ramdiskoptions}`、bootsequence 均消失，bootentry.txt 被清空。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 提示未找到 deploy.json | 显式 `/config:` 路径、exe 目录、`C:\FireWinPE` | 修正路径或去掉显式参数 |
| 目标分区不可用 | deploy.log 中卷 GUID 解析结果、是否分配盘符 | 分配盘符或改卷 GUID 配置 |
| apply 退出码非 0 | 镜像是否完整、索引是否存在 | 重新复制镜像，核对 `wimIndex` |
| bcdboot 退出码非 0 | 目标 `Windows` 目录、引导分区文件系统 | 重新 apply，格式化 ESP 为 FAT32 |
| 重启仍进 PE | bootsequence、bootEntryGuid | 手动删除启动项与 bootsequence |
| 加载页无驱动进度 | boot.log、`drvload.exe` 是否存在 | 检查 DriverStore 与驱动包完整性 |
| FireBoot 列表为空 | 固定盘、boot.wim/boot.sdi | 补齐文件或从 exe 目录运行 |
| 工具图标缺失 | `X:\Tools\Apps\<名>` 内是否有 x64 exe | 换 x64 版本或检查目录名 |

## 相关 skill

- `firewin-driver-injection`：决定 boot.wim 内置哪些驱动，本 skill 只负责运行时 drvload。
- `firewin-pe-apps`：决定 `X:\Tools\Apps` 内容，本 skill 负责扫描与按钮生成。
- `firewin-usb-boot`：桌面端制作 PE、payload 组装与临时引导写入，与本 skill 的 deploy.json 对接。
- `firewin-cli-automation`：给出 `/shell`、`/autodeploy`、`/config:` 的参数语义与退出码。
