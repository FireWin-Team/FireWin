---
name: firewin-pe-apps
description: Download and package the 21 WinPE tools into pe apps and verify the Tools layout and x64 payload for FireWin. Use when adding or refreshing built-in tools. Not for WIM builds or driver injection.
---

# PE 内置工具集下载与打包

download-pe-apps.ps1 把 21 个 x64 工具下载到 `pe\apps\<目录>`，build-minipe.ps1 再整体复制进 boot.wim 的 `\Tools\Apps`，由 FireWinPE.exe 扫描成桌面与开始菜单按钮。

## 适用范围

- 首次拉取或刷新内置工具，补齐缺失目录。
- 只补某一个失败的工具，或强制重下某个包。
- 调整下载超时、落地目录，核对下载来源。
- 排查工具不出现、点了报错、32 位无法启动。
- 控制 `pe\apps` 体积，避免 boot.wim 超过 1200 MB。
- 核对 `\Tools` 五件套、payload 同步与旧文件残留。

## 不适用范围

- 解包或修改 WIM/ESD 镜像，WIM 只能用 wimlib-imagex，不能用 7z。
- 提取驱动、注入 boot.wim，见 firewin-driver-injection。
- U 盘分区与写入，见 firewin-usb-boot。

## 关键文件与符号

| 路径 | 作用 | 关键符号 |
|---|---|---|
| scripts\download-pe-apps.ps1 | 21 个工具清单与下载入口 | `$tools`、`Get-ToolCandidates` |
| scripts\download-pe-apps.ps1 | 来源解析 | `Get-GitHubLatestAssetUrl`、`Get-WingetInstallerUrl`、`Get-ChromeForTestingUrl`、`Get-PageLink` |
| scripts\download-pe-apps.ps1 | 解包与判定 | `Expand-ZipKeeping`、`Expand-ArchiveKeeping`、`Test-HasExe`、`Get-PackageKind` |
| scripts\download-pe-apps.ps1 | 参数与临时目录 | `$AppsDirectory`、`$Only`、`$Force`、`$TimeoutSeconds`、`$env:TEMP\firewin-pe-apps` |
| scripts\build-minipe.ps1 | 复制工具进镜像 | `robocopy /S /XF`、`$targetTools`、`$targetApps` |
| scripts\build-minipe.ps1 | Tools 五件套与 payload | `$toolsSource`、`$vendorTools`、`-AssemblePayload`、`payload\pe` |
| scripts\build-minipe.ps1 | 补齐 PE 缺失系统 DLL | `pe\runtime`、`comctl32.dll`、`oledlg.dll`、`msi.dll` |
| pe\main.cpp | 扫描与按钮生成 | `ScanAppTools`、`PickAppExe`、`IsX64Executable`、`kKnownApps`、`HasStaticButtonFor` |
| pe\apps | 下载落地目录，也是构建输入 | 每个工具一个子目录 |
| tools\7za.exe | 解嵌套 7z 的唯一工具 | ToDesk 的 `app64.7z` |

## 核心流程

1. 参数落地：`$AppsDirectory` 默认 `<repo>\pe\apps`；临时目录固定 `%TEMP%\firewin-pe-apps`；强制 TLS 1.2；`$ErrorActionPreference = 'Stop'`。
2. 遍历 `$tools`，先按 `-Only` 过滤（`-notcontains`，大小写不敏感，名称必须等于 `Directory`）。
3. 若目标目录存在且 `Test-HasExe` 递归找到任意 `.exe`，未加 `-Force` 时输出 `[SKIP] 已存在`。
4. 解析候选地址，顺序为 ChromeForTesting、WingetId、GitHubRepo、PageUrl、Url 数组，最后追加 `UrlFallback`；单个候选失败会继续下一个。
5. 下载到 `%TEMP%\firewin-pe-apps\<URL 文件名>`，超时用 `-TimeoutSeconds`（默认 600）。
6. 有 `NestedArchive` 时走 ToDesk 分支：`tools\7za.exe x <安装包> -o<nestedRoot> -y`，找到 `app64.7z`，再 `7za x` 到目标目录，删除 `uninst.exe`、`updater.exe`、`CrashReport.exe`、`codec_checker.exe`。
7. 普通分支先看条目 `Kind`，否则按扩展名：`.zip` 用 `Expand-Archive`，`.rar`/`.7z` 用 `tar.exe -xf`，其余按单 exe 复制并可用 `ExeName` 改名。
8. zip 解压到临时目录后，若只有一层目录则自动下钻；`SubPath` 指定子目录（如 `PA_Green\x64`），再整体复制到目标。
9. 每个工具独立 try/catch，成功打印 `[OK]`，失败打印 `[FAIL] 原因`，不影响其它工具。
10. 构建挂载后复制五件套到 `\Tools`：`FireWinPE.exe`、`FireBoot.exe`（缺失仅告警）、`wimlib-imagex.exe`、`libwim-15.dll`、`7za.exe`。
11. 用 `robocopy <pe\apps> <挂载>\Tools\Apps /S /XF 说明.txt *.pdb *.chm` 复制工具，退出码大于等于 8 直接抛错。
12. 把 `pe\runtime\System32` 与 `pe\runtime\SysWOW64` 下的文件整体复制回镜像（含 `comctl32.dll`、`oledlg.dll`、`msi.dll`），避免工具报 14001 或缺少 DLL。
13. `-AssemblePayload` 时把 boot.wim、boot.sdi 与五件套复制到 `payload\pe`；`payload\pe\Tools` 不含 Apps，Apps 已在 boot.wim 内。
14. PE 运行时 `ScanAppTools` 扫描 `exe 目录\Apps`：先按 `kKnownApps` 固定顺序，再扫未知目录归入「其它工具」；静态目录 `7-Zip`、`Notepad++`、`Explorer++`、`SDelete` 不生成动态按钮。

| 目录情况 | 按钮结果 |
|---|---|
| 名称命中 `kKnownApps` 且有 x64 exe | 按固定分类和说明生成按钮 |
| 名称命中但目录缺失或只有 32 位 exe | 静默跳过，不生成按钮 |
| 未知目录且有 x64 exe | 按钮文字用目录名，分类为「其它工具」 |
| 静态目录 `7-Zip`、`Notepad++`、`Explorer++`、`SDelete` | 跳过，由启动器静态按钮负责 |
| 目录内多个 x64 exe | 先找与 exeHint 同名，再包含关键字，再与目录同名，最后取第一个 |

## 参数或清单表

| 目录 | 来源类型 | 具体来源或版本策略 | 形态 |
|---|---|---|---|
| Everything | 固定直链 | voidtools `Everything-1.4.1.1028.x64.zip` | zip |
| Dism++ | GitHub Release | `Chuyu-Team/Dism-Multi-language`，附件 `^Dism\+\+.*\.zip$` | zip |
| CrystalDiskInfo | 固定直链 | SourceForge 9.9.5，`CrystalDiskInfo9_9_5.zip` | zip |
| ProcExp | 固定直链 | `live.sysinternals.com/procexp64.exe` | 单 exe |
| NTPWEdit | 固定直链 | `cdslow.org.ru/files/ntpwedit/ntpwed07.zip` | zip |
| CPU-Z | 固定直链 | cpuid `cpu-z_2.15-en.zip` | zip |
| DiskGenius | 固定直链 | eassos `DGLite6201829_x64.zip` | zip |
| TestDisk | 固定直链 | cgsecurity `testdisk-7.3-WIP.win64.zip` | zip |
| SystemInformer | GitHub Release | `winsiderss/systeminformer`，附件 `-bin\.zip$` | zip |
| Q-Dir | 固定直链 | softwareok `Q-Dir_Portable_x64.zip` | zip |
| Autoruns | 固定直链 | Sysinternals `Autoruns.zip` | zip |
| TCPView | 固定直链 | Sysinternals `TCPView.zip` | zip |
| SDelete | 固定直链 | Sysinternals `SDelete.zip` | zip |
| Chrome | Chrome for Testing | last-known-good Stable 的 `win64` 包 | zip |
| RustDesk | GitHub Release | `rustdesk/rustdesk`，附件 `x86_64\.exe$` | 单 exe |
| ToDesk | 固定直链 + 嵌套 | `ToDesk_Setup.exe` 内取 `app64.7z`，用 7za 二次解压 | NSIS |
| AOMEI | 固定直链 + Referer | `PAGreen.zip`，实际是 7z，`SubPath=PA_Green\x64` | archive |
| BOOTICE | 官网页面 + 直链兜底 | `ipauly.com` 页面正则，失败退 `BOOTICE_v1.3.4.0_en.zip` | zip |
| WinNTSetup | 官网页面 | `winntsetup.com` 页面正则，无直链兜底 | rar |
| HWiNFO | 官网页面 | `hwinfo.com/download/` 匹配 `hwi64_\d+\.exe` | 单 exe |
| GPU-Z | winget-pkgs | `TechPowerUp.GPU-Z` 清单内 `InstallerUrl` | 单 exe |

### 参数表

| 参数 | 默认值 | 作用 |
|---|---|---|
| `-AppsDirectory` | `pe\apps` | 工具落地根目录 |
| `-Only` | 空，处理全部 | 只处理指定 `Directory`，逗号分隔 |
| `-Force` | 关 | 已存在也重新下载并覆盖 |
| `-TimeoutSeconds` | `600` | 单文件下载超时秒数 |

`-Only` 与 `-Force` 可组合使用；`-Only` 不会删除未列出的工具目录，未列出的保持原样。

## 约束与坑

1. 现象：工具目录里有 exe，PE 桌面却没有按钮。原因：`PickAppExe` 只接受 `IMAGE_FILE_MACHINE_AMD64`，PE 无 WoW64，32 位 exe 被静默跳过。正确做法：核对 exe 为 x64；不要放 32 位主程序充数。
2. 现象：下载了 32 位工具脚本却显示 `[OK]`。原因：`Test-HasExe` 只数任意 `.exe`，不校验架构；winget 分支在无 x64 链接时会回退取第一条 `InstallerUrl`。正确做法：下载后自行检查 PE 头或 `dumpbin /headers` 的 machine 字段。
3. 现象：一个半解压的目录以后再也不会更新。原因：目标只要有一个 exe 就判定「已存在」并 SKIP，缺文件、缺子目录都发现不了。正确做法：删除该目录，或用 `-Force` 重下。
4. 现象：`-Only Dism` 什么都没做，也没报错。原因：匹配的是完整 `Directory`（`Dism++`），未知名称被静默 `continue`。正确做法：用 `-Only 'Dism++'` 这类精确目录名；批量时用逗号分隔。
5. 现象：ToDesk 下载成功但报「缺少 7za.exe」。原因：嵌套解包固定读取 `<repo>\tools\7za.exe`，不读 PATH。正确做法：保留仓库 `tools\7za.exe`；该文件也随构建进 `\Tools`。
6. 现象：AOMEI 解压报「不是有效的 zip」。原因：官方 `PAGreen.zip` 实际是 7z 格式，条目用 `Kind = 'archive'` 强制走 `tar.exe`，并且只取 `PA_Green\x64`。正确做法：不要删 `Kind` 和 `SubPath`；tar 失败时手工用 7za 解压到 `pe\apps\AOMEI`。
7. 现象：CrystalDiskInfo 下载得到 HTML 或 403。原因：SourceForge 直链在部分网络被拦截。正确做法：从 crystalmark.info 下 portable zip 手工解压到 `pe\apps\CrystalDiskInfo`。
8. 现象：WinNTSetup 或 HWiNFO 突然 FAIL。原因：两者只有 `PageUrl` 解析，正则匹配页面里的 rar/exe，没有直链兜底。正确做法：手工下载官方包解压到对应目录；页面改版时更新 `PagePattern`。
9. 现象：删除了某个旧工具，重新构建后它仍在 PE 里。原因：`robocopy /S` 只复制不删除，挂载目录或已提交 WIM 里残留旧文件；boot.wim 只增不减。正确做法：从干净 boot.wim 重新构建，或构建前清空挂载点 `\Tools\Apps`；不要指望 `/S` 清理残留。
10. 现象：把新工具直接放进 `payload\pe\Tools`，PE 里看不到。原因：payload 的 Tools 只放五件套，工具必须先进 boot.wim 的 `\Tools\Apps`。正确做法：放 `pe\apps`，重跑构建并加 `-AssemblePayload`。
11. 现象：用 7za 解开 boot.wim 后 PE 无法启动或文件权限异常。原因：WIM 含硬链接、ACL、启动标记，7z 只能部分还原。正确做法：WIM/ESD/SWM 一律用 `wimlib-imagex apply` 或 `dism /Apply-Image`，7za 只用于 ToDesk 这类普通 7z。
12. 现象：工具一启动就报 14001 或缺少 `comctl32.dll`。原因：WinPE 裁掉了 comctl32、oledlg、msi，而多数工具静态依赖。正确做法：保留 `pe\runtime` 并让构建补齐；手工组装 payload 时也要补。
13. 现象：SDelete 已下载却没有独立按钮，7-Zip/Notepad++/Explorer++ 不在下载清单里。原因：这四个是启动器静态按钮目录，`HasStaticButtonFor` 会跳过动态生成；后三个由仓库自带，不属于 21 个网络条目。正确做法：不要以「目录数与 21 不符」判定失败。
14. 现象：加入 Chrome 后 boot.wim 超过 1200 MB。原因：Chrome 是最大单包，且 `pe\apps` 全量进 WIM。正确做法：先清理 `pe\apps` 中不用的目录，再构建；体积门禁在提交后才检查。
15. 现象：加 `-Force` 重下后目标目录里仍混着旧版本的 DLL。原因：解压只做覆盖复制，不先清空目标目录，多余文件会保留。正确做法：需要干净版本时先删除目录再下载。
16. 现象：手工把 `Dism++` 改成 `Dism` 后工具仍出现，但跑到「其它工具」且没有说明。原因：`kKnownApps` 的 folder 与脚本 `Directory` 不一致，未知目录兜底扫描仍会收录。正确做法：两边目录名保持完全一致。

## 验证清单

- [ ] `.\scripts\download-pe-apps.ps1 -Only RustDesk` 只改动 `pe\apps\RustDesk`，输出 `[OK]`。
- [ ] 每个工具目录至少一个 exe，用 PE 头确认 Machine 为 AMD64，无 0 字节文件。
- [ ] 没有残留的 `app64.7z`、`uninst.exe`、`updater.exe` 等无用文件。
- [ ] `pe\apps` 总大小可控，构建后 boot.wim 显示低于 1200 MB。
- [ ] 构建日志 robocopy 退出码小于 8，挂载点 `\Tools\Apps` 目录数与 `pe\apps` 一致。
- [ ] `\Tools` 同时存在 FireWinPE.exe、FireBoot.exe、wimlib-imagex.exe、libwim-15.dll、7za.exe（FireBoot 缺失只告警）。
- [ ] `payload\pe` 含 boot.wim、boot.sdi 与五件套，且没有 Tools\Apps。
- [ ] PE 桌面按钮与开始菜单分类正确，未知目录落入「其它工具」，无 32 位空按钮。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 工具按钮不出现 | 目录内是否有 x64 exe | 换 x64 版本 |
| 下载 `[SKIP] 已存在` 但文件旧 | 目标目录是否含任意 exe | 删除目录或加 `-Force` |
| `-Only` 无效果 | 名称是否等于 `Directory` | 用精确名称并加引号 |
| ToDesk 解包失败 | `tools\7za.exe` 是否存在 | 恢复 7za 后重跑 |
| AOMEI 解压失败 | `Kind`、`SubPath` 是否被改 | 恢复 archive 与 PA_Green\x64 |
| 下载得到 HTML | 源站是否 403 或改版 | 手工下载官方包解压 |
| 工具报 14001 | `pe\runtime` 与构建补齐日志 | 补 comctl32/oledlg/msi |
| 构建后旧工具仍在 | 挂载点或 WIM 是否干净 | 从干净 boot.wim 重建 |
| boot.wim 超 1200 MB | `pe\apps` 各目录大小 | 删除不用的工具后重建 |
| `-Force` 后仍混有旧 DLL | 目标目录是否先清空 | 删除目录后重下 |

## 相关 skill

- `firewin-pe-runtime`：负责 `\Tools\Apps` 扫描、`kKnownApps` 顺序与按钮生成。
- `firewin-driver-injection`：与本 skill 共同占用 boot.wim 的 1200 MB 预算。
- `firewin-usb-boot`：消费 `payload\pe` 并制作启动盘，负责最终分发。
