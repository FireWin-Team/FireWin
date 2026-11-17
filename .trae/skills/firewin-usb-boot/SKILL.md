---
name: firewin-usb-boot
description: Build FireWin PE USB boot disks via diskpart dual partitions, dual BCD stores, bootsect BIOS code and in place upgrade. Use for USB write or upgrade. Not for ISO or PE image build.
---

# U 盘 PE 启动盘制作与原地升级

把内置 PE（boot.wim + boot.sdi）写入可移动磁盘，做出 UEFI 与 Legacy BIOS 双引导、数据分区可当普通 U 盘使用的启动盘。

## 适用范围

- 枚举可移动磁盘并按物理磁盘号去重，向用户展示候选 U 盘（`UsbBootService.ListUsbDisks`）。
- 全新制作启动盘：清盘、MBR 双分区、写 PE 文件与两套 BCD、写 BIOS 引导代码、隐藏启动分区。
- 判断某块 U 盘是否已是 FireWin 启动盘（`CanUpgradeInPlace`），并执行原地升级（`UpgradeBootDiskAsync`）。
- 排查启动失败：0xc000000f、只有 UEFI 能启动、启动分区盘符被占用、升级时报剩余空间不足。
- 调整启动分区容量估算策略（`EstimateBootPartitionMb` 的 1536 MB 下限与 512 MB 对齐）。

## 不适用范围

- 生成 ISO 镜像：由 `IsoImageService` 负责，它不分区、不写 bootsect，见 firewin-iso-builder。
- 重新构建 boot.wim 本体或注入 WinPE 可选组件、运维驱动：见 firewin-pe-build 与 `scripts\build-minipe.ps1`。
- 桌面端「一键安装系统」的一次性引导（`bcdedit /bootsequence` 指向 `C:\FireWinPE`）：由 `BcdBootService` 负责。

## 关键文件与符号

| 路径 | 作用 | 关键类型/方法 |
|---|---|---|
| `src\FireWin\Services\UsbBootService.cs` | U 盘枚举与制作/升级全部逻辑 | `ListUsbDisks`、`CreateBootDiskAsync`、`UpgradeBootDiskAsync`、`CanUpgradeInPlace` |
| 同上 | 分区与卷挂载 | `RunDiskpartScriptAsync`、`WaitForVolumeAsync`、`FindVolumeNumberAsync`、`HideBootPartitionAsync` |
| 同上 | PE 文件与引导配置 | `WritePeFilesAsync`、`CreateBcdStoreAsync`、`CreateBcdStoreAtAsync`、`TryWriteBiosBootCodeAsync` |
| 同上 | 盘符与物理盘映射 | `TryGetDiskNumber`（IOCTL）、`GetDiskTotalBytes`、`PickFreeDriveLetter` |
| `src\FireWin\Services\DeployService.cs` | 大文件带进度复制、ISO 挂载、字节格式化 | `CopyFileWithProgressAsync`、`MountIsoAsync`、`FormatBytes` |
| `src\FireWin\Services\BundledPayload.cs` | 内置 PE 资源定位 | `PeDirectory`、`TryGetPePayload` |
| `payload\pe\` | 随包 PE（boot.wim / boot.sdi / Tools） | 由 `build-minipe.ps1 -AssemblePayload` 产出 |
| `README.md` §5.2 | 面向用户的操作序列与产物说明 | 制作流程、双引导、FWIMG 说明 |

## 核心流程

1. 枚举磁盘：`DriveInfo.GetDrives()` 过滤 `IsReady && DriveType.Removable`，取盘符前两位拼 `\\.\X:`，用 `CreateFileW` 打开卷句柄后发 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`（0x00560000）拿 `DiskNumber`；再打开 `\\.\PhysicalDriveN` 发 `IOCTL_DISK_GET_DRIVE_GEOMETRY`（0x00070000），按 `Cylinders × TracksPerCylinder × SectorsPerTrack × BytesPerSector` 算总容量（失败回退分区容量）。以 DiskNumber 为字典键去重，只保留第一条。
2. 估算启动分区：`EstimateBootPartitionMb` = 内置 boot.wim + boot.sdi 字节 + 512 MB 余量，向上取整到 MB，再向上对齐 512 MB，最后 `Math.Max(1536, …)`。
3. 判定分区方案：`dualPartition = DiskTotalBytes > (bootPartitionMb + 4096) * 1024 * 1024`。够大就双分区（启动分区 + 数据分区），否则单分区独占整盘且不隐藏。
4. 选盘符：`PickFreeDriveLetter` 从 D 到 Z 找未被 `DriveInfo` 占用的盘符，数据分区再排除刚选的启动分区盘符。
5. 执行 diskpart 脚本：脚本写入 `%TEMP%\firewin-usb-<guid>.txt`（UTF-8 无 BOM，行尾 CRLF），再 `diskpart.exe /s <文件>`，退出码非 0 抛 `DeployException`，最后删除临时脚本。脚本序列见「布局或参数表」。
6. 等卷挂载：`WaitForVolumeAsync(label)` 轮询 40 次、每次 500 ms，按卷标找到盘符；找不到即抛异常（FireWinPE 或 FireData）。
7. 写 PE 文件（`WritePeFilesAsync`，进度 18→88）：先 `\sources\boot.wim`（18%→60%，带进度），再 `\boot\boot.sdi`，然后从宿主 `C:\Windows\Boot\PCAT\bootmgr` 复制 bootmgr、从 `C:\Windows\Boot\EFI\bootmgfw.efi` 复制成 `\EFI\Boot\bootx64.efi` 与 `\EFI\Microsoft\Boot\bootmgfw.efi`，字体 `C:\Windows\Boot\Fonts\wgl4_boot.ttf` 复制到 `\boot\fonts\` 与 `\EFI\Microsoft\Boot\fonts\`；再创建 `FWIMG` 目录并写入说明文本；若指定了镜像，ISO 先挂载取 `InstallWimPath`，复制为 `FWIMG\install.<ext>`（66%→88%）。
8. 生成两份 BCD（`CreateBcdStoreAsync`，89% / 92%）：`\boot\bcd` 用 `efi: false`，`\EFI\Microsoft\Boot\BCD` 用 `efi: true`。
9. 写 BIOS 引导代码（`TryWriteBiosBootCodeAsync`）：`bootsect.exe /nt60 X: /force /mbr`；找不到 bootsect 只打印警告，UEFI 不受影响。
10. 双分区时隐藏启动分区：`diskpart` 执行 `select volume X` + `remove letter=X`，仅移除挂载点，不改分区类型，进度 96%→100%。

原地升级（`UpgradeBootDiskAsync`）：

1. 前置：`payload\pe\boot.wim` 与 `boot.sdi` 必须存在，否则直接拒绝。
2. 定位分区：`FindOwnVolumeLetter("FireData"/"FireWinPE", diskNumber)` 按卷标 + 物理盘号找可见盘符。启动分区通常无盘符：`diskpart list volume` 找到含 `FireWinPE` 的行，正则取第一个数字当卷号，`select volume N` + `assign letter=<PickFreeDriveLetter>`，再 `WaitForVolumeAsync` 复核盘符确实落在目标物理盘上；不符则 `remove letter` 并抛异常。
3. 校验：`DriveInfo.DriveFormat` 必须是 FAT32；`requiredBytes = 新 wim 长度 - 启动分区内旧 wim 长度 + 64 MB`，`AvailableFreeSpace` 不足即报"请改用全新制作"（FAT32 分区不能像重新分区那样扩容）。
4. 覆盖 PE 文件 → 重建两份 BCD → 重写 bootsect；数据分区与 `FWIMG` 内容不动。
5. `finally` 仅在本次临时分配过盘符时调用 `HideBootPartitionAsync` 恢复隐藏，失败只告警不影响启动。

BCD 生成（`CreateBcdStoreAtAsync`，U 盘与 ISO 共用）：

```text
bcdedit /createstore <bcd>.new
bcdedit /store <bcd>.new /create {bootmgr} /d "FireWin Boot Manager"
bcdedit /store <bcd>.new /set {bootmgr} device boot
bcdedit /store <bcd>.new /set {bootmgr} timeout 0
bcdedit /store <bcd>.new /create {ramdiskoptions} /d "FireWin RAMDisk Options"
bcdedit /store <bcd>.new /set {ramdiskoptions} ramdisksdidevice boot
bcdedit /store <bcd>.new /set {ramdiskoptions} ramdisksdipath \boot\boot.sdi
bcdedit /store <bcd>.new /create /d "FireWin PE" /application osloader   （输出里正则抓 {GUID}）
bcdedit /store <bcd>.new /set {GUID} device ramdisk=[boot]\sources\boot.wim,{ramdiskoptions}
bcdedit /store <bcd>.new /set {GUID} osdevice ramdisk=[boot]\sources\boot.wim,{ramdiskoptions}
bcdedit /store <bcd>.new /set {GUID} path \Windows\System32\boot\winload.efi   （efi=true）
bcdedit /store <bcd>.new /set {GUID} systemroot \Windows
bcdedit /store <bcd>.new /set {GUID} detecthal yes
bcdedit /store <bcd>.new /set {GUID} winpe yes
bcdedit /store <bcd>.new /set {bootmgr} default {GUID}
bcdedit /store <bcd>.new /set {bootmgr} displayorder {GUID}
# 全部成功后才 File.Move(<bcd>.new, <bcd>, overwrite: true)
```

## 布局或参数表

关键常量（`UsbBootService` 私有/公开字段）：

| 名称 | 值 | 用途 |
|---|---|---|
| `BootPartitionLabel` | `FireWinPE` | 启动分区卷标，也是定位与判定归属的依据 |
| `DataPartitionLabel` | `FireData` | 数据分区卷标 |
| `ImageFolderName` | `FWIMG` | 公开常量，PE 端优先扫描的镜像目录 |
| `MinBootPartitionMb` | 1536 | 启动分区下限 |
| `DataPartitionReserveMb` | 4096 | 判定双分区时为数据分区预留的 MB |

diskpart 脚本（`RunDiskpartScriptAsync` 生成，双分区形态）：

```text
select disk <N>
clean
create partition primary size=<bootPartitionMb>
format fs=fat32 quick label=FireWinPE
active
assign letter=<bootLetter>
create partition primary
format fs=exfat quick label=FireData
assign letter=<dataLetter>
```

单分区形态去掉 `size=` 与最后三行（`create partition primary` 独占整盘，卷标同为 FireWinPE，不隐藏）。

启动分区文件落位：

| 目标路径 | 来源 | 说明 |
|---|---|---|
| `\sources\boot.wim` | `BundledPayload.PeDirectory\boot.wim` | 最大文件，进度 18%→60% |
| `\boot\boot.sdi` | 同目录 `boot.sdi` | ramdisk 必需 |
| `\bootmgr` | `C:\Windows\Boot\PCAT\bootmgr` | BIOS 引导管理器 |
| `\EFI\Boot\bootx64.efi` | `C:\Windows\Boot\EFI\bootmgfw.efi` | UEFI 默认回退路径 |
| `\EFI\Microsoft\Boot\bootmgfw.efi` | 同上 | bootmgfw 固定查找路径 |
| `\boot\fonts\wgl4_boot.ttf` | `C:\Windows\Boot\Fonts\wgl4_boot.ttf` | 缺则文字显示方块 |
| `\EFI\Microsoft\Boot\fonts\wgl4_boot.ttf` | 同上 | UEFI 界面字体 |
| `\boot\bcd` | `bcdedit` 生成（efi=false） | 加载器 winload.exe |
| `\EFI\Microsoft\Boot\BCD` | `bcdedit` 生成（efi=true） | 加载器 winload.efi |
| `FWIMG\`（数据分区，单分区时在启动分区根） | 程序创建 | 附「把系统镜像放到这里.txt」 |
| `FWIMG\install.<ext>` | 可选镜像（ISO 先挂载） | 扩展名转小写 |

## 约束与坑

1. 升级后 BCD 文件被写坏、U 盘无法启动 → 直接 `bcdedit /createstore <目标 BCD>` 在已存在的文件上执行会失败或留下残缺 store → 必须先生成 `<bcd>.new`，全部命令成功后再 `File.Move(..., overwrite: true)`；若 `.new` 已存在先 `File.SetAttributes(Normal)` 再删除。
2. UEFI 启动报 0xc000000f → 两份 BCD 只有一份，或两份共用 PCAT 的 store（其加载器是 winload.exe） → `\boot\bcd` 用 `efi: false`、`\EFI\Microsoft\Boot\BCD` 用 `efi: true` 分开生成，两份都要写 `\EFI\Microsoft\Boot\bootmgfw.efi`。
3. `bcdedit` 报参数错误 → 把 `/store` 与路径拼成一个字符串（进程参数被整体加引号） → 通过 `ArgumentList` 逐项追加，`/store` 与路径必须是两个独立参数。
4. 升级提示剩余空间不足 → FAT32 启动分区大小固定，无法原地扩容 → 校验式 `新 wim - 旧 wim + 64 MB`，不满足就引导用户重新制作（重新制作会按新体积重算分区）。
5. 分区完成却找不到盘符 → diskpart 返回后卷挂载有延迟 → `WaitForVolumeAsync` 40 次 × 500 ms；升级隐藏分区时先用 `list volume` 取卷号再 `assign letter`，用完在 `finally` 里 `remove letter`。
6. 两个同卷标 U 盘同时插入时制作到了错误的盘 → `FindDriveByLabel` / `WaitForVolumeAsync` 只比卷标，不校验物理盘号；升级流程会复核 `TryGetDiskNumber`，全新制作流程不会 → 制作前只保留一块目标 U 盘，或用 `DiskNumber` 再核对一次。
7. 只有 UEFI 能启动，Legacy BIOS 不进 → 缺少 bootsect.exe，代码仅打印警告并继续 → 把 bootsect.exe 放到程序目录或 `tools\` 子目录，或安装 ADK 后重做（搜索顺序：程序目录 → 程序目录\tools → `KitsRoot10` 下 `Assessment and Deployment Kit\Deployment Tools` 递归）。
8. 启动界面全是方块、进度条异常 → 宿主缺 `C:\Windows\Boot\Fonts\wgl4_boot.ttf`（代码静默跳过） → 用 Windows 10/11 制作，或在完整系统上补齐该文件后重做。
9. 数据分区放不进 4 GB 以上的 install.wim → 单分区布局（磁盘太小）时 FWIMG 落在 FAT32 启动分区 → 换 ≥ 8 GB 的 U 盘走双分区（数据分区 exFAT），或改用 ISO。
10. 制作后资源管理器里看不到启动分区 → 这是设计行为，`HideBootPartitionAsync` 只执行 `remove letter`，未改分区类型，UEFI 固件仍能识别 → 需要维护时用 `diskpart` 的 `list volume` + `assign letter` 临时挂载。
11. `CanUpgradeInPlace` 返回 false 但仍想升级 → 它依赖卷标为 FireWinPE/FireData 的可见分区，或根目录存在 `sources\boot.wim`；手工改过卷标的盘会被判为不可升级 → 恢复卷标后重试，否则只能全新制作。
12. IOCTL 只取第一个 extent → `VolumeDiskExtents` 缓冲区按 `Marshal.SizeOf * 8` 传，但只读 `Extent`（跨盘区的卷会得到不完整映射） → U 盘不会跨盘，正常；若返回 false 则该项磁盘被跳过而不是猜盘符。

## 验证清单

- [ ] `UsbBootService.ListUsbDisks()` 对一块双分区 U 盘只返回一条记录，`SummaryText` 中的物理磁盘号与磁盘管理中一致。
- [ ] 制作完成后 `\sources\boot.wim`、`\boot\boot.sdi`、`\bootmgr`、`\EFI\Boot\bootx64.efi`、`\EFI\Microsoft\Boot\bootmgfw.efi`、两份 BCD 全部存在。
- [ ] 启动分区在「此电脑」中不可见，数据分区可见且卷标为 FireData，根下有 `FWIMG` 与说明文本。
- [ ] UEFI 机器从 U 盘启动进入 PE；Legacy BIOS 机器同样能进入（需 bootsect）。
- [ ] 对已制作 U 盘再次执行升级：不重新分区、数据分区文件不变、启动分区被重新隐藏。
- [ ] 把启动分区填满或换成 NTFS 后升级，得到明确的"格式不对 / 空间不足，请改用全新制作"提示，而不是损坏的引导。
- [ ] 升级过程中 `list volume` 临时分配的盘符在结束后被移除（`fsutil volume` 或磁盘管理确认无盘符）。
- [ ] 未安装 ADK 的机器上制作，日志出现 bootsect 警告但 UEFI 启动正常。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| UEFI 报 0xc000000f | `\EFI\Microsoft\Boot\BCD` 与 `bootmgfw.efi` 是否存在 | 重新生成 efi=true 的 BCD；确认 bootmgfw 两份都在 |
| 卡在"未找到启动分区" | diskpart 输出、卷标是否为 FireWinPE | 手动 `diskpart list volume` 核对；重新制作 |
| 升级提示找不到 FireWinPE 分区 | 启动分区是否被手工改过卷标 | 恢复卷标或用 `list volume` 手工 assign 后重试 |
| 进度停在 18%–60% 长时间不动 | boot.wim 体积与 U 盘写入速度 | 等带进度复制结束；换 USB 3.0 口 |
| bootsect 失败（非 0 退出码） | 是否被安全软件拦截、盘符是否正确 | 关掉拦截后重试；UEFI 场景可忽略 |
| 数据分区不可见 | 是否单分区布局 | 单分区不会创建 FireData，属预期 |
| 制作后盘符错乱 | 同卷标多盘、残留挂载点 | 只插一块目标盘；`diskpart` 清理多余盘符 |

## 相关 skill

- firewin-iso-builder：另一种交付形态，复用本 skill 的 BCD 生成与宿主引导文件；ISO 不做分区与隐藏。
- firewin-pe-build：产出 `payload\pe\`（boot.wim / boot.sdi / Tools），是本 skill 的输入；boot.wim 体积直接决定启动分区估算值。
- firewin-overview：仓库总览与各 skill 的分工关系，先读它再定位到具体流程。
