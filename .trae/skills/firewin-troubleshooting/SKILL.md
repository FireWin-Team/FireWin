---
name: firewin-troubleshooting
description: Diagnose FireWin boot, U disk, PE build and deployment failures by symptom; use for error codes such as 0xc000000f, 14001, BCD or WIM issues; not for feature development.
---

# FireWin 故障排查手册

按现象索引的排障手册：先定位故障发生阶段（宿主制作/PE 内/引导期），再用日志与命令取证，最后最小修复。

## 适用范围

- 桌面端制作或升级 U 盘失败，报 `bcdedit`、`diskpart`、空间不足等错误
- U 盘或 ISO 启动失败，出现 `0xc000000f`、黑屏、自动进 PE
- PE 内工具无法启动（14001）、中文显示方块、32 位程序报错
- `boot.wim` 过大、DISM 注入驱动整批失败、镜像无法生成
- 下载 PE 工具失败、ADK 安装退出码异常、桌面端发布报 CS2012

## 不适用范围

- 从零构建/发布流程与参数说明，见 `firewin-build-release`
- 新功能开发、界面调整；只处理可复现的故障
- 与 FireWin 无关的通用 Windows 启动修复（可用系统自带工具，但不在本手册范围）

## 关键文件与符号

| 文件/符号 | 用途 |
|---|---|
| `src/FireWin/Services/UsbBootService.cs` | `UpgradeBootDiskAsync`、`FindOwnVolumeLetter`、`PickFreeDriveLetter`、`HideBootPartitionAsync`、`CanUpgradeInPlace` |
| `src/FireWin/Services/BcdBootService.cs` | `CreateBcdStoreAsync`、`{ramdiskoptions}` 复用、`/bootsequence` 写入与清理 |
| `src/FireWin/Services/IsoImageService.cs` | ISO 文件名单一化、单文件 4 GB 上限 |
| `src/FireWin/Services/DeployService.cs` | `deploy.log` 写入、`DeployException` 文案 |
| `src/FireWin/Services/WimInitService.cs` | wimlib 初始化与 `--wim-init-check` |
| `src/FireWin/App.xaml.cs` | CLI 参数 `--version`、`--adk-status`、`--adk-install`、`--wim-init-check`、`--demo-image` |
| `pe/main.cpp` | PE 端 `ResolveVolumeToDriveLetter`、`FireWinPE-boot.log`、32 位 exe 判定 |
| `pe/boottool.cpp` | `FireBoot.exe`：写 `/bootsequence`、重启进 PE |
| `scripts/build-minipe.ps1` | 1200 MB 上限、SxS Winners 注册、`WinPE-FontSupport-ZH-CN` |
| `scripts/extract-drivers.ps1` | 默认 `-SkipInf '*:^c_'`、`Get-MissingPackageFiles` |
| `scripts/download-pe-apps.ps1` | 单工具下载失败不中断，`-Only` 重试 |
| `scripts/install-adk.ps1` | ADK 与 WinPE 组件安装，接受退出码 3010 |
| `payload\pe\` | `boot.wim`、`boot.sdi`、`Tools\`；旧 `Tools\Apps` 残留源头 |

## 核心流程

1. 先取版本与自检，判断是环境问题还是流程问题。

```powershell
.\dist\desktop\FireWin.exe --version
.\dist\desktop\FireWin.exe --adk-status
.\dist\desktop\FireWin.exe --wim-init-check
.\dist\desktop\FireWin.exe --adk-install
.\dist\desktop\FireWin.exe --demo-image .\demo.wim
```

判断：`--adk-status` 报缺组件 → 先 `--adk-install`（管理员，3010 需重启）；`--wim-init-check` 失败 → 查 `Tools\libwim-15.dll` 是否为 x64；`--demo-image` 是内部演示参数，直接进入镜像页并完成识别（供自动化截图，不写盘）。

2. 划分故障阶段。

```text
制作/升级（宿主） → PE 内操作（安装/驱动/网络） → 引导期（UEFI/BIOS 加载 PE） → 部署到目标系统
```

判断：桌面端弹 `DeployException` 文案 → 宿主阶段；PE 能进但按钮报错 → PE 阶段；出现固件错误码/黑屏 → 引导阶段。

3. 取日志（只读，不改现场）。

```powershell
notepad C:\FireWinPE\deploy.log                       # 桌面端与 PE 部署主日志
notepad X:\Tools\FireWinPE-install.log                # PE 内安装日志，exe 同目录
notepad $env:TEMP\FireWinPE-boot.log                  # PE 启动早期日志，X:\Windows\Temp
```

判断：`deploy.json` 未给 `logPath` 或写入失败时，PE 退回 `Tools\deploy.log`；`FireWinPE-boot.log` 为空说明卡在更早的 BCD/ramdisk 阶段。

4. BCD 取证（离线挂载 U 盘启动分区，盘符以实际为准）。

```powershell
bcdedit /store S:\boot\bcd /enum all
bcdedit /store S:\EFI\Microsoft\Boot\BCD /enum all
bcdedit /enum {bootmgr}
bcdedit /enum {ramdiskoptions}
```

判断：`{bootmgr}` 下出现 `bootsequence` → 每次开机进 PE；`{ramdiskoptions}` 的 `ramdisksdidevice` 不是本盘 → 条目被别的工具覆盖。

5. 磁盘与分区取证。

```powershell
diskpart
# list disk / list volume / select volume N / detail volume
Get-Volume | Select-Object DriveLetter,FileSystemLabel,FileSystem,Size
```

判断：存在卷标 `FireWinPE` 但 `DriveLetter` 为空 → 隐藏启动分区需临时盘符；数据分区卷标 `FireData` 缺失 → 不是本工具制作的双分区布局。

6. 镜像取证。

```powershell
Get-Item .\payload\pe\boot.wim | Select-Object Length
& .\tools\wimlib-imagex.exe info .\payload\pe\boot.wim
& .\tools\wimlib-imagex.exe dir .\payload\pe\boot.wim 1 \Tools\Apps
```

判断：`boot.wim` 超过 1200 MB → 无法在常见内存下启动；`Tools\Apps` 里缺目录 → 启动器按钮缺失。

## 故障索引表
### 1. U 盘升级报 bcdedit /createstore 文件已存在
- 现象：升级接近完成时报 `bcdedit` 无法创建存储，`\boot\bcd` 或 `bcd.efi` 已存在。
- 根因：旧 BCD 文件只读/被占用，或上次升级中断留下半成品。
- 诊断：`attrib S:\boot\bcd`、`attrib S:\EFI\Microsoft\Boot\BCD`、`dir /a S:\boot`。
- 修复：升级前用 `bcdedit /store S:\boot\bcd /enum` 确认可读；代码先写 `bcd.new` 再替换，手工则 `attrib -r -s -h` 后删 `bcd.new` 重试。
- 验证：`bcdedit /store S:\boot\bcd /enum` 能看到 `{default}` 指向 ramdisk。
### 2. 启动报 0xc000000f
- 现象：UEFI 启动显示 `0xc000000f`，找不到 `\EFI\Microsoft\Boot\BCD` 或 `BOOTMGR.EFI`。
- 根因：ISO 用 ISO9660 短名，长文件被截断；或 BCD 未写入/路径大小写不符；或 U 盘根目录缺 `bootmgr`/`bootmgfw.efi`。
- 诊断：`7z l dist\FireWinPE.iso | findstr /i "BCD BOOTMGR"`；挂载后 `dir S:\EFI\Microsoft\Boot`、`dir S:\sources`。
- 修复：重跑 `build-minipe.ps1`（ISO 写入 Joliet 长名，或改用 `dist\FireWinPE.udf.iso`）；U 盘场景用桌面端「升级」重建 BCD。
- 验证：虚拟机 UEFI 能进 PE，`bcdedit /store S:\boot\bcd /enum` 的 `device` 为 `boot`。
### 3. boot.wim 超 1200 MB 无法启动
- 现象：`build-minipe.ps1` 报 `boot.wim` 超过 1200 MB，或 PE 启动卡在转圈/蓝屏。
- 根因：ramdisk 整份载入内存，镜像过大超过低内存机可用 RAM；工具/驱动/Font 包堆叠。
- 诊断：`Get-Item .\payload\pe\boot.wim | Select Length`；`wimlib-imagex dir` 看 `Tools\Apps` 体积。
- 修复：删 `pe\apps` 中不用的工具、精简 `pe\drivers`，去掉 `WinPE-NetFx`/`WinPE-PowerShell` 后重建。
- 验证：新 `boot.wim` 小于 1200 MB，2 GB 内存虚拟机可进 PE。
### 4. DISM 注入 c_* 驱动整批失败
- 现象：`Add-Driver` 返回失败，原本正常的驱动也一条没注入。
- 根因：`c_*.inf` 引用了未打包的文件，DISM 任一条失败即整批中止。
- 诊断：`Get-ChildItem .\pe\drivers -Directory | Where-Object Name -like 'c_*'`；对目录单独执行 `dism /Add-Driver` 复现。
- 修复：删除 `pe\drivers` 下全部 `c_*` 目录，或 `extract-drivers.ps1 -Clean -Force` 重取（脚本默认 `-SkipInf '*:^c_'`）。
- 验证：重建镜像时 DISM 段无错误，PE 内设备管理器无未知设备批量报错。
### 5. PE 缺 comctl32/oledlg 报 14001
- 现象：工具启动弹「并行配置不正确」，错误代码 14001；`explorer`、部分安装器同样失败。
- 根因：精简 PE 缺少 SxS `Winners` 注册，`comctl32.dll`/`oledlg.dll` 的 side-by-side 依赖解析失败。
- 诊断：`reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\SideBySide\Winners" /f comctl32`；`dir X:\Windows\System32\comctl32.dll`。
- 修复：用 `-RuntimeDirectory .\pe\runtime` 重建，脚本会注入 SxS 注册与运行库文件。
- 验证：PE 内 `reg query ...\Winners` 有输出，`explorer.exe` 与工具都能启动。
### 6. zh-cn 字体方块
- 现象：PE 中文界面全是方块，按钮文字不可读。
- 根因：未安装 `WinPE-FontSupport-ZH-CN`，镜像内只有西文字体。
- 诊断：挂载 `boot.wim` 后 `dism /Get-Packages /Image:...`，搜 `FontSupport`。
- 修复：安装 ADK 的 zh-cn WinPE 语言包，`build-minipe.ps1 -Language zh-cn` 重建。
- 验证：PE 启动后中文字体正常，`X:\Windows\Fonts` 含 `msyh`/`simsun` 类字体。
### 7. 盘符漂移与 ESP 无盘符
- 现象：PE 内部署报找不到系统盘/引导分区；`deploy.json` 里的卷 GUID 对应卷无盘符。
- 根因：`bcdboot`、DISM 需要盘符，而 ESP/隐藏分区默认不分配；盘符在重启后漂移。
- 诊断：`type X:\Tools\deploy.json`；`diskpart` → `list volume` → `detail volume`。
- 修复：在 PE 中 `diskpart`：`select volume N` + `assign letter=S`，用 `S:` 重试；卷 GUID 仍匹配则不必改配置。
- 验证：`bcdboot` 成功返回，目标系统重启进入 OOBE/桌面。
### 8. {ramdiskoptions} 全局复用冲突
- 现象：引导其他 WIM 工具后，FireWin 的 PE 启动项指向错误设备，或反之。
- 根因：`{ramdiskoptions}` 是全局共享条目，多个工具都改写 `ramdisksdidevice`，后写覆盖先写。
- 诊断：`bcdedit /enum {ramdiskoptions}`，核对 `ramdisksdidevice` 是否为当前 U 盘分区。
- 修复：删除冲突条目后由本工具重新生成；只清理本工具创建的条目，不要盲删系统条目。
- 验证：`bcdedit /enum {ramdiskoptions}` 指向本盘 `boot.sdi`，其他 WIM 引导项仍可启动。
### 9. /bootsequence 遗留导致每次开机进 PE
- 现象：每次重启都自动进 FireWin PE，Windows 无法成为默认项。
- 根因：`FireBoot.exe` 写入的一次性 `/bootsequence` 未被清除（PE 崩溃、用户强制断电、重复点击重启）。
- 诊断：`bcdedit /enum {bootmgr}` 中出现 `bootsequence` 行。
- 修复：`bcdedit /deletevalue {bootmgr} bootsequence`；再 `bcdedit /delete {GUID} /f` 删除孤立 PE 项。
- 验证：重启进入 Windows；`bcdedit /enum {bootmgr}` 不再有 `bootsequence`。
### 10. ISO 单文件超 4 GB
- 现象：生成 ISO 时抛错，提示单文件超过 4 GB。
- 根因：ISO9660/Joliet 单文件上限 4 GB，大镜像/工具无法放入。
- 诊断：`Get-ChildItem -Recurse .\pe\apps | Sort Length -Desc | Select -First 5`。
- 修复：大文件移出 ISO，改放 U 盘数据分区 `FWIMG`（exFAT），或精简工具。
- 验证：ISO 能生成并启动，PE 能从 `FWIMG` 读到镜像。
### 11. 宿主缺 C:\Windows\Boot\DVD 文件
- 现象：制作 U 盘报缺少 `C:\Windows\Boot\DVD\...` 下的 `bootmgr`/`bootmgfw.efi`/字体。
- 根因：宿主 Windows 精简过 Boot 目录，或不是 Windows 10/11 完整安装。
- 诊断：`Test-Path C:\Windows\Boot\DVD\PCAT\bootmgr`、`Test-Path C:\Windows\Boot\DVD\EFI\bootmgfw.efi`。
- 修复：换完整 Win10/11 宿主机，或从同版本系统复制 `C:\Windows\Boot\DVD` 后再制作。
- 验证：制作流程走完，U 盘根目录有 `bootmgr` 与 `EFI\Boot\BOOTX64.EFI`。
### 12. PE 无 WoW64 导致 32 位程序无法运行
- 现象：运行 32 位 exe 报「不是有效的 Win32 应用程序」或直接无响应。
- 根因：WinPE 只含 64 位子系统，没有 `SysWOW64`，无法加载 32 位 PE。
- 诊断：`dir X:\Windows\SysWOW64`；对 exe 用 `dumpbin /headers` 看是否为 x86。
- 修复：改用 64 位版本工具（`download-pe-apps.ps1` 只收 64 位）；确需 32 位则改在完整系统侧执行。
- 验证：工具能启动并完成原功能。
### 13. 下载脚本失败与 ADK 安装退出码 3010
- 现象：`download-pe-apps.ps1` 打印 `FAIL` 后继续；ADK 安装返回 `3010`。
- 根因：GitHub 直链被限速/阻断；3010 表示安装成功但需重启。
- 诊断：`.\scripts\download-pe-apps.ps1 -Only 工具名 -Force` 复现；看 `$LASTEXITCODE`。
- 修复：配置代理后重试或手工把解压目录放到 `pe\apps\工具名`；3010 只需重启再跑第 3 步。
- 验证：`pe\apps\工具名` 内有 exe，重建后 PE 内按钮可启动。
### 14. 桌面端发布 CS2012
- 现象：`dotnet publish` 报 `CS2012`，无法写入 `obj`/`bin` 下文件。
- 根因：上一轮 `FireWin.exe` 仍在运行，或杀软/索引器占用输出文件。
- 诊断：`Get-Process FireWin`；`Get-Process | Where-Object Path -like '*FireWin*'`。
- 修复：结束进程，删除 `src\FireWin\bin`、`src\FireWin\obj` 后重发；给仓库目录加杀软排除。
- 验证：发布成功，`dist\desktop\FireWin.exe` 可启动且 `--version` 正常。
### 15. payload 旧 Apps 残留
- 现象：`payload\pe\Tools\Apps` 出现数百 MB 旧工具，发布包异常膨胀且版本对不上。
- 根因：旧构建或手工复制把 `pe\apps` 镜像进 payload；csproj 按原样复制 payload。
- 诊断：`Get-ChildItem .\payload\pe\Tools\Apps -Recurse -File | Measure-Object Length -Sum`。
- 修复：删除 `payload\pe\Tools\Apps`，用 `build-minipe.ps1 -AssemblePayload` 重新生成 payload。
- 验证：`payload\pe\Tools` 仅剩五个文件，`dist\desktop` 体积回落。
### 16. 升级时隐藏分区无盘符
- 现象：升级报「无法挂载启动分区（可能属于其他磁盘）」或「启动分区剩余空间不足」。
- 根因：双分区布局的 `FireWinPE` 分区默认隐藏；临时分配盘符失败，或 FAT32 空间放不下新 wim。
- 诊断：`diskpart` → `list volume`，找卷标 `FireWinPE`；`Get-Volume` 看剩余空间。
- 修复：手工 `assign letter=S` 后重试升级；空间不足只能改用「全新制作」重新分区。
- 验证：升级完成后启动分区恢复隐藏，PE 版本与 payload 一致。

## 约束与坑

1. 先取证再动手 → 现象是「进不去 PE」，原因可能是 BCD、ramdisk、wim 大小；未看 `FireWinPE-boot.log` 就重做 U 盘会丢现场。
2. 不要盲删 `{ramdiskoptions}` → 它是全局条目，可能被其他 WIM 工具复用；只删除本工具创建且确认无其他引用的条目。
3. `bcdedit` 必须带 `/store` → 在宿主上改的是本机 BCD，误操作会让本机无法启动；对 U 盘一律 `bcdedit /store S:\boot\bcd`。
4. 驱动不要手工塞 `c_*` → 整批 DISM 注入会失败，正确做法是让 `extract-drivers.ps1` 过滤。
5. 不要用 32 位工具验证 PE → PE 无 WoW64，失败不代表工具损坏；统一用 x64 版本。
6. 日志默认路径可能退化 → 无 `C:\` 写权限时 PE 写 `Tools\deploy.log`；先看 exe 同目录再判定「没有日志」。
7. 升级空间判断按「新 wim 减旧 wim + 64 MB」 → FAT32 不能扩容，剩余不足时只能全新制作，不要反复重试升级。
8. `--adk-install` 需要管理员 → 未提权时安装必定失败，且 3010 必须重启后才生效。

## 验证清单

- [ ] `--version`、`--adk-status`、`--wim-init-check` 三项自检均正常
- [ ] 目标 U 盘能被 UEFI 与 Legacy BIOS 分别启动到 PE
- [ ] PE 内中文显示正常，无方块
- [ ] PE 内 `explorer`、安装器等依赖 comctl32 的程序无 14001
- [ ] PE 内可枚举目标磁盘与 ESP，盘符可临时分配
- [ ] 部署后重启进入目标系统，未再自动进 PE
- [ ] `bcdedit /enum {bootmgr}` 无 `bootsequence` 残留
- [ ] 升级后 `sources\boot.wim` 与 `payload\pe\boot.wim` 哈希一致

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 固件报 0xc000000f | ISO 内 `EFI\Microsoft\Boot\BCD` 与长文件名 | 重生成 ISO 或升级重建 BCD |
| 开机自动进 PE | `bcdedit /enum {bootmgr}` | 删 `bootsequence` 与孤立项 |
| PE 启动转圈后重启 | `boot.wim` 大小、`FireWinPE-boot.log` | 精简镜像到 1200 MB 以内 |
| 工具报 14001 | `Winners` 注册、`pe\runtime` | 带 `-RuntimeDirectory` 重建 |
| 部署找不到系统盘 | `deploy.json` 卷 GUID、`list volume` | 临时 `assign letter` 后重试 |
| 驱动一条没进 | `pe\drivers` 是否有 `c_*` | 删除后用脚本重取 |
| 发布报 CS2012 | 是否有 `FireWin.exe` 在跑 | 结束进程，清 `bin`/`obj` |

## 相关 skill

- `firewin-build-release`、`firewin-bcd-boot`、`firewin-usb-boot`、`firewin-image-deploy`：构建发布参数、BCD 写入实现、U 盘制作、系统部署流程
- `firewin-cli-automation`：用 `--adk-status`、`--wim-init-check` 与 PE 端退出码快速切分故障域
- `firewin-wpf-styling`：界面显示异常（进度条不动、日志不刷新、高 DPI 裁切）的样式侧排查
