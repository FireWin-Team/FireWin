---
name: firewin-iso-builder
description: Generate bootable FireWin PE ISO images with Windows IMAPI2FS UDF bridge plus an El Torito patch. Use for ISO output without ADK. Not for USB writing or WIM builds.
---

# FireWin PE ISO 生成（IMAPI2FS UDF 桥接盘）

用 Windows 自带的 IMAPI2FS COM 组件写出 UDF + ISO9660 + Joliet 桥接盘，再补写 El Torito 引导目录，得到 UEFI 与 Legacy BIOS 双引导的 PE ISO，全程不依赖 ADK / oscdimg。

之所以必须是 UDF 桥接盘：VMware 的 EFI 固件只把带 UDF 文件系统的光盘挂载成可读卷，纯 ISO9660 + Joliet 的盘在 EFI Shell 里只会呈现空目录，cdboot 枚举不到 `\bootmgr.efi`，直接 Not Found。旧版自研 ISO9660 写入器及其固定扇区布局（引导目录写死在固定 LBA）已随本次重写整体删除，不要再按旧文档排查。

## 适用范围

- 生成可启动 PE ISO（`IsoImageService.CreatePeIsoAsync`），可附带一个系统镜像到 `\FWIMG`。
- 排查 ISO 启动失败：EFI Shell 下空目录 / Not Found、0xc000000f、Legacy BIOS 不进引导、卷标异常。
- 调整 ISO 目录布局、引导项来源、BCD 取用与回退逻辑。
- 判断本机能否生成 ISO（`HostBcdAvailable`）以及缺哪份宿主引导文件。

## 不适用范围

- 写入 U 盘、分区、隐藏启动分区、bootsect：见 firewin-usb-boot。
- 构建或修改 `boot.wim` 本体（copype / DISM / 组件注入）：见 firewin-pe-build。
- 生成含完整 `sources\install.wim` 全量安装目录的 Windows 安装盘：本流程只产 PE 盘。
- 生成后的 ISO 刻录、Rufus 写盘等外部工具行为：不在本 skill 内。

## 关键文件与符号

| 路径 | 作用 | 关键类型/方法 |
|---|---|---|
| `src\FireWin\Services\IsoImageService.cs` | ISO 生成全部逻辑 | `CreatePeIsoAsync`、`HostBcdAvailable` |
| 同上 | IMAPI2FS 写盘 | `WriteImage`、`CreateComObject`、`IStream.Read` 循环 |
| 同上 | 宿主引导文件 | `RequireHostFile`、`FindEfiBootImage`、`FindHostBcd`、`StageFile`、`CreateHardLink` |
| 同上 | El Torito 补写 | `PatchBiosBootEntry`、`WriteBootEntry`、`ReadSector`、`ReadExtent`、`FindRecord`、`PutLE16`、`PutLE32` |
| 同上 | 常量 | `SectorSize=2048`、`BootSectorSize=512`、`FileSystemsToCreate=7` |
| `src\FireWin\Services\UsbBootService.cs` | BCD 生成与镜像目录名 | `CreateBcdStoreAtAsync`、`ImageFolderName` |
| `src\FireWin\Services\DeployService.cs` | 源 ISO 挂载/卸载 | `MountIsoAsync`、`TryDismountIso`、`FindInstallWim`、`IsoMountResult` |
| `src\FireWin\Services\BundledPayload.cs` | 内置 PE 资源 | `TryGetPePayload`、`PeDirectory` |
| `src\FireWin\UsbWindow.xaml.cs` | ISO 模式入口与管理员前置检查 | `CreateIsoAsync`、`IsAdministrator` |
| `README.md` §5.2 ② | 用户视角说明（仍为旧写入器措辞） | 布局、双引导、4 GB 提示 |

IMAPI2FS 写入参数（`WriteImage`）：

| 设置 | 值 | 说明 |
|---|---|---|
| ProgID | `IMAPI2FS.MsftFileSystemImage` | 创建失败抛 `DeployException`，提示系统组件缺失 |
| `FileSystemsToCreate` | `7` | ISO9660(1) + Joliet(2) + UDF(4)，缺 UDF 会导致 EFI 下空目录 |
| `VolumeName` | `FireWinPE` | 光盘卷标 |
| `FreeMediaBlocks` | `0` | 按内容自动扩展，不预占整张盘 |
| `Root.AddTree` | `AddTree(stage, false)` | false 表示 stage 内容直接落在根，不多套一层目录 |
| 引导镜像流 | `ADODB.Stream`，`Type=1`（adTypeBinary），`Open` 后 `LoadFromFile(efisys)` | `AssignBootImage` 只接受 IStream，传路径会类型转换失败 |
| `IMAPI2FS.BootOptions` | `AssignBootImage(stream)` | 必须在 `CreateResultImage` 之前赋值 |
| `Emulation` | `0` | 不模拟（no emulation） |
| `PlatformId` | `0xEF` | 先写 EFI 项，BIOS 项由补写阶段补上 |

宿主引导文件来源（均为 Windows 10/11 自带安装介质文件）：

| ISO 内路径 | 宿主来源 | 缺失后果 |
|---|---|---|
| `\BOOTMGR` | `C:\Windows\Boot\PCAT\bootmgr` | `RequireHostFile` 抛异常，建议改用写 U 盘 |
| `\BOOTMGR.EFI` | `C:\Windows\Boot\EFI\bootmgr.efi`，缺失回退 `bootmgfw.efi` | cdboot 加载失败，UEFI Not Found |
| `\BOOT\ETFSBOOT.COM` | `C:\Windows\Boot\DVD\PCAT\etfsboot.com` | 无法补写 BIOS 默认项 |
| `\EFI\BOOT\BOOTX64.EFI`、`\EFI\MICROSOFT\BOOT\BOOTMGFW.EFI` | `C:\Windows\Boot\EFI\bootmgfw.efi` | UEFI 引导管理器缺失 |
| 引导目录中的 efisys | `C:\Windows\Boot\DVD\EFI\en-US\efisys_noprompt.bin` 或 `efisys.bin` | 抛「缺少 efisys_noprompt.bin / efisys.bin」 |
| `\BOOT\BCD`、`\EFI\MICROSOFT\BOOT\BCD` | 宿主 PCAT / EFI BCD，缺失则 `CreateBcdStoreAtAsync` | 对应固件无引导配置 |
| 两处 `WGL4_BOOT.TTF` | `C:\Windows\Boot\Fonts\wgl4_boot.ttf`（可选） | 静默跳过，启动界面可能缺字 |

## 核心流程

1. 前置检查（进度 1）：`BundledPayload.TryGetPePayload` 必须通过，缺任一文件直接抛「内置 PE 资源不完整（缺 …）」。必需项为 `boot.wim`、`boot.sdi`、`Tools\FireWinPE.exe`、`Tools\wimlib-imagex.exe`、`Tools\libwim-15.dll`、`Tools\7za.exe`。
2. 取宿主引导文件（`RequireHostFile`，缺失即抛 `DeployException` 并建议改用「写入 U 盘」）：
   `C:\Windows\Boot\PCAT\bootmgr` → 根 `\bootmgr`；
   `C:\Windows\Boot\DVD\PCAT\etfsboot.com` → `\boot\etfsboot.com`；
   `C:\Windows\Boot\EFI\bootmgfw.efi` → `\efi\boot\bootx64.efi` 与 `\efi\microsoft\boot\bootmgfw.efi`。
   根 `\bootmgr.efi` 优先用 `C:\Windows\Boot\EFI\bootmgr.efi`，缺失才退回 `bootmgfw.efi`（efisys 里的 cdboot 只加载卷根 bootmgr.efi）。
3. `FindEfiBootImage`：先找 `C:\Windows\Boot\DVD\EFI\en-US\efisys_noprompt.bin`，再找同目录 `efisys.bin`，最后在 `C:\Windows\Boot\DVD\EFI` 递归搜索；都找不到抛「缺少 efisys_noprompt.bin / efisys.bin」。字体 `C:\Windows\Boot\Fonts\wgl4_boot.ttf` 可选，缺失则跳过两份字体。
4. 可选镜像（进度 2）：路径先 `Trim()` 再 `Trim('"')`，不存在抛「找不到系统镜像」。以 `.iso` 结尾则 `DeployService.MountIsoAsync` 挂载并取 `InstallWimPath`，同时记录 `mountedIso`；其他扩展名直接当镜像文件。
5. 准备两份 BCD（进度 6）：工作目录 `%TEMP%\firewin-iso-<guid:N>`，`bcd-bios` / `bcd-efi` 分别复制 `C:\Windows\Boot\DVD\PCAT\BCD` 与 `C:\Windows\Boot\DVD\EFI\BCD`（`FindHostBcd` 只认存在且长度大于 0 的文件）。哪份缺失就对那份调用 `UsbBootService.CreateBcdStoreAtAsync(bcdPath, efi: false / true, progress, token)`；两份不能共用。
6. 组装暂存目录树 `stage`：`\bootmgr`、`\bootmgr.efi`、`\boot\boot.sdi`、`\boot\bcd`、`\boot\etfsboot.com`、`\efi\boot\bootx64.efi`、`\efi\microsoft\boot\bootmgfw.efi`、`\efi\microsoft\boot\bcd`、`\sources\boot.wim`；有字体则加 `\boot\fonts\wgl4_boot.ttf` 与 `\efi\microsoft\boot\fonts\wgl4_boot.ttf`；有镜像则加 `\FWIMG\INSTALL<大写扩展名>`（目录名取 `UsbBootService.ImageFolderName`）。每个文件走 `StageFile`：同一卷优先 `CreateHardLink`（`kernel32!CreateHardLinkW`），失败才 `File.Copy`，避免复制约 1 GB 的 boot.wim。
7. 打开输出（进度 10）：`Path.GetFullPath(outputPath)`、确保父目录存在；已存在则先 `File.SetAttributes(..., FileAttributes.Normal)`；以 `FileMode.Create`、`FileAccess.Write`、`FileShare.None`、1 MB 缓冲打开 `FileStream`。
8. `WriteImage(stage, efisys, output)`：创建 `IMAPI2FS.MsftFileSystemImage`，设 `FileSystemsToCreate=7`、`VolumeName="FireWinPE"`、`FreeMediaBlocks=0`，`Root.AddTree(stage, false)`；用 `ADODB.Stream` 载入 efisys，塞进 `IMAPI2FS.BootOptions`（`Emulation=0`、`PlatformId=0xEF`）赋给 `BootImageOptions`；`CreateResultImage()` 拿 `IStream`，`Seek(0,0,0)` 后用 1 MB 缓冲逐段写入输出流并 `Flush`。
9. 进度 92，关闭输出流后调用 `PatchBiosBootEntry(fullOutputPath)`，把引导目录改写成官方介质结构（见下节）。
10. 进度 100，返回 `fullOutputPath`；`finally` 里先 `DeployService.TryDismountIso(mountedIso)`（仅当挂载过），再 `Directory.Delete(workDirectory, true)`，清理异常被吞掉。

## 引导目录补写

IMAPI2FS 只允许一个 El Torito 引导项，本次写入的是 EFI 项；而 Legacy BIOS 固件只读引导目录偏移 32 的默认项。`PatchBiosBootEntry` 以读写方式独占打开已生成的 ISO，按 ISO9660 结构定位并整扇区重写引导目录：

1. 读 LBA 17，校验 `[0]==0x00` 且 `[1..5]=="CD001"`，否则抛「LBA 17 不是 El Torito 引导记录」。
2. 引导目录 LBA 取引导记录偏移 71 的 LE32；PVD 在 LBA 16，根目录 extent 取偏移 158、长度取偏移 166，再经 `FindRecord` 依次找到 `BOOT` 目录与 `ETFSBOOT.COM`（记录名带 `;1`，比较前去掉，忽略大小写）。
3. 读引导目录扇区，要求偏移 32 的字节为 `0x88`（IMAPI2FS 写入的 EFI 默认项），否则抛「偏移 32 不是默认引导项」。
4. 在内存里重建 2048 字节引导目录并整扇区写回：

| 偏移 | 写入值 | 含义 |
|---|---|---|
| 0 | `0x01` | 校验项标识（可启动） |
| 1 | `0x00` | 平台 x86 |
| 4 | ASCII `Windows PE` | 标识串 |
| 28 | `0x10000 - 前 32 字节按 16 位小端字累加` | 校验和，使累加为 0 |
| 30 / 31 | `0x55` / `0xAA` | 有效标识 |
| 32 | `0x88`、`0x00`、加载段 0、系统类型 0、扇区数、LBA | 默认项 = `\BOOT\ETFSBOOT.COM` |
| 64 | `0x91` | 最终分区头 |
| 65 | `0xEF` | 平台 EFI |
| 66 | LE16 `1` | 该分区下引导项数量 |
| 96 | 复制原引导目录偏移 32 起的 32 字节 | 保留 IMAPI2FS 写的 EFI 项（efisys） |

5. 默认项扇区数按 `(etfsboot.Size + 511) / 512` 计算（`BootSectorSize=512`），LBA 取 `ETFSBOOT.COM` 的 extent；`WriteBootEntry` 固定写加载段 0（传统 0x7C0）、系统类型 0。

结果与微软官方安装介质一致：唯一校验项 + 默认项（etfsboot + 最终分区）+ 最终分区下的 EFI 项，因此同一张盘 UEFI 与 Legacy BIOS 都能启动。

## 约束与坑

1. 现象：EFI Shell 里光盘是空目录，cdboot 报 Not Found。原因：盘上没有 UDF，VMware EFI 固件不挂载纯 ISO9660 + Joliet。正确做法：保持 `FileSystemsToCreate = 7`，不要改成 1 或 3。
2. 现象：UEFI 报 0xc000000f。原因：两个位置用了同一份 BCD（PCAT 的加载器是 winload.exe，EFI 的是 winload.efi）。正确做法：`\boot\bcd` 取 PCAT BCD，`\efi\microsoft\boot\bcd` 取 EFI BCD，缺一份才单独生成。
3. 现象：补写抛「偏移 32 不是默认引导项」。原因：`BootImageOptions` 没在 `CreateResultImage` 之前赋值，或引导项结构变化。正确做法：先设 BootOptions 再出镜像；补写失败即视为失败产物。
4. 现象：补写抛「找不到 \BOOT 目录」或「找不到 \BOOT\ETFSBOOT.COM」。原因：没有 `StageFile(stage, @"boot\etfsboot.com", etfsboot)`，或宿主介质不完整。正确做法：确认暂存树含该项；补写只读 ISO9660 主卷目录，不查 Joliet / UDF 树。
5. 现象：ISO 生成完成但 Legacy BIOS 不启动。原因：`PatchBiosBootEntry` 在输出流关闭后才执行，若抛异常，磁盘上已留下只有 EFI 项的盘。正确做法：看到「正在补写 BIOS 引导项」后无异常才算成功；补写失败的 ISO 不要刻盘。
6. 现象：失败后留下半成品 ISO。原因：直接以 `FileMode.Create` 写目标文件，异常路径不删除输出。正确做法：手工删除后重试；不要用未知完整性的 ISO。
7. 现象：源 ISO 一直处于挂载状态。原因：`mountedIso` 在 `MountIsoAsync` 返回后才赋值，而该方法内部挂载成功后 `FindInstallWim` 可能抛「未找到 sources\install.wim」。正确做法：失败后手工 `Dismount-DiskImage -ImagePath <iso>`，或在源码异常路径补挂载跟踪。
8. 现象：大 `install.wim` 生成报 COM / IO 错误，提示不友好。原因：新实现已删除旧版 `uint` 单文件 4 GB 校验，源码中不再有显式大小检查（界面文案仍写「ISO 内单个文件上限 4 GB」）。正确做法：优先用 `install.esd`，或改用写 U 盘把镜像放 `\FWIMG`（exFAT 无此限制）。
9. 现象：输出文件无法创建。原因：`FileShare.None` 打开已被挂载或占用的同名 ISO。正确做法：先卸载/关闭占用进程；已存在文件只会被重置为 Normal 属性。
10. 现象：`%TEMP%\firewin-iso-*` 残留。原因：`finally` 中删除失败被忽略（例如文件仍被 COM 持有）。正确做法：生成结束后手工清理，不影响已产出 ISO。
11. 现象：无管理员权限时被界面拦下。原因：只有宿主 BCD 缺失、需要 `bcdedit` 生成时才要求提权（`!IsAdministrator() && !HostBcdAvailable`）；服务本身不做权限判断。正确做法：以管理员运行，或补齐 `C:\Windows\Boot\DVD` 下的 PCAT / EFI BCD。
12. 现象：生成中途取消留下不完整 ISO。原因：只在挂载后、暂存后、写盘前检查 `cancellationToken`，`CreateResultImage` 与流复制期间无法中断；桌面端传入 `CancellationToken.None`。正确做法：取消后删除输出文件再重试。
13. 现象：PE 端扫不到附带镜像。原因：目录名或文件名被改。正确做法：固定用 `UsbBootService.ImageFolderName`（`FWIMG`）+ `INSTALL` + 大写扩展名，与 U 盘模式保持一致。

## 验证清单

- [ ] 日志出现「正在生成 UDF + ISO9660 + Joliet 镜像」与「正在补写 BIOS 引导项」，最后返回 ISO 全路径。
- [ ] 资源管理器或 7-Zip 可打开 ISO，卷标 `FireWinPE`，文件系统含 UDF。
- [ ] 根目录有 `BOOTMGR`、`BOOTMGR.EFI`，且 `\EFI\MICROSOFT\BOOT\BOOTMGFW.EFI` 存在。
- [ ] `\BOOT\BCD` 与 `\EFI\MICROSOFT\BOOT\BCD` 字节内容不同（分别来自 PCAT / EFI）。
- [ ] `\BOOT\ETFSBOOT.COM` 与 `\SOURCES\BOOT.WIM` 存在，`BOOT.WIM` 大小与 `payload\pe\boot.wim` 一致。
- [ ] VMware（EFI）从 ISO 启动进入 PE，EFI Shell 下能看到磁盘内容（旧纯 ISO9660 盘的关键回归点）。
- [ ] Legacy BIOS 虚拟机同样从 ISO 进入 PE。
- [ ] 附带镜像时 `\FWIMG\INSTALL.WIM` 或 `.ESD` 存在，PE 端「安装系统」可扫描到。
- [ ] 生成结束后 `%TEMP%\firewin-iso-<guid>` 已删除，源 ISO 已卸载。
- [ ] 缺宿主文件时报错包含具体路径并建议改用「写入 U 盘」。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| EFI Shell 空目录 / `\bootmgr.efi` Not Found | ISO 是否含 UDF，`FileSystemsToCreate` 是否为 7 | 保持 7，禁止退回纯 ISO9660；同时确认根有 `BOOTMGR.EFI` |
| UEFI 0xc000000f | 两份 BCD 是否同一份 | 分别取 PCAT / EFI BCD，缺失才调用 `CreateBcdStoreAtAsync` |
| 补写报「偏移 32 不是默认引导项」 | `BootImageOptions` 是否在出镜像前赋值 | 调整写入顺序，不要跳过补写 |
| 补写报找不到 `\BOOT\ETFSBOOT.COM` | 暂存树与宿主 `C:\Windows\Boot\DVD\PCAT` | 补齐 etfsboot.com 后重新生成 |
| 报缺 `efisys_noprompt.bin` / `efisys.bin` | `C:\Windows\Boot\DVD\EFI` 是否完整 | 换完整 Windows 10/11 机器生成，或改用写 U 盘 |
| 报无法创建 IMAPI2FS 组件 | `IMAPI2FS.MsftFileSystemImage` 是否可创建 | 修复系统组件（Win10/11 自带），不要退回已删除的自研写入器 |
| ISO 生成成功但 BIOS 不进引导 | 是否执行到补写且无异常 | 删除该 ISO 重新生成，必要时核对 LBA 17 与引导目录偏移 32 |
| 大镜像生成失败 | 附加镜像体积与格式 | 换 `install.esd`，或改用 U 盘（exFAT） |
| 输出被占用 / 拒绝访问 | 同名 ISO 是否已挂载 | 卸载后重试，必要时重置文件属性 |
| 源 ISO 仍挂载 | `Get-DiskImage` 中的镜像列表 | `Dismount-DiskImage -ImagePath <iso>` |
| 临时目录残留 | `%TEMP%\firewin-iso-*` | 手工删除，不影响已生成的 ISO |

## 相关 skill

- firewin-usb-boot：另一交付形态，U 盘不受 ISO 单文件体积与文件系统限制；ISO 复用其 `CreateBcdStoreAtAsync` 与 `FWIMG` 目录名约定。
- firewin-pe-build：提供 `boot.wim` / `boot.sdi` 与 ADK 路线，说明 `MakeWinPEMedia` 与本流程的产物差异。
- firewin-overview：仓库总览与各 skill 分工，先读它再定位具体流程（其中 ISO 相关描述仍是旧写入器措辞）。
- firewin-build-release：编译桌面端、组装 payload 并产出 `dist` 发布物的流程。
- firewin-troubleshooting：按启动症状（0xc000000f、Not Found、14001）做跨模块排查的总入口。
- firewin-cli-automation：命令行/无界面方式触发制作与验证时使用。
