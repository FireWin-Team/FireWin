---
name: firewin-build-release
description: Build FireWinPE desktop app, rebuild PE payload/ISO, and package dist deliverables. Use when asked to compile, publish, inject drivers, rebuild ISO, or sync dist\desktop. Do not use for unrelated C# features.
---

# FireWin 构建与发布

项目根目录 `e:\openwininstall`。桌面端为 WPF 单文件程序（`src\FireWin`），PE 端为 C++ 程序（`pe`），大体积资源（boot.wim / ISO / 安装包）不进 Git。

## 产物与来源（最容易出错的地方）

| 产物 | 位置 | 来源 |
|------|------|------|
| 单文件主程序 | `src\FireWin\bin\Release\net8.0-windows\win-x64\publish\FireWin.exe` | `dotnet publish` |
| 内置 PE 资源 | `payload\pe\` | 仓库 `payload`（权威源） |
| ISO | `dist\FireWinPE.iso` | `isobuild.dll`，读取 **app 目录下的 payload** |
| 分发包 | `dist\desktop\` | publish 产物 + 仓库 `payload` |

同步链：`payload\`（权威）→ `src\FireWin\bin\...\win-x64\payload\` → `...\publish\payload\` → `dist\desktop\payload\`。
改过 `payload` 后必须逐级同步，否则 ISO 和分发包会用旧 `boot.wim`（曾出现 ISO 仍是旧镜像的问题）。

## 构建主程序

```powershell
& 'e:\openwininstall\.buildtools\dotnet\dotnet.exe' publish 'e:\openwininstall\src\FireWin\FireWin.csproj' -c Release -r win-x64 -p:SelfContained=true -p:PublishSingleFile=true -v m
```

- 报 `CS2012 无法打开 obj\...\FireWin.dll`：多半是有 FireWin 进程占用，`Get-Process FireWin` 确认后关闭再重试。
- 完成后核对产物约 66.7 MB。

## 重建 ISO

```powershell
& 'e:\openwininstall\.buildtools\dotnet\dotnet.exe' 'e:\openwininstall\src\FireWin\bin\Release\net8.0-windows\win-x64\isobuild.dll' 'e:\openwininstall\dist\FireWinPE.iso'
```

构建前确认 `src\FireWin\bin\Release\net8.0-windows\win-x64\payload\pe\boot.wim` 与仓库 `payload\pe\boot.wim` 大小、时间一致。ISO 应为 UEFI + Legacy BIOS 双引导。

## 驱动提取与注入

- 提取常用驱动：`powershell -File scripts\extract-drivers.ps1`，输出到 `pe\drivers\<类别>\<驱动包>`（当前约 150 包 / 46 MB）。排除 Wi-Fi 与 `c_` 类扩展 INF。
- 注入 boot.wim：`dism /Cleanup-Wim` → 复制 boot.wim 到独立工作副本 → `/Mount-Image /Index:1` → `/Add-Driver /Recurse` → `/Unmount-Image /Commit` → 替换原镜像；先备份到 `.tmp\wim-inject\`。
- 未签名的 boot-critical 驱动（如 `hidlamparray`）会让整批 `/Add-Driver` 报错误 50 并卡死；必须在注入集中排除，不要靠逐个重试。
- 不注入显卡驱动。注入后 boot.wim 不得超过 1.2 GB（当前约 1065 MB）。

## 打包到 dist\desktop

1. `publish\FireWin.exe` 复制到 `dist\desktop\FireWin.exe`（被占用时提示用户关闭旧窗口）。
2. 用 `robocopy /MIR` 以 **仓库 `payload\`** 为源同步 `dist\desktop\payload\`。不要拿 publish 里的 payload 做 `/MIR` 源——它缺少 `Tools\Apps` 下的部分目录，会把 dist 里已有的应用删掉（AOMEI、SystemInformer\amd64、Notepad++\plugins\doc、Q-Dir\Favoriten 曾被误删）。
3. 校验：`Get-FileHash` 比对 publish 与 dist 的 exe；比对两侧 `boot.wim` 的大小与时间戳。

## 脚本约束

`scripts\*.ps1` 必须以 UTF-8 **带 BOM** 保存：

```powershell
[IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($true))
```

ADK 不完整时 `scripts\build-minipe.ps1` 不可用，改用系统 `dism` 加自研 ISO 构建。

## U 盘制作 / 升级要点

- 双分区布局：FAT32 启动分区（卷标 `FireWinPE`，写完移除盘符隐藏）+ exFAT 数据分区（`FireData`，含 `\FWIMG`）。
- 升级只替换启动分区内的 PE 文件；隐藏分区需用 `diskpart list volume` 找卷号 → `assign letter`，结束后 `remove letter`。
- `bcdedit /createstore` 不能覆盖已存在的 BCD：先写 `<bcd>.new`，全部配置成功后再 `File.Move(..., overwrite: true)` 替换；两份 store 分别是 `\boot\bcd`（BIOS，winload.exe）与 `\EFI\Microsoft\Boot\BCD`（UEFI，winload.efi）。

## 推送到 GitHub

仓库 `FireWin-Team/FireWin`（原 `Almmor/FireWin`，已转移并自动重定向），主分支 `main`。`gh` CLI 未安装；`github` 插件的 token 没有建仓/转移权限，但可用于 git 推送：

```powershell
git -c credential.helper='!f() { echo username=x-access-token; echo password=$GITHUB_TOKEN; }; f' push -u origin main
```

`.gitignore` 已排除 `payload/`、`pe/apps/`、`pe/drivers/`、`dist/`、`bin/`、`obj/`、`.tmp/`、`*.wim`、`*.iso`、`*.sdi`，保持这些大文件不入库。
