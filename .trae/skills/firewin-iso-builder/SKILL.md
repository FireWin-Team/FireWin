---
name: firewin-iso-builder
description: Generate bootable FireWin PE ISO images with the built in ISO9660 Joliet El Torito writer. Use for ISO output without ADK. Not for USB writing or WIM builds.
---

# 自研 PE ISO 生成（ISO9660 + Joliet + El Torito）

不依赖 ADK / oscdimg，直接用内置写入器把 boot.wim、宿主引导文件与两份 BCD 组装成 UEFI 与 Legacy BIOS 双引导的 ISO。

## 适用范围

- 生成可启动 PE ISO（`IsoImageService.CreatePeIsoAsync`），可附加一个系统镜像到 `\FWIMG`。
- 排查 ISO 启动失败：0xc000000f、UEFI 下 Not Found、Legacy BIOS 不进引导、卷标异常。
- 修改 ISO 目录布局、El Torito 引导项、Joliet 长名策略或 BCD 来源逻辑。
- 判断当前主机能否免 ADK 生成 ISO（`HostBcdAvailable`），以及缺哪份宿主文件。

## 不适用范围

- 写入 U 盘、分区、隐藏启动分区、bootsect：见 firewin-usb-boot。
- 构建或修改 boot.wim 本体（copype / DISM / 组件注入）：见 firewin-pe-build。
- 生成安装介质用的完整 Windows ISO（含 install.wim 全量 install 目录）：本写入器只产 PE 盘。

## 关键文件与符号

| 路径 | 作用 | 关键类型/方法 |
|---|---|---|
| `src\FireWin\Services\IsoImageService.cs` | ISO 生成全部逻辑 | `CreatePeIsoAsync`、`HostBcdAvailable`、`FindEfiBootImage`、`RequireHostFile` |
| 同上 | 卷描述符与引导目录 | `BuildPrimaryVolumeDescriptor`、`BuildJolietVolumeDescriptor`、`BuildBootRecord`、`BuildBootCatalog`、`WriteBootEntry` |
| 同上 | 目录树与扇区分配 | `IsoDirectory`、`IsoFile`、`EnumerateEntries`、`MeasureDirectory`、`BuildPathTable`、`ToIsoIdentifier` |
| 同上 | 写入与校验 | `CopyFileIntoIso`、`WritePadded`、`SectorsFor`、`stream.Length` 校验 |
| `src\FireWin\Services\UsbBootService.cs` | 复用 BCD 生成器 | `CreateBcdStoreAtAsync`、`ImageFolderName` |
| `src\FireWin\Services\DeployService.cs` | ISO 挂载与格式化 | `MountIsoAsync`、`TryDismountIso`、`FormatBytes` |
| `src\FireWin\Services\BundledPayload.cs` | 内置 PE 资源 | `TryGetPePayload`、`PeDirectory` |
| `README.md` §5.2 ② | 用户视角的目录与要点 | 布局说明、双引导说明 |

## 核心流程

1. 前置检查：`BundledPayload.TryGetPePayload` 必须通过（缺 `boot.wim` / `boot.sdi` 直接报缺项）；`percentProgress` 从 1 起报。
2. 取宿主引导文件（缺任一即抛 `DeployException` 并建议改用写 U 盘）：
   `C:\Windows\Boot\PCAT\bootmgr`、`C:\Windows\Boot\DVD\PCAT\etfsboot.com`、`C:\Windows\Boot\EFI\bootmgfw.efi`；`FindEfiBootImage` 先试 `C:\Windows\Boot\DVD\EFI\en-US\efisys_noprompt.bin`，再试 `efisys.bin`，最后在 `C:\Windows\Boot\DVD\EFI` 下递归找。`C:\Windows\Boot\Fonts\wgl4_boot.ttf` 可选，缺了就跳过字体。
3. 处理可选镜像（进度 2）：路径先 `Trim` 再 `Trim('"')`；`.iso` 结尾则 `DeployService.MountIsoAsync` 挂载并取 `InstallWimPath`，记住 `mountedIso` 以便 `finally` 里 `TryDismountIso`；其他扩展名直接当镜像文件。
4. 准备 BCD（进度 6）：工作目录 `%TEMP%\firewin-iso-<guid>`，分别复制 `C:\Windows\Boot\DVD\PCAT\BCD` → `bcd-bios`、`C:\Windows\Boot\DVD\EFI\BCD` → `bcd-efi`；哪份缺失就只对那份调用 `UsbBootService.CreateBcdStoreAtAsync`（`efi: false` / `efi: true`）。
5. 组装目录树：根下 `BOOTMGR`、`BOOTMGR.EFI`；`BOOT` 下 `BOOT.SDI`、`BCD`、`FONTS\WGL4_BOOT.TTF`；`SOURCES\BOOT.WIM`；`EFI\BOOT\BOOTX64.EFI`；`EFI\MICROSOFT\BOOT\BOOTMGFW.EFI`、`BCD`、`FONTS\WGL4_BOOT.TTF`；有镜像则 `FWIMG\INSTALL<大写扩展名>`。
6. 编号与度量：以根为 1 广度优先遍历，同级子目录按 `IsoName` 用 `StringComparer.Ordinal` 排序后再入队；每个目录分别计算主卷长度与 Joliet 长度（均按 2048 对齐，至少 1 个扇区）。
7. 分配 LBA：`BootCatalogLba = 20` 起，依次为主卷 L 路径表、主卷 M 路径表、Joliet L、Joliet M、全部主卷目录、全部 Joliet 目录、etfsboot.com、efisys、各文件；`totalSectors` 即最终 LBA。
8. 组装扇区：PVD、Joliet SVD、引导记录、终止符、引导目录（`BuildBootCatalog`），随后 `FileStream` 顺序写出 16 个零扇区、5 个描述符、4 份路径表、两轮目录、两个引导镜像、各文件数据（每段按 2048 补齐）。
9. 进度与校验：文件阶段按已写字节映射 8%→99%；`stream.Flush()` 后断言 `stream.Length == totalSectors * 2048`，不等抛"ISO 长度校验失败：期望 X 字节，实际 Y 字节"。
10. 收尾：返回 `Path.GetFullPath(outputPath)`，`finally` 卸载挂载的 ISO 并递归删除临时目录（删除失败忽略）。

## 布局或参数表

ISO 目录布局（主卷全名 / Joliet 长名一致，主卷文件名追加 `;1`）：

| ISO 路径 | 来源 | 缺失后果 |
|---|---|---|
| `\BOOTMGR` | `C:\Windows\Boot\PCAT\bootmgr` | Legacy BIOS 无法引导 |
| `\BOOTMGR.EFI` | `C:\Windows\Boot\EFI\bootmgfw.efi` | UEFI 立即 Not Found |
| `\BOOT\BOOT.SDI` | 内置 `boot.sdi` | ramdisk 起不来 |
| `\BOOT\BCD` | 宿主 PCAT BCD 或 efi=false 生成 | BIOS 无配置 |
| `\BOOT\FONTS\WGL4_BOOT.TTF` | 宿主字体（可选） | 界面方块 |
| `\SOURCES\BOOT.WIM` | 内置 `boot.wim` | 无 PE 主体 |
| `\EFI\BOOT\BOOTX64.EFI` | 宿主 `bootmgfw.efi` | 固件回退路径找不到 |
| `\EFI\MICROSOFT\BOOT\BOOTMGFW.EFI` | 同上 | UEFI 加载失败 |
| `\EFI\MICROSOFT\BOOT\BCD` | 宿主 EFI BCD 或 efi=true 生成 | 0xc000000f |
| `\EFI\MICROSOFT\BOOT\FONTS\WGL4_BOOT.TTF` | 同上 | 界面方块 |
| `\FWIMG\INSTALL.WIM` / `.ESD` / `.ISO` | 可选镜像 | 无附加镜像 |

扇区布局：

| LBA | 内容 |
|---|---|
| 0–15 | 保留，全零（16 个扇区） |
| 16 | PVD（类型 1，`CD001`，卷标识 `FireWinPE`，应用标识 `FireWin`） |
| 17 | Joliet SVD（类型 2，偏移 88 写转义序列 `%/E`） |
| 18 | 引导记录（`EL TORITO SPECIFICATION`，偏移 71 写 `BootCatalogLba`=20） |
| 19 | 终止符（类型 255） |
| 20 | El Torito 引导目录 |
| 21 起 | 主卷 L / M 路径表 → Joliet L / M 路径表 |
| 其后 | 主卷目录 extent（按编号）→ Joliet 目录 extent |
| 其后 | etfsboot.com → efisys |
| 其后 | 文件数据（按目录收集顺序） |

引导目录字段（`BuildBootCatalog`）：

| 偏移 | 值 | 含义 |
|---|---|---|
| 0 / 1 | `0x01` / `0x00` | 校验项标记 / 平台 x86 |
| 28 | `0x10000 - 前 32 字节 16 位字累加` | 校验和 |
| 30 / 31 | `0x55` / `0xAA` | 有效标识 |
| 32 | `0x88`、`0x00`、扇区数、LBA | 默认项（BIOS，etfsboot.com） |
| 64 / 65 / 66 | `0x91` / `0xEF` / `1` | 最终分区头 / 平台 EFI / 引导项数 |
| 96 | `0x88`、`0x00`、扇区数、LBA | EFI 引导项（efisys） |

扇区数均按 `(字节数 + 511) / 512` 计算；`WriteBootEntry` 的加载段写 0（不模拟，传统 0x7C0）。

卷描述符关键偏移（`BuildPrimaryVolumeDescriptor` / `BuildJolietVolumeDescriptor`）：

| 偏移 | 字段 | 写入值 |
|---|---|---|
| 0 | 描述符类型 | 主卷 `1`，Joliet `2` |
| 1 | 标准标识符 | `CD001` |
| 6 | 版本 | `1` |
| 40 | 卷标识 | 32 字节空格补齐；Joliet 为 UCS-2 大端 `FireWinPE` |
| 80 | 卷空间大小 | 总扇区数（LE + BE 双写） |
| 120 / 124 | 卷集大小 / 卷序号 | `1` / `1` |
| 128 | 逻辑块大小 | 2048 |
| 132 | 路径表字节数 | 实际长度 |
| 140 / 148 | L / M 路径表位置 | LE32 / BE32 |
| 88 | Joliet 转义序列 | `%/E`（Level 3） |
| 156 | 根目录记录 | 目录记录结构，名字为单字节 0 |
| 574 | 应用标识 | `FireWin` |
| 813 / 830 | 创建 / 修改时间 | `yyyyMMddHHmmss00` |
| 847 / 864 | 失效 / 生效时间 | 17 个字符 `0` |

目录记录字段（`WriteDirectoryRecord`）：长度、扩展属性长度 0、extent（+2，双端）、数据长度（+10，双端）、记录时间 7 字节（+18，GMT 偏移 0）、文件标志（+25，目录 `0x02`）、卷序号 1（+28）、名字长度（+32）、名字（+33）。

## 约束与坑

1. UEFI 报 0xc000000f → 两份 BCD 复用了同一份（PCAT 的 store 加载器是 winload.exe，与 UEFI 固件不匹配） → `\BOOT\BCD` 取 PCAT BCD，`\EFI\MICROSOFT\BOOT\BCD` 取 EFI BCD，缺一份才生成一份。
2. UEFI 读不到 `\EFI\Microsoft\Boot\BCD` → 主卷按 8.3 截断把 `MICROSOFT` 写成 `MICROSOF` → `ToIsoIdentifier` 保留全名（大写、非法字符替 `_`、上限 30 字符），同时写 Joliet UCS-2 长名卷；不要退回 8.3。
3. ISO 目录乱序或目录长度与记录不符 → 路径表要求按"层次 → 父目录编号 → 名字"排列，且量算与写出必须用同一套排序 → 编号阶段 BFS 且同级按 `IsoName` 排序，`MeasureDirectory` 与 `BuildDirectory` 共用 `EnumerateEntries`。
4. 目录记录跨扇区导致数据错位 → 单条记录不能横跨 2048 边界 → 写记录前若 `offset % 2048 + 记录长度 > 2048` 就跳到下一扇区；记录长度 = `33 + 名字长度 + (名字长度为偶数 ? 1 : 0)`。
5. 镜像超过 4 GB 直接失败 → `IsoFile.Size` 是 `uint`，`AddFile` 对 `Length > uint.MaxValue` 抛"ISO 内单个文件不能超过 4 GB" → 改用写入 U 盘（exFAT 数据分区），或拆分 install.wim 为 SWM，或去掉附加镜像。
6. UEFI 启动立刻 Not Found → 卷根缺少 `BOOTMGR.EFI`（efisys 内的 cdboot 只加载卷根 `\bootmgr.efi`） → 必须同时写 `\BOOTMGR` 与 `\BOOTMGR.EFI`，二者都源自宿主 bootmgfw/bootmgr。
7. 生成结束抛"ISO 长度校验失败" → 实际写入字节与 `totalSectors * 2048` 不一致（漏写补齐或 LBA 计算偏差） → 视为实现缺陷，不要交付该 ISO；排查 `SectorsFor`、`WritePadded`、文件 extent 分配顺序。
8. 主卷文件名缺少 `;1` 或 Joliet 带版本号 → ISO9660 要求文件标识带版本号，Joliet 明确禁止 → 主卷 `file.IsoName + ";1"`，Joliet 只用 `file.Name` 的 UCS-2 字节。
9. 在无 ADK 机器上生成需要管理员 → `HostBcdAvailable` 为 false 说明宿主缺 PCAT/EFI 安装介质 BCD，会走 `bcdedit` 生成 → 以管理员身份运行，或从完整 Windows 10/11 机器上补齐 `C:\Windows\Boot\DVD`。
10. 生成中途取消留下半成品 ISO → 文件是边算边写，取消时 `FileStream` 已落盘部分内容 → 取消后手工删除输出文件；不要用该文件刻盘。
11. 输出文件被占用/只读 → 代码只对已存在文件设置 `FileAttributes.Normal`，若仍被挂载或占用会抛 IO → 先卸载已挂载的同名 ISO 再重试。
12. 与 `MakeWinPEMedia` 路线结果不同 → ADK 的 oscdimg 用 UDF 承载长名，本写入器用 ISO9660 全名 + Joliet 达到等效效果 → 两者都能双启动；ADK 路线可承载 4 GB 以上单文件，本项目 ISO 路线不行。

## 验证清单

- [ ] 生成过程中无异常，输出 ISO 能被 Windows 资源管理器或 7-Zip 打开并看到上表全部目录。
- [ ] `\BOOT\BCD` 与 `\EFI\MICROSOFT\BOOT\BCD` 字节内容不同（分别来自 PCAT 与 EFI）。
- [ ] `\BOOTMGR` 与 `\BOOTMGR.EFI` 均在卷根，`\EFI\MICROSOFT\BOOT\BOOTMGFW.EFI` 存在。
- [ ] UEFI 虚拟机从 ISO 启动进入 PE；Legacy BIOS 虚拟机同样能进入。
- [ ] 附加镜像生成的 `\FWIMG\INSTALL.WIM`（或 `.ESD`）可被 PE 端「安装系统」扫描到。
- [ ] 选择超过 4 GB 的 install.wim 时报"ISO 内单个文件不能超过 4 GB"，且不产生半成品。
- [ ] 在未安装 ADK 的机器上生成成功（`HostBcdAvailable` 为 true 时不需要管理员）。
- [ ] 生成结束后 `%TEMP%\firewin-iso-<guid>` 已删除，挂载的源 ISO 已卸载。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 0xc000000f | 两份 BCD 是否同一份内容 | 分别取 PCAT / EFI BCD，缺失才用 `CreateBcdStoreAtAsync` 生成 |
| Not Found 且提示 `\bootmgr.efi` | 卷根是否有 `BOOTMGR.EFI` | 补 `root.AddFile("BOOTMGR.EFI", bootmgfw)` |
| 资源管理器看不到 `EFI\Microsoft` | 主卷是否被截断成 `MICROSOF` | 检查 `ToIsoIdentifier` 的 30 字符上限与 Joliet 卷 |
| 生成报缺 `etfsboot.com` / `efisys` | 宿主 `C:\Windows\Boot\DVD` 是否完整 | 换完整 Windows 10/11 生成，或改用写 U 盘 |
| ISO 体积正常但无法启动 | El Torito 引导目录的 LBA / 扇区数 | 核对 LBA 20 的 `BootCatalogLba` 与两个 `WriteBootEntry` |
| 大镜像报 4 GB 错误 | 附加镜像体积 | 改用 U 盘（exFAT）或拆分 SWM |
| 生成极慢 | 单个 wim 体积与磁盘速度 | 属正常顺序写入；进度按文件字节推进 |

## 相关 skill

- firewin-usb-boot：另一交付形态，U 盘不受 4 GB 单文件限制；本次 ISO 复用其 `CreateBcdStoreAtAsync` 与宿主引导文件约定。
- firewin-pe-build：提供 `boot.wim` / `boot.sdi`，并说明 ADK 路线（copype + MakeWinPEMedia）与本自研写入器的差异。
- firewin-overview：仓库总览与各 skill 的分工关系，先读它再定位到具体流程。
