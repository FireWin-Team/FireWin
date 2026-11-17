---
name: firewin-build-release
description: Build FireWin PE executables with CMake, mini PE boot.wim and ISO, the single-file desktop exe and payload sync; use for compile or release work, not runtime troubleshooting.
---

# FireWin 构建与发布

从源码到 `dist\` 可分发产物的完整链路：PE 端原生 exe、迷你 PE 镜像、桌面端单文件与 payload 同步。

## 适用范围

- 首次搭建构建环境（Windows ADK、VS 2022 x64、CMake、便携 .NET SDK）
- 重新编译 `pe\` 工程，产出 `FireWinPE.exe` 与 `FireBoot.exe`
- 重新生成 `boot.wim` / `FireWinPE.iso` 并组装 `payload\pe\`
- 增量更新 `pe\apps`、`pe\drivers`、`pe\runtime` 后重建 PE 镜像
- 发布桌面端 `dist\desktop\FireWin.exe`，并核对四段同步链
- 按大小与哈希校验构建产物是否真的更新

## 不适用范围

- 运行期故障定位（0xc000000f、14001、盘符漂移、BCD 残留等）见 `firewin-troubleshooting`
- 业务逻辑与界面开发；本 skill 只覆盖构建、打包与发布
- 目标机器上的 .NET / VC 运行库安装；项目产物是自包含、静态链接的

## 关键文件与符号

| 文件/目录 | 作用与关键符号 |
|---|---|
| `src/FireWin/FireWin.csproj` | `net8.0-windows`、`Platforms/PlatformTarget/RuntimeIdentifier=win-x64`、`SelfContained`、`PublishSingleFile`、`IncludeNativeLibrariesForSelfExtract`、`EnableCompressionInSingleFile` |
| `src/FireWin/FireWin.csproj` | `Content Include="..\..\payload\**\*"` + `Link` + `ExcludeFromSingleFile`；`Target CopyBundledPayloadToPublish`（`AfterTargets="Publish"`） |
| `src/FireWin/app.manifest` | `requireAdministrator`，启动即提权（写引导需要 bcdedit） |
| `pe/CMakeLists.txt` | 仅 MSVC、仅 x64、`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`（/MT）；目标 `FireWinPE`（CONSOLE）与 `FireBoot`（WINDOWS）输出到 `${CMAKE_BINARY_DIR}/bin` |
| `pe/main.cpp` | PE 桌面/安装/驱动/网络入口，约 266 KB |
| `pe/boottool.cpp` | `FireBoot.exe` 独立引导工具 |
| `scripts/install-adk.ps1` | 参数 `-InstallPath`、`-AdkSetupPath`、`-WinPeSetupPath`、`-Force` |
| `scripts/build-minipe.ps1` | 参数 `-Architecture amd64`、`-WorkDirectory`、`-Language`、`-ToolsSource`、`-VendorToolsDirectory`、`-AppsDirectory`、`-DriversDirectory`、`-RuntimeDirectory`、`-OutputIso`、`-AssemblePayload`、`-PayloadOutputDirectory` |
| `scripts/download-pe-apps.ps1` | 参数 `-AppsDirectory`、`-Force`、`-Only`、`-TimeoutSeconds 600`；只收录 64 位工具 |
| `scripts/extract-drivers.ps1` | 参数 `-Include`、`-OnlyInf`、`-SkipInf`、`-MaxPackageMB 20`、`-MaxTotalMB 120`、`-Clean`、`-Force` |
| `payload/pe/` | 唯一随桌面端分发的 PE 资源：`boot.wim`、`boot.sdi`、`Tools\` |
| `tools/` | `wimlib-imagex.exe`、`libwim-15.dll`、`7za.exe` 及许可证文本 |
| `.buildtools/dotnet/dotnet.exe` | 便携 .NET SDK 入口（约 157 KB shim） |
| `.buildtools/cmake/cmake-4.5.0-windows-x86_64/bin/cmake.exe` | 便携 CMake 4.5.0 |
| `downloads/vs_buildtools.exe` | VS 2022 生成工具引导器（约 4.4 MB） |
| `downloads/nuget/managedwimlib.2.6.0.nupkg` | ManagedWimLib 离线 NuGet 包 |

## 核心流程

1. 环境自检（管理员 PowerShell，仓库根目录）。

```powershell
Test-Path 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
& .\.buildtools\dotnet\dotnet.exe --info
& .\.buildtools\cmake\cmake-4.5.0-windows-x86_64\bin\cmake.exe --version
```

判断：注册表项不存在 → 先装 ADK；`dotnet --info` 失败 → 用便携 SDK 或安装 .NET 8 SDK；CMake 版本低于 3.20 不满足 `CMakeLists.txt` 要求。

2. 安装 ADK 主包与 WinPE 附加组件（两者是独立安装程序，都要装）。

```powershell
.\scripts\install-adk.ps1
# 离线/指定安装包
.\scripts\install-adk.ps1 -AdkSetupPath .\downloads\adk\adksetup.exe -WinPeSetupPath .\downloads\adk\adkwinpesetup.exe
```

脚本优先 `winget install Microsoft.WindowsADK` / `Microsoft.WindowsADK.WinPEAddon`，失败回退 `adksetup.exe /quiet /norestart /ceip off /features OptionId.DeploymentTools` 与 `adkwinpesetup.exe /quiet /norestart`；安装程序退出码 `3010` 视为成功但要求重启。

判断：`KitsRoot10`、`...\Assessment and Deployment Kit\Deployment Tools`、`...\Windows Preinstallation Environment` 三者同时存在才算就绪。

3. 安装 MSVC x64 工具链（若 `cmake` 配置时报找不到编译器）。

```powershell
.\downloads\vs_buildtools.exe
```

勾选「使用 C++ 的桌面开发」，必须包含 x64 生成工具；ARM/32 位工具集不能满足 `-A x64`。

4. 构建 PE 端（失败会直接提示 MSVC/x64 约束）。

```powershell
$cmake = '.\.buildtools\cmake\cmake-4.5.0-windows-x86_64\bin\cmake.exe'
& $cmake -S pe -B pe\build -G "Visual Studio 17 2022" -A x64
& $cmake --build pe\build --config Release
```

产物固定落在 `pe\build\bin\FireWinPE.exe`、`pe\build\bin\FireBoot.exe`（多配置生成器的 `Release/` 也被重定向到 `bin`）。

判断：报「PE 端只支持 x64」→ 补 `-A x64`；报「必须使用 MSVC」→ 生成器不能用 MinGW/Ninja+clang。

5. 校验静态 CRT。

```powershell
dumpbin /dependents .\pe\build\bin\FireWinPE.exe
```

输出不得出现 `VCRUNTIME140.dll`、`MSVCP140.dll`；链接库只应有 `comdlg32`、`msimg32`、`shell32` 及系统基础 DLL。

6. 准备 PE 内工具与驱动（可跳过，但会失去对应能力）。

```powershell
.\scripts\download-pe-apps.ps1                 # 已存在且含 exe 的目录 SKIP
.\scripts\download-pe-apps.ps1 -Only Chrome,RustDesk -Force
.\scripts\extract-drivers.ps1                  # 默认输出 pe\drivers，20MB/包、120MB 总量
```

判断：`pe\apps` 为空只会让启动器少按钮；`pe\drivers` 为空时 `build-minipe.ps1` 只告警并跳过注入。

7. 构建 boot.wim、ISO 并组装 payload（必须管理员）。

```powershell
.\scripts\build-minipe.ps1 -Language zh-cn -OutputIso .\dist\FireWinPE.iso -AssemblePayload
# 显式给齐源目录
.\scripts\build-minipe.ps1 -ToolsSource .\pe\build\bin -VendorToolsDirectory .\tools `
  -AppsDirectory .\pe\apps -DriversDirectory .\pe\drivers -RuntimeDirectory .\pe\runtime `
  -OutputIso .\dist\FireWinPE.iso -AssemblePayload -PayloadOutputDirectory .\payload\pe
```

判断：脚本先 `copype amd64`（自行补齐 `WinPERoot`/`OSCDImgRoot`/`DISMRoot` 等环境变量），挂载后按 WMI→NetFx→Scripting→PowerShell 注入，`-Language zh-cn` 额外注入 `WinPE-FontSupport-ZH-CN`；卸载 Commit 后若 `boot.wim` 超过 1200 MB 直接抛错；只有带 `-AssemblePayload` 才写 `payload\pe`。

8. 发布桌面端单文件。

```powershell
& .\.buildtools\dotnet\dotnet.exe publish src\FireWin\FireWin.csproj -c Release -r win-x64 `
  --self-contained true -p:PublishSingleFile=true -o .\dist\desktop
```

判断：`dist\desktop\FireWin.exe` 为单文件，`payload\pe` 只能位于 exe 同级目录；csproj 已声明单文件/自解压/压缩，命令行重复传参无害但不要改成 `--self-contained false`。

9. 四段同步链，顺序不可颠倒。

```text
pe\build\bin + tools + pe\apps + pe\drivers + pe\runtime
  → payload\pe            （build-minipe.ps1 -AssemblePayload：boot.wim + boot.sdi + Tools\5 个文件）
  → 发布目录 payload\pe    （CopyBundledPayloadToPublish，AfterTargets=Publish；Content 的 Link 控制输出）
  → dist\desktop\payload\pe（dotnet publish -o 指定的目录）
```

判断：只重建了 `pe\` 而未重跑第 7 步 → `payload\pe\boot.wim` 仍是旧 PE；只更新了 payload 而未重发第 8 步 → `dist\desktop\payload` 仍是旧副本。

10. 校验产物与哈希。

```powershell
Get-Item .\payload\pe\boot.wim, .\payload\pe\boot.sdi, .\dist\desktop\FireWin.exe | Select-Object Name,Length
Get-FileHash .\payload\pe\boot.wim, .\dist\desktop\payload\pe\boot.wim -Algorithm SHA256
```

判断：两个 `boot.wim` 哈希必须一致，否则同步链断在第 8 步。

## 产物与校验表

| 产物 | 路径 | 参考大小 | 校验方式 |
|---|---|---|---|
| PE 主程序 | `pe\build\bin\FireWinPE.exe` | 3803648 B | `dumpbin /dependents` 无 VC 运行库 |
| 引导工具 | `pe\build\bin\FireBoot.exe` | 688640 B | 同上 |
| PE 镜像 | `payload\pe\boot.wim` | 1116400802 B | 不超过 1200 MB；与发布副本哈希一致 |
| ramdisk 引导盘 | `payload\pe\boot.sdi` | 3170304 B | 与 copype 的 `media\Boot\boot.sdi` 一致 |
| PE 工具集 | `payload\pe\Tools\` | `FireWinPE.exe` 3803648 B、`FireBoot.exe` 688640 B、`wimlib-imagex.exe` 137230 B、`libwim-15.dll` 483854 B、`7za.exe` 1334784 B | 五个文件齐全，且无旧 `Apps\` 目录残留 |
| 启动 ISO | `dist\FireWinPE.iso` | 1127948288 B（随工具/语言变化） | 虚拟机 UEFI 与 BIOS 各启动一次 |
| 桌面端 | `dist\desktop\FireWin.exe` | 66767646 B（约 66.8 MB） | 单文件自包含，`--version` 正常 |
| 发布 payload | `dist\desktop\payload\pe\` | 目录 | `boot.wim`/`boot.sdi`/`Tools\` 齐全 |

## 约束与坑

1. 发布报 `CS2012`「无法打开 … 进行写入，文件正由另一进程使用」 → 上一轮 `FireWin.exe` 仍在运行或 VS 宿主/杀软占用 `obj`；先 `Stop-Process -Name FireWin -Force`，再删 `src\FireWin\bin`、`src\FireWin\obj` 后重发。
2. `libwim` 只有 x64 → 项目固定 `win-x64` + `PlatformTarget=x64`；改成 AnyCPU/x86 会 `BadImageFormatException` 或 `DllNotFoundException`，不是代码问题。
3. payload 不能塞进单文件 → `Content` 必须带 `ExcludeFromSingleFile=true`，由 `CopyBundledPayloadToPublish`（`AfterTargets="Publish"`）把 `payload\**` 复制到发布目录；删掉该 Target 后 exe 能生成，但欢迎页提示「缺 PE 资源」。
4. payload 缺失时构建静默通过 → csproj 的 `Content` 与 Target 都带 `Exists` 条件；构建前先 `Test-Path .\payload\pe\boot.wim`，否则发布出一个无法制作启动盘的 exe。
5. `boot.wim` 超过 1200 MB → 整个镜像载入内存盘，2 GB 内存机器直接启动失败；精简 `pe\apps`、`pe\drivers`，或去掉 `WinPE-NetFx`/`WinPE-PowerShell`，而不是加大分区。
6. 注入顺序 WMI→NetFx→Scripting→PowerShell → PowerShell 依赖 Scripting，Scripting/NetFx 依赖 WMI；乱序调用 DISM 会报依赖缺失并中断。
7. `robocopy /S` 只增不删 → `build-minipe.ps1` 同步 `pe\apps` 时旧目录会残留进 `Tools\Apps`；升级工具版本前先删 `pe\apps` 内旧目录，再重建并确认 `payload\pe\Tools\Apps` 无同名残留。
8. 中文界面必须 `-Language zh-cn` → 只有该参数会注入 `WinPE-FontSupport-ZH-CN`；用默认 `en-us` 构建出来的中文 UI 全是方块，属构建参数错误。
9. 驱动目录默认排除 `c_*` → `extract-drivers.ps1` 的 `-SkipInf` 默认匹配 `*:^c_`，手工把 `c_*.inf` 扔进 `pe\drivers` 会让整批 `/Add-Driver` 失败。
10. 便携工具不要依赖 PATH → CMake 在 `.buildtools\cmake\cmake-4.5.0-windows-x86_64\bin\cmake.exe`，.NET 在 `.buildtools\dotnet\dotnet.exe`；`cmake`/`dotnet` 直接敲可能命中系统旧版本。
11. 多配置生成器的输出目录被重定向 → 不要去找 `pe\build\Release\FireWinPE.exe`，CMakeLists 已把运行时输出固定为 `pe\build\bin`。
12. ISO 上限 4 GB → 单文件超过 4 GB 时 `build-minipe.ps1` 会抛错；大工具应留在 `pe\apps`（进 boot.wim）或改走 U 盘数据分区，而不是塞进 ISO。

## 验证清单

- [ ] `.\dist\desktop\FireWin.exe --version` 输出版本号，无缺 DLL 弹窗
- [ ] `.\dist\desktop\FireWin.exe --wim-init-check` 输出 wimlib 初始化成功
- [ ] `dumpbin /dependents` 检查 `FireWinPE.exe`、`FireBoot.exe` 无 `VCRUNTIME140.dll` / `MSVCP140.dll`
- [ ] `payload\pe\boot.wim` 存在且不超过 1200 MB，`boot.sdi` 为 3170304 B
- [ ] `payload\pe\Tools\` 五个文件齐全，版本与 `pe\build\bin` 一致
- [ ] `dist\desktop\payload\pe\boot.wim` 与 `payload\pe\boot.wim` SHA256 相同
- [ ] `dist\FireWinPE.iso` 在虚拟机中 UEFI、BIOS 各能进 PE
- [ ] 桌面端制作 U 盘后，`sources\boot.wim` 与 payload 哈希一致

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| `dotnet publish` 报 CS2012 | `Get-Process FireWin` | 结束进程，删 `bin`/`obj` 后重发 |
| `cmake -S pe -B pe\build` 无编译器 | VS 2022 x64 生成工具是否安装 | 运行 `downloads\vs_buildtools.exe` 补装 C++ x64 |
| 报「PE 端只支持 x64」 | 传给 CMake 的 `-A` | 补 `-A x64`，不要改源码 |
| `build-minipe.ps1` 找不到 `copype`/`oscdimg` | ADK 与 WinPE 附加组件是否都装 | 重跑 `scripts\install-adk.ps1`，注意 3010 需重启 |
| `boot.wim` 超 1200 MB | `pe\apps`、`pe\drivers` 体积 | 删旧工具/驱动目录，重建镜像 |
| 注入驱动整批失败 | `pe\drivers` 中是否有 `c_*.inf` | 删除 `c_*` 目录，用 `extract-drivers.ps1 -Clean` 重取 |
| 发布目录没有 `payload` | csproj 的 `Content`/`CopyBundledPayloadToPublish` | 恢复 `ExcludeFromSingleFile` 与 Publish 后复制 Target |
| U 盘里的 PE 是旧版 | 两处 `boot.wim` 哈希 | 依次重跑第 7、8 步，不能只跑一步 |

## 相关 skill

- `firewin-pe-build`、`firewin-iso-builder`、`firewin-troubleshooting`：PE 源码与 CMake 细节、ISO 写入器实现、运行期故障定位
- `firewin-cli-automation`：发布后必须执行的 `--version`、`--wim-init-check`、`--adk-status` 三条自检

