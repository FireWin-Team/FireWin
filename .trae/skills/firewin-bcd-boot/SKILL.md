---
name: firewin-bcd-boot
description: BCD PE boot for FireWin covering bcdedit sequence, deploy.json contract, rollback, cleanup and firmware detection. Use when changing boot entries or deploy config, not for WPF UI or ISO mounting.
---
# FireWin BCD 与临时引导

本 skill 解释桌面端如何把 PE 放进 `C:\FireWinPE`、写一次性 `bootsequence` 与 `deploy.json`，以及 PE 端如何消费并清理这些内容。

## 适用范围

- 修改 `PreparePeBootAsync` 的 `bcdedit` 命令顺序或参数。
- 调整 `deploy.json` 字段、序列化策略与 PE 端解析契约。
- 处理 `{ramdiskoptions}`、临时启动项、失败回滚与清理。
- 排查 UEFI 与 Legacy 加载器路径、`detecthal`、固件检测差异。
- 定位盘符漂移问题，使用卷 GUID 路径跨环境传参。
- 划清桌面端准备与 `pe/boottool.cpp`、`pe/main.cpp` 的职责边界。

## 不适用范围

- 窗口、按钮状态与日志显示，转到 firewin-desktop-ui。
- ISO 挂载、WIM 索引与镜像复制，转到 firewin-image-deploy。
- U 盘的独立 BCD 写入与分区制作，属于 `UsbBootService`，不走本服务。

## 关键文件与符号

| 路径 | 作用 | 关键类型/方法 |
|---|---|---|
| `src/FireWin/Services/BcdBootService.cs` | 引导准备总入口 | `PreparePeBootAsync`、`DefaultStagingDirectory` |
| `src/FireWin/Services/BcdBootService.cs` | 部署参数字典 | `DeployConfiguration`、`DeployJsonOptions` |
| `src/FireWin/Services/BcdBootService.cs` | 请求与会话契约 | `PeBootRequest`、`PeBootSession` |
| `src/FireWin/Services/BcdBootService.cs` | 清理与回滚 | `RemoveTemporaryEntryAsync`、`RollbackAsync` |
| `src/FireWin/Services/BcdBootService.cs` | 固件与卷路径 | `DetectFirmwareType`、`TryGetVolumeRoot`、`TryBuildVolumePath` |
| `src/FireWin/Services/BcdBootService.cs` | 落地目录复制 | `CopyPePayload`、`CopyDirectory` |
| `src/FireWin/Services/BcdBootService.cs` | 输入校验与命令执行 | `ValidateRequest`、`RunBcdEditAsync`、`TryRunBcdEditAsync` |
| `src/FireWin/Services/BcdBootService.cs` | 进程结果封装 | `ProcessResult(int ExitCode, string Output)` |
| `src/FireWin/Services/BcdBootService.cs` | kernel32 声明 | `NativeMethods.GetFirmwareType`、`GetVolumeNameForVolumeMountPoint` |
| `src/FireWin/Services/BcdBootService.cs` | 异常类型 | `BcdOperationException` |
| `pe/main.cpp` | PE 端读取配置并真正部署 | `LoadConfig`、`LocateConfig`、`Deploy` |
| `pe/main.cpp` | PE 端参数入口 | `/autodeploy`、`/shell`、`/config:<path>` |
| `pe/boottool.cpp` | 独立引导工具，可手工写/删引导 | `WritePeBoot`、`RemovePeBoot`、`RepairBoot` |
| `pe/boottool.cpp` | GUID 持久化文件 | `bootentry.txt`、`SaveGuid`、`LoadSavedGuid` |

## 核心流程

1. 前置检查：`PreparePeBootAsync` 先 `EnsureAdministrator()`，再要求 `OperatingSystem.IsWindows()`，最后 `ValidateRequest`。
2. 校验源目录：`PeSourceDirectory` 必须存在，且包含 `boot.wim`、`boot.sdi`、`Tools\FireWinPE.exe`；`boot.sdi` 缺失时报错会提示从 ADK 的 `Windows Preinstallation Environment\amd64\Media\Boot\` 复制。
3. 规范化路径：`staging = Path.GetFullPath(request.StagingDirectory)`，`systemDrive = NormalizeDrive(request.SystemDrive)`（单字符补 `:`，统一大写，去掉尾部 `\`）。
4. 分区一致性：取 `stagingRoot = GetRootPath(staging)`，要求其与 `systemDrive` 相等，否则抛“落地目录 … 不在系统分区 … 上”。
5. 求相对路径：`stagingRelative = "\" + staging.Substring(stagingRoot.Length).Trim('\')`，例如 `C:\FireWinPE` 得到 `\FireWinPE`。
6. 检测固件：`DetectFirmwareType()` 优先调用 `GetFirmwareType`，老系统回退 `bcdedit /enum {current}` 中是否含 `winload.efi` 或 `winload.exe`。
7. 复制 payload：`CopyPePayload` 把 `boot.wim`、`boot.sdi` 复制到 staging，并递归复制 `Tools`；同尺寸文件跳过。
8. 补齐部署参数：`WimVolumePath ??= TryBuildVolumePath(WimPath)`、`TargetVolume ??= TryGetVolumeRoot(TargetDrive)`、`SystemVolume ??= TryGetVolumeRoot(systemDrive)`，`Firmware` 按检测结果写 `UEFI` 或 `BIOS`，`LogPath = staging\deploy.log`。
9. 准备 ramdisk 选项：`bcdedit /enum {ramdiskoptions}` 失败时执行 `bcdedit /create {ramdiskoptions} /d "FireWin RAMDisk Options"`，并记录 `ramdiskOptionsCreated=true`；已存在则复用。
10. 指向 boot.sdi：`bcdedit /set {ramdiskoptions} ramdisksdidevice partition=C:`，再 `bcdedit /set {ramdiskoptions} ramdisksdipath \FireWinPE\boot.sdi`。
11. 创建加载器项：`bcdedit /create /d "FireWin PE" /application osloader`，用正则 `\{[0-9a-fA-F]{8}-...-[0-9a-fA-F]{12}\}` 从输出解析 GUID。
12. 绑定 WIM：`wimOnDisk = ramdisk=[C:]\FireWinPE\boot.wim,{ramdiskoptions}`，分别写入该 GUID 的 `device` 与 `osdevice`。
13. 写加载器属性：UEFI 用 `path \Windows\System32\boot\winload.efi`，BIOS 用 `path \Windows\System32\winload.exe`；随后 `systemroot \Windows`、`winpe yes`，仅 BIOS 额外 `detecthal yes`。
14. 写 deploy.json：`deploy.BootEntryGuid = entry`，`JsonSerializer.Serialize` 使用 camelCase、忽略 null、缩进，并以 `new UTF8Encoding(false)` 写入 `staging\deploy.json`。
15. 设置一次性引导：`bcdedit /bootsequence {GUID}`，只影响下一次启动，不改变默认项，然后返回 `PeBootSession`。
16. 失败回滚：`try/catch` 中调用 `RollbackAsync(createdEntry, ramdiskOptionsCreated)`，删除已创建项与本次创建的 `{ramdiskoptions}`，再重新抛出。
17. 收尾：桌面端可调 `RestartComputerAsync` 执行 `shutdown.exe /r /t 0`；PE 部署完成后由 `main.cpp` 用 `bcdedit /delete {GUID} /f` 清理临时项。
18. 路径规范化细节：`GetRootPath` 遇到 `C:` 会补成 `C:\`，因为 `GetVolumeNameForVolumeMountPoint` 只接受带尾斜杠的挂载点。
19. 清理容错：PE 端删除临时项失败只记 `WARN` 并继续后续重启，不把整个部署判为失败。

## 数据结构与契约

`DeployConfiguration` 字段（JSON 全为 camelCase）：

| 字段 | 类型/默认值 | 用途 |
|---|---|---|
| `WimPath` | `string.Empty` | 桌面端原始镜像路径，`DeployAsync` 传入 staging 内的 `install.wim` |
| `WimVolumePath` | `string?` | 卷 GUID 路径，PE 端优先使用以规避盘符漂移 |
| `WimIndex` | `int = 1` | 传给 `wimlib-imagex apply` 的索引 |
| `TargetDrive` | `string.Empty` | 目标系统分区盘符，如 `D:` |
| `TargetVolume` | `string?` | 目标分区卷 GUID 根，PE 端据此重解析盘符 |
| `SystemDrive` | `string.Empty` | 引导/系统分区盘符，如 `C:` |
| `SystemVolume` | `string?` | 系统分区卷 GUID 根 |
| `Firmware` | `string = "UEFI"` | 只写 `UEFI` 或 `BIOS`，PE 端按小写比较 |
| `BootEntryGuid` | `string.Empty` | 桌面端临时项 GUID，供 PE 端删除 |
| `LogPath` | `string.Empty` | PE 端日志，固定 `staging\deploy.log` |
| `AutoReboot` | `bool = true` | PE 部署完成后是否 `shutdown /r /t 15` |

其他契约：

- `PeBootRequest`：`PeSourceDirectory`（required）、`StagingDirectory`（默认 `C:\FireWinPE`）、`SystemDrive`（required）、`Deployment`（required）。
- `PeBootSession(string BootEntryGuid, string StagingDirectory, string DeployConfigPath, FirmwareType Firmware, bool RamdiskOptionsCreated)`。
- `FirmwareType`：`Unknown = 0`、`Bios = 1`、`Uefi = 2`。
- 关键常量：`DefaultStagingDirectory = C:\FireWinPE`、`RamdiskOptionsAlias = {ramdiskoptions}`、GUID 正则含两侧花括号。
- PE 端配置查找顺序：`/config:` 显式路径 → exe 同目录 `deploy.json` → `C:\FireWinPE\deploy.json` → C 到 Z 各分区下的 `\FireWinPE\deploy.json`。
- `boottool.cpp` 把 GUID 持久化到 exe 同目录 `bootentry.txt`，删除引导时先读该文件。
- `TryBuildVolumePath` 返回 `\\?\Volume{GUID}\相对路径`；`TryGetVolumeRoot` 返回 `\\?\Volume{GUID}\`，二者都依赖 `GetVolumeNameForVolumeMountPoint`。
- `BootEntryGuid` 保留两侧花括号，与 `bcdedit` 输出格式一致，PE 端可直接拼 `/delete`。
- `RunBcdEditAsync` 把 stdout 与 stderr 合并后 `Trim`，失败信息包含完整命令行与退出码。
- PE 端 `Deploy` 顺序：解析镜像 → 解析目标分区 → `wimlib-imagex apply <wim> <index> <target>\ --check` → `bcdboot <target>\Windows /s <system> /f UEFI|BIOS` → 删除 `bootEntryGuid` → 可选重启。

## 约束与坑

1. 现象：部署直接报“落地目录不在系统分区”。原因：ramdisk 的 `[C:]` 必须与 boot.wim 实际所在分区一致，代码显式校验。正确做法：让 `StagingDirectory` 与 `SystemDrive` 指向同一分区，不要固定假设 C:。
2. 现象：写入引导成功但 PE 无法启动。原因：`boot.sdi` 是 ramdisk 必需的磁盘映像，缺它必失败。正确做法：确保 staging 与源目录都有 `boot.sdi`，缺失时按报错从 ADK 复制。
3. 现象：清理后其它工具的 ramdisk 引导也失效。原因：`{ramdiskoptions}` 是全局共享项，代码在已存在时复用并覆盖 `ramdisksdidevice`/`ramdisksdipath`，但 `RamdiskOptionsCreated=false` 时不会删除。正确做法：接受“只覆盖不删除”的语义，或部署后重新配置原值。
4. 现象：UEFI 机器上写了 `winload.exe` 导致无法启动。原因：`PreparePeBootAsync` 用 `firmware == Uefi ? UEFI : BIOS`，`Unknown` 会落到 BIOS；而 `boottool.cpp` 用 `firmware != 1` 把未知当 UEFI，`MainWindow` 提示也写“未知按 UEFI 处理”，三处语义不一致。正确做法：以 `DetectFirmwareType` 的实际返回为准，未知时先确认再决定，不要依赖界面文案。
5. 现象：系统启动时提示找不到启动项。原因：`RemoveTemporaryEntryAsync` 只删 GUID 与可选 `{ramdiskoptions}`，不清 `{bootmgr}` 的 `bootsequence`；`boottool.cpp.RemovePeBoot` 才有 `/deletevalue {bootmgr} bootsequence`。正确做法：清理后检查 `bcdedit /enum {bootmgr}`，必要时手工清空一次性引导值。
6. 现象：PE 读 deploy.json 报 JSON 解析失败。原因：PE 端是“极简 JSON 解析”，只支持扁平对象，且桌面端以 UTF-8 无 BOM 写入。正确做法：不要新增嵌套对象、数组或注释，保持字段扁平且值类型简单。
7. 现象：替换 boot.wim 后 PE 仍是旧界面。原因：`CopyPePayload` 以文件长度相同判定“已复制过”。正确做法：重建后核对字节数，必要时删除 staging 下 `boot.wim` 再部署。
8. 现象：staging 直接放在分区根时生成 `\\boot.sdi` 双反斜杠。原因：`stagingRelative` 为空时先置 `\`，再拼接 `\boot.sdi`。正确做法：避免把 staging 设为 `C:\`，保留一个子目录。
9. 现象：`bcdedit /create` 成功但回滚没删掉孤儿项。原因：`createdEntry` 在 `CreateOsLoaderEntryAsync` 返回后才赋值，GUID 解析失败时 `createdEntry` 仍为 null，`RollbackAsync` 无从删除。正确做法：失败后手工 `bcdedit /enum` 检查 `FireWin PE` 并删除。
10. 现象：重复调用 `PreparePeBootAsync` 带上旧卷路径。原因：`WimVolumePath`、`TargetVolume`、`SystemVolume` 使用 `??=`，已有值不会被新路径覆盖。正确做法：每次部署新建 `DeployConfiguration`，不要跨任务复用一个实例。
11. 现象：`SystemDrive` 传成 `C:\Windows` 后生成非法 `partition=C:\WINDOWS`。原因：`NormalizeDrive` 只去尾部反斜杠并大写，不校验是否单盘符。正确做法：只传 `C` 或 `C:`。
12. 现象：老系统上固件检测返回 `Unknown`。原因：`GetFirmwareType` 不存在时回退 `bcdedit` 文本匹配，文本被本地化或格式变化会失配。正确做法：未知时人工确认固件类型，并注意第 4 条的默认差异。
13. 现象：`deploy.json` 里卷 GUID 字段为 null。原因：`GetVolumeNameForVolumeMountPoint` 对网络路径、未挂载卷或非本地盘返回失败，`TryGetVolumeRoot` 返回 null。正确做法：保证镜像与目标分区是本地已挂载卷，并保留盘符字段作为回退。
14. 现象：调用方取消后回滚没有执行完。原因：`RollbackAsync` 内部固定使用 `CancellationToken.None`，本身不会被取消，但外层进程若被强杀仍会中断。正确做法：不要在写入 BCD 期间强制结束进程，失败后手工核对残留项。

## 验证清单

- [ ] 非管理员运行 `PreparePeBootAsync` 抛出 `BcdOperationException`，提示需要管理员权限。
- [ ] 缺少 `boot.sdi` 或 `Tools\FireWinPE.exe` 时在复制前就失败，且错误信息包含具体路径。
- [ ] staging 与 `SystemDrive` 不同分区时被拒绝，不写入任何 BCD 项。
- [ ] `bcdedit /enum {ramdiskoptions}` 能看到正确的 `ramdisksdidevice` 与 `ramdisksdipath`。
- [ ] 新建的 `FireWin PE` 项 `device`/`osdevice` 指向 `ramdisk=[C:]\FireWinPE\boot.wim,{ramdiskoptions}`。
- [ ] UEFI 使用 `winload.efi`，BIOS 使用 `winload.exe` 且带 `detecthal yes`。
- [ ] `deploy.json` 为无 BOM UTF-8，字段为 camelCase，`firmware` 只有 `UEFI` 或 `BIOS`。
- [ ] `bcdedit /enum {bootmgr}` 的 `bootsequence` 指向本次 GUID，默认启动项未改变。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 报“bcdedit 需要管理员权限” | 当前进程提权状态 | 以管理员身份重启程序 |
| 报“缺少 boot.sdi” | 源目录与 staging 的 `boot.sdi` | 从 ADK Media\Boot 复制 |
| 重启后没有进入 PE | `bootsequence`、`device`、`osdevice` | 按验证清单逐项核对 BCD |
| 进入 PE 后找不到镜像 | `deploy.json` 的 `wimPath`/`wimVolumePath` | 确认 staging 与卷 GUID 路径存在 |
| PE 提示 JSON 解析失败 | 是否新增嵌套结构、是否带 BOM | 改回扁平字段并用无 BOM 写入 |
| 部署完成后仍循环进 PE | 临时 GUID 是否删除、`bootsequence` 是否清空 | 执行 `bcdedit /delete {GUID} /f` 并清 bootsequence |
| 写入失败后残留 FireWin PE 项 | `createdEntry` 是否为 null、`bcdedit /enum` | 手工删除残留项并重建 |
| 卷 GUID 路径为 null | 分区是否本地已挂载、路径是否网络盘 | 改用本地卷并保留盘符回退 |
| UEFI 机器报 winload 错误 | `firmware` 字段与 `path` 值 | 强制写 `UEFI` 并确认加载器为 `.efi` |

## 相关 skill

- `firewin-overview`：给出仓库层与 ADK、payload 的来源，本 skill 依赖其 `payload\pe\` 六件套。
- `firewin-desktop-ui`：解释确认页、完成页、重启按钮如何触发本服务。
- `firewin-image-deploy`：解释 `DeployConfiguration.WimPath` 在进入本服务前如何生成。
- `firewin-cli-automation`：给出 `deploy.json` 的完整键表、布尔写法与 PE 端退出码。
