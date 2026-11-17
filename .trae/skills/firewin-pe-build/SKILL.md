---
name: firewin-pe-build
description: Build the FireWin mini PE boot.wim and ISO with ADK copype, DISM package injection and payload assembly. Use to rebuild PE. Not for USB writing or ISO-only assembly.
---

# 迷你 PE 镜像构建（build-minipe.ps1）

用 ADK 的 copype 生成基础 PE，挂载 boot.wim 注入组件、工具、SxS 修复与启动器入口，产出可被桌面端内置的 `payload\pe\`。

## 适用范围

- 从零构建 FireWin PE：`boot.wim`、可选 ISO、桌面端随包 PE 源目录。
- 调整注入的 WinPE 可选组件、语言包、字体包、基础驱动、常用工具目录。
- 排查构建失败：找不到 ADK、DISM 依赖错误、挂载残留、robocopy 失败、boot.wim 超限。
- 排查进 PE 后第三方工具报 14001（SxS 配置错误）、中文显示方块、启动器不自动运行。
- 安装或重装 Windows ADK 主包与 WinPE 附加组件（`install-adk.ps1`）。

## 不适用范围

- 制作 U 盘启动盘或升级已有启动盘：见 firewin-usb-boot。
- 自研 ISO9660 写入器与 ISO 目录细节：见 firewin-iso-builder。
- 桌面端程序发布（`dotnet publish`）与 PE 端 C++ 编译（CMake / MSVC）：不在本 skill 范围。

## 关键文件与符号

| 路径 | 作用 | 关键参数/函数 |
|---|---|---|
| `scripts\build-minipe.ps1` | 构建主脚本 | `-Architecture`、`-WorkDirectory`、`-Language`、`-ToolsSource`、`-VendorToolsDirectory`、`-AppsDirectory`、`-DriversDirectory`、`-RuntimeDirectory`、`-OutputIso`、`-AssemblePayload`、`-PayloadOutputDirectory` |
| 同上 | 内部流程 | `Assert-Administrator`、`Get-KitsRoot`、`Find-AdkTool`、`Resolve-ToolsSource`、`Resolve-VendorTools`、`Invoke-Dism`、`Add-WinPePackage` |
| `scripts\install-adk.ps1` | ADK 与 WinPE 附加组件静默安装 | `-InstallPath`、`-AdkSetupPath`、`-WinPeSetupPath`、`-Force`、`Invoke-Winget`、`Invoke-Setup` |
| `pe\build\bin\` | PE 端产物（`FireWinPE.exe`、`FireBoot.exe`） | `-ToolsSource` 默认探测路径 |
| `pe\apps\` | 常用工具目录，递归进 `\Tools\Apps\` | `-AppsDirectory` 默认值 |
| `pe\drivers\` | 含 `.inf` 的驱动包 | 递归 `dism /Add-Driver` |
| `pe\runtime\System32`、`pe\runtime\SysWOW64` | 补齐 WinPE 裁掉的组件 | `-RuntimeDirectory` 默认值 |
| `payload\pe\` | 桌面端随包 PE（boot.wim + boot.sdi + Tools） | `-AssemblePayload` 输出 |
| `README.md` §4.3 / §8 | 用户视角流程与 ADK 说明 | 注入顺序、ADK 检测依据 |

## 核心流程

1. 确认管理员：`Assert-Administrator` 用 `WindowsPrincipal.IsInRole(Administrator)` 判断，不满足抛"需要管理员权限"（挂载 WIM 必须提权）。同时 `$ErrorActionPreference='Stop'`、`Set-StrictMode -Version Latest`。
2. 定位 ADK：`Get-KitsRoot` 依次读 `HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots` 与 `WOW6432Node` 版本的 `KitsRoot10`，拼 `$kitsRoot\Assessment and Deployment Kit`；`Deployment Tools` 与 `Windows Preinstallation Environment\amd64\WinPE_OCs` 任一不存在即抛错。
3. 找工具与补环境变量：`copype.cmd` / `MakeWinPEMedia.cmd` 由 `Find-AdkTool` 在 ADK 根目录递归找（它们在附加组件里，不在 Deployment Tools）；显式设置 `WinPERoot`、`WinPERootNoArch`、`OSCDImgRoot`、`DISMRoot`、`BCDBootRoot`、`ImagingRoot`、`WdsmcastRoot`、`WindowsSetupRootNoArch`、`USMTRootNoArch` 并把前四个加入 `PATH`。`dism` 优先 `Deployment Tools\amd64\DISM\dism.exe`，否则递归查找。
4. 解析输入目录：`Resolve-ToolsSource` 用 `-ToolsSource` 或探测 `pe\build\bin\Release`、`pe\build\bin`、`pe\build\Release`、`pe\build`（必须含 `FireWinPE.exe`）；`Resolve-VendorTools` 默认 `tools\`，强制要求 `wimlib-imagex.exe`、`libwim-15.dll`、`7za.exe`。
5. copype：`WorkDirectory` 已存在则 `Remove-Item -Recurse -Force`，然后 `cmd.exe /c ""<copype>" amd64 "<WorkDirectory>""`；校验 `media\sources\boot.wim` 存在。
6. 挂载：`New-Item <WorkDirectory>\mount`，`dism /Mount-Image /ImageFile:<boot.wim> /Index:1 /MountDir:<mount>`，成功置 `$mounted = $true`。
7. 注入可选组件（顺序固定）：`WinPE-WMI` → `WinPE-NetFx` → `WinPE-Scripting` → `WinPE-PowerShell`。`Add-WinPePackage` 先 `dism /Add-Package /Image:<mount> /PackagePath:<winPeRoot>\<名称>.cab`，再尝试同名 `_<Language>.cab`（不存在打印"跳过语言包"）。
8. 语言为 `zh-*` 时注入字体：包名 `WinPE-FontSupport-` + `$Language.ToUpperInvariant()`（即 `WinPE-FontSupport-ZH-CN.cab`），缺失仅 `Write-Warning`。
9. 注入驱动：`$DriversDirectory` 或 `pe\drivers`；目录存在且递归统计 `.inf` 数量 > 0 才 `dism /Add-Driver /Image:<mount> /Driver:<目录> /Recurse`，否则跳过并提示。
10. 复制工具到 `\Tools\`：`FireWinPE.exe`（必需）、`FireBoot.exe`（可选，缺失警告"引导工具按钮将不可用"）、`wimlib-imagex.exe`、`libwim-15.dll`、`7za.exe`。
11. 复制常用工具到 `\Tools\Apps\`：`robocopy <apps> <mount>\Tools\Apps /S /XF '说明.txt' '*.pdb' '*.chm' /NFL /NDL /NJH /NJS /NP`；退出码 `>= 8` 才抛错（0–7 属正常）。目录不存在只警告。
12. 补齐系统组件：对 `System32`、`SysWOW64` 分别把 `pe\runtime\<arch>\*` 复制到 `<mount>\Windows\<arch>`（`comctl32.dll`、`oledlg.dll`、`msi.dll` 等，缺了第三方工具直接 14001）。
13. 改写 `startnet.cmd`：读原内容（不存在则视为 `wpeinit`）。以 `Get-ChildItem <mount>\Windows\WinSxS` 匹配 `^(?<prefix>.+)_(?<version>\d+\.\d+\.\d+\.\d+)_none_[0-9a-f]+$`，只处理 `amd64_microsoft.windows.common-controls_6595b64144ccf1df_none_62fe57338acfab7a` 与 `amd64_microsoft.windows.gdiplus_6595b64144ccf1df_none_6e8eef9408f10c66` 两个前缀；对每个匹配写 `reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\SideBySide\Winners\<规范名>" /ve /d "" /f`、`...\<规范名>\<主版本.次版本>` 与 `...\<完整程序集名>` 两项，均带 `/f` 且 `>nul 2>nul`。若内容已含 `FireWinPE.exe` 就整段跳过（用 exe 名做标记，兼容旧 `/autodeploy` 版本）；确保存在 `wpeinit` 行（正则 `(?im)^\s*wpeinit\s*$`），最后追加：

```text
rem ---- FireWin launcher entry ----
if exist X:\Tools\FireWinPE.exe (
    X:\Tools\FireWinPE.exe
)
```

以 `Set-Content -Encoding ASCII`、CRLF 写回。
14. 落盘：`dism /Unmount-Image /MountDir:<mount> /Commit`，`$mounted = $false`；然后 `boot.wim` 体积按 MB 与 1200 比较，超限抛错并提示精简 `pe\apps` 或 `pe\drivers`。
15. 可选导出 ISO：`cmd.exe /c ""<MakeWinPEMedia>" /ISO "<WorkDirectory>" "<OutputIso>""`，非 0 退出即失败；输出目录不存在先创建。
16. 可选组装 payload：目录为 `-PayloadOutputDirectory` 或 `payload\pe`；复制 `boot.wim`、`<WorkDirectory>\media\Boot\boot.sdi`、`Tools\` 下 `FireWinPE.exe`、`FireBoot.exe`（存在才复制）、`wimlib-imagex.exe`、`libwim-15.dll`、`7za.exe`。
17. 失败兜底：`finally` 中若 `$mounted` 仍为真，`dism /Unmount-Image /MountDir:<mount> /Discard`，避免下次报"映像已挂载"。

ADK 安装（`install-adk.ps1`）：

1. `Get-AdkStatus`：无 `KitsRoot10` 视为未安装；`AdkInstalled` = `Deployment Tools` 存在，`WinPeAddonInstalled` = `Windows Preinstallation Environment` 存在。两者均已装且未 `-Force` 时直接 `exit 0`。
2. 主包：先 `winget install --id Microsoft.WindowsADK --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity`；失败回退 `adksetup.exe /quiet /norestart /ceip off /features OptionId.DeploymentTools`（`-InstallPath` 时追加 `/installpath`），本地文件优先于下载 `https://go.microsoft.com/fwlink/?linkid=2289980`。
3. 附加组件：先 `winget install --id Microsoft.WindowsADK.WinPEAddon`，失败回退 `adkwinpesetup.exe /quiet /norestart`（下载 `linkid=2289981`）。
4. 退出码处理：`0` 与 `3010` 都算成功，`3010` 打印"需要重启后才会完全生效"；其余抛错。安装完再次 `Get-AdkStatus` 复核，未检测到则提示重启。

## 布局或参数表

`build-minipe.ps1` 参数：

| 参数 | 默认值 | 作用/约束 |
|---|---|---|
| `-Architecture` | `amd64` | `ValidateSet` 只允许 amd64 |
| `-WorkDirectory` | `"$env:SystemDrive\WinPE_amd64"` | copype 输出，每次先删除重建 |
| `-Language` | `en-us` | 附加组件语言包后缀，`zh-cn` 触发字体包 |
| `-ToolsSource` | 自动探测 `pe\build\bin*` | 必须含 `FireWinPE.exe` |
| `-VendorToolsDirectory` | `tools\` | 必须含 wimlib-imagex / libwim-15 / 7za |
| `-AppsDirectory` | `pe\apps` | 递归进 `\Tools\Apps\`，缺失只警告 |
| `-DriversDirectory` | `pe\drivers` | 有 `.inf` 才注入 |
| `-RuntimeDirectory` | `pe\runtime` | 复制 `System32` / `SysWOW64` |
| `-OutputIso` | 空（不导出） | 交给 MakeWinPEMedia /ISO |
| `-AssemblePayload` | 关 | 额外产出桌面端 PE 源目录 |
| `-PayloadOutputDirectory` | `payload\pe` | 仅配合 `-AssemblePayload` |

产物与落点：

| 产物 | 路径 | 说明 |
|---|---|---|
| boot.wim | `<WorkDirectory>\media\sources\boot.wim` | 已注入组件与 `\Tools` |
| ISO | `-OutputIso` | MakeWinPEMedia 生成（UDF 路线） |
| 桌面端 PE | `payload\pe\{boot.wim, boot.sdi, Tools\}` | `-AssemblePayload` |
| 挂载点 | `<WorkDirectory>\mount` | 失败时 `/Discard` |

注入组件与依赖（`scripts\build-minipe.ps1` 固定顺序）：

| 组件 | 依赖 | 语言包 cab | 作用 |
|---|---|---|---|
| `WinPE-WMI` | 无 | `WinPE-WMI_<lang>.cab` | NetFx 的前置 |
| `WinPE-NetFx` | WMI | `WinPE-NetFx_<lang>.cab` | .NET 组件，纯原生工具可省略 |
| `WinPE-Scripting` | 无 | `WinPE-Scripting_<lang>.cab` | PowerShell 的前置 |
| `WinPE-PowerShell` | NetFx + Scripting | `WinPE-PowerShell_<lang>.cab` | 脚本引擎 |
| `WinPE-FontSupport-<LANG>` | 无 | 无 | 仅 `-Language zh-*` 时注入 |

## 约束与坑

1. DISM 报依赖缺失 → 注入顺序与依赖相反（PowerShell 依赖 NetFx 与 Scripting，NetFx 依赖 WMI） → 固定 `WinPE-WMI → WinPE-NetFx → WinPE-Scripting → WinPE-PowerShell`；纯原生工具可省掉 NetFx 以减小体积。
2. 第二次构建报"映像已挂载" → 上次异常退出未卸载 → `finally` 的 `/Unmount-Image /Discard` 已兜底；仍残留时手工 `dism /Cleanup-Wim`。
3. PE 里第三方工具报 14001（ERROR_SXS_CANT_GEN_ACTCTX） → WinPE 的 SOFTWARE 注册表没有 `SideBySide\Winners`，而工具带 comctl32 v6 / GdiPlus 清单 → 由 `startnet.cmd` 在启动时 `reg add` 补旧式 `名字_版本_none_哈希` 与新式 `名字_none_哈希` + `主版本.次版本` 两套键；WinSxS 里找不到程序集时只告警，需检查 `pe\runtime` 是否补齐 dll。
4. 工具报"缺少 comctl32.dll / msi.dll" → WinPE 精简掉了这些组件 → 把文件放进 `pe\runtime\System32`（与 `SysWOW64`）由脚本复制进镜像，而不是手工到 PE 里拷。
5. `startnet.cmd` 里加了新块但不生效 → 脚本用内容是否含 `FireWinPE.exe` 做幂等标记，已存在就整段跳过 → 需要重写时先把该字符串从文件里去掉再构建。
6. `startnet.cmd` 中文变问号 → 写回使用 `Set-Content -Encoding ASCII` → 追加块只用英文注释与命令，中文放到 exe 内或独立 bat。
7. boot.wim 超过 1200 MB 构建失败 → boot.wim 会整份载入内存盘，2 GB 内存机器容易起不来 → 精简 `pe\apps`（`robocopy` 只排除了 `说明.txt`、`*.pdb`、`*.chm`）与 `pe\drivers`，或移除 `WinPE-NetFx`。
8. 常用工具复制被误判失败 → `robocopy` 退出码 1–7 表示已复制/有跳过，只有 `>= 8` 才是错误 → 脚本按 `$LASTEXITCODE -ge 8` 判断，不要改成 `-ne 0`。
9. `-AssemblePayload` 后桌面端起不来或工具缺失 → 它只复制 `payload\pe\Tools` 根下 5 个文件，不复制 `Tools\Apps`，也从不清理目标目录（旧版残留的 `FireBoot.exe` 等会一直被分发） → 构建前清空 `payload\pe`，并确认 PE 运行时用的是 wim 内的 `X:\Tools\Apps` 而非 payload 目录。
10. 在普通 PowerShell 里手工跑 `copype` 失败 → copype 依赖 ADK 的环境变量（平时由 `DandISetEnv.bat` 设置） → 走脚本，由它设置 `WinPERoot`、`OSCDImgRoot`、`DISMRoot` 等。
11. 中文界面显示方块 → 缺少 `WinPE-FontSupport-ZH-CN` 字体包（脚本仅警告）或宿主未安装对应语言组件 → 用 `-Language zh-cn` 并确认 `WinPE_OCs` 里有该 cab。
12. 找不到 `FireWinPE.exe` / 第三方工具 → 默认探测目录与 `tools\` 白名单校验失败 → 先编译 `pe\` 工程，或显式传 `-ToolsSource`、`-VendorToolsDirectory`。
13. `install-adk.ps1` 返回 3010 后仍报检测不到 → 3010 表示需重启才完全生效 → 重启后重新运行脚本（已安装且非 `-Force` 时秒退）。
14. `-AssemblePayload` 缺 `boot.sdi` 抛错 → 取自 `<WorkDirectory>\media\Boot\boot.sdi`（注意是 `Boot` 不是 `boot`） → 确认 copype 产物完整，不要手工改名目录。

## 验证清单

- [ ] `Get-ChildItem payload\pe` 看到 `boot.wim`、`boot.sdi`、`Tools\{FireWinPE.exe, FireBoot.exe, wimlib-imagex.exe, libwim-15.dll, 7za.exe}`。
- [ ] `dism /Mount-Image` 能重新挂载新 boot.wim，且 `\Tools\Apps\` 下工具子目录结构完整。
- [ ] 挂载后的 `Windows\System32\startnet.cmd` 含 `wpeinit`、SxS `reg add` 块与 `if exist X:\Tools\FireWinPE.exe`。
- [ ] boot.wim 大小在日志中低于 1200 MB（日志打印"上限 1200 MB"）。
- [ ] 虚拟机从新 PE 启动后自动进入图形启动器，工具列表可点击，无 14001 弹窗。
- [ ] `-Language zh-cn` 构建后中文菜单/按钮正常显示，不是方块。
- [ ] 构建过程中断后无残留挂载（`dism /Get-MountedWimInfo` 为空）。
- [ ] `install-adk.ps1` 在已装 ADK + 附加组件的机器上 `exit 0` 且不重复安装。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| "未找到 ADK（KitsRoot10 不存在）" | 注册表两处 `KitsRoot10`、ADK 是否装完并重启 | 运行 `install-adk.ps1`，必要时 `-Force` |
| "WinPE 可选组件目录不存在" | 是否只装了 ADK 主包 | 补装 WinPE 附加组件 |
| DISM 注入失败（依赖） | 包注入顺序 | 改回 WMI → NetFx → Scripting → PowerShell |
| robocopy 抛"复制常用工具失败" | 退出码是否 >= 8 | 清理 `pe\apps` 中锁定/超长路径文件后重试 |
| boot.wim 超 1200 MB | `pe\apps`、`pe\drivers` 体积 | 精简目录，或去掉 `WinPE-NetFx` |
| PE 工具 14001 | `startnet.cmd` 是否有 SxS 块、`pe\runtime` 是否补齐 | 重新构建并检查 WinSxS 程序集前缀匹配 |
| 桌面端提示内置 PE 缺失 | `payload\pe` 是否被完整复制到 exe 同级 | 用 `-AssemblePayload` 重新组装并在干净目录发布 |

## 相关 skill

- firewin-usb-boot：消费本 skill 产出的 `payload\pe\`；boot.wim 体积直接决定启动分区估算（1536 MB 下限 + 512 MB 对齐）。
- firewin-iso-builder：另一条 ISO 路线（自研写入器，不依赖 ADK），与本 skill 的 MakeWinPEMedia/UDF 路线互补。
- firewin-overview：仓库总览与各 skill 的分工关系，先读它再定位到具体流程。
