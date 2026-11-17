---
name: firewin-cli-automation
description: CLI and headless automation for FireWin covering desktop switches, exit codes, demo image mode, PE autodeploy and deploy.json keys. Use for scripted verification, not for UI styling.
---
# FireWin 命令行与无人值守自动化

桌面端与 PE 端都提供无界面入口，用于构建后自检、自动化截图和无人值守部署。两端的退出码含义并不一致，脚本必须按端分别判断。

## 适用范围

- 构建后自动验证 wimlib、ADK、版本号是否正常。
- 用 `--demo-image` 做界面自动化与截图。
- 手工构造 `deploy.json` 走 PE 端 `/autodeploy` 流程。
- 排查“脚本拿不到输出”“退出码与预期不符”“无人值守仍弹窗卡住”等问题。

## 不适用范围

- 窗口、页面与控件样式，转到 firewin-desktop-ui 与 firewin-wpf-styling。
- `bcdedit` 序列与临时引导写入实现，转到 firewin-bcd-boot。
- 构建、payload 组装与发布命令，转到 firewin-build-release。

## 关键文件与符号

| 路径 | 作用 | 关键符号 |
|---|---|---|
| `src/FireWin/App.xaml.cs` | 桌面端入口分发与 CLI 实现 | `OnStartup`、`RunCli`、`NativeConsole`、`ConsoleProgress` |
| `src/FireWin/MainWindow.xaml.cs` | 演示入口 | `DemoSelectImage(string path)` |
| `src/FireWin/FireWin.csproj` | 决定无控制台行为 | `<OutputType>WinExe</OutputType>`、`<Version>1.0.0</Version>` |
| `pe/main.cpp` | PE 端参数解析 | `wmain`、`LocateConfig`、`LoadConfig`、`DeployConfig`、`Deploy`、`ShowResult` |
| `pe/boottool.cpp` | FireBoot 独立引导工具 | `WritePeBoot`、`RemovePeBoot`、`RepairBoot` |

## 桌面端 CLI

入口判定在 `App.OnStartup`：先看 `--demo-image`，再看是否有任意参数，最后才开图形界面。

| 命令 | 行为 | 退出码 |
|---|---|---|
| `FireWin.exe` | 打开 `LauncherWindow`（二选一页） | 进程常驻 |
| `FireWin.exe --demo-image <镜像>` | 打开 `MainWindow`，直接进入镜像页并识别该镜像 | 进程常驻 |
| `FireWin.exe --wim-init-check` | 初始化 libwim，输出原生库路径 | 0 成功 / 1 异常 |
| `FireWin.exe --adk-status` | 输出 ADK 主包、WinPE 附加组件、`KitsRoot10` | 0 就绪 / 2 未就绪 / 1 异常 |
| `FireWin.exe --adk-install` | 静默安装 ADK 主包 + WinPE 附加组件（需管理员） | 0 成功 / 1 异常 |
| `FireWin.exe --version` | 输出 `FireWin 1.0.0 (GPL-3.0-only)` | 0 |
| 任意未知参数 | 打印用法说明 | 0 |

要点：

- `--demo-image` 必须出现在第 1 个参数位，且总参数数至少 2；否则落到 `RunCli` 打印用法。
- 除 `--demo-image` 外的任何参数都会进入 CLI 分支，不会弹出图形界面。
- 所有命令异常统一写 `Console.Error`，格式 `[错误] <消息>`，返回 1。
- 输出被重定向时不再 `AttachConsole`，标准句柄已可用；只有交互式终端才附加父控制台。
- `--adk-status` 的 0/2 是“状态”不是“成功/失败”，脚本要显式区分。

### PowerShell 调用模板

```powershell
$exe = "dist\desktop\FireWin.exe"

# 捕获标准输出与标准错误（WinExe 必须重定向才能拿到文本）
$version = & $exe --version 2>&1
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^FireWin 1\.0\.0') {
    throw "版本校验失败：$version"
}

& $exe --wim-init-check 2>&1 | Write-Host
if ($LASTEXITCODE -ne 0) { throw "wimlib 初始化失败" }

& $exe --adk-status 2>&1 | Write-Host
switch ($LASTEXITCODE) {
    0 { Write-Host "ADK 就绪" }
    2 { Write-Host "ADK 未就绪（需安装主包或 WinPE 附加组件）" }
    default { throw "ADK 检测异常" }
}
```

## PE 端 CLI

`pe/main.cpp` 的 `wmain` 按顺序解析 `/autodeploy`、`/shell`、`/config:<路径>`、`/?|/help|-h`，短横线写法等价。

| 命令 | 行为 | 退出码 |
|---|---|---|
| `FireWinPE.exe` | 进入 PE 桌面（壁纸 + 图标 + 任务栏） | 常驻 |
| `FireWinPE.exe /shell` | 打开经典单窗口启动器 | 常驻 |
| `FireWinPE.exe /autodeploy` | 按自动定位到的 `deploy.json` 部署 | 见下表 |
| `FireWinPE.exe /config:<路径>` | 用指定配置部署（不写 `/autodeploy` 也会部署） | 见下表 |
| `FireWinPE.exe /?`（或 `/help`、`-h`） | 弹 MessageBox 显示用法 | 0 |

部署路径的退出码：

| 退出码 | 含义 |
|---|---|
| 0 | `Deploy` 成功 |
| 1 | 部署失败（`DeploymentResult.success == false`） |
| 2 | 未找到 `deploy.json` |
| 3 | `deploy.json` 读取或解析失败 |

`deploy.json` 定位顺序（`LocateConfig`）：

1. 显式 `/config:<路径>`，文件不存在则直接失败，不再回退。
2. `<FireWinPE.exe 所在目录>\deploy.json`。
3. `C:\FireWinPE\deploy.json`。
4. 依次扫描 `C:` 到 `Z:` 的 `<盘符>:\FireWinPE\deploy.json`。

## deploy.json 契约

桌面端由 `BcdBootService` 以 camelCase、`WriteIndented`、UTF-8 无 BOM 写出，PE 端用扁平 JSON 解析器读取。

| 键 | 类型 | 说明 |
|---|---|---|
| `wimPath` | string | 镜像原始路径 |
| `wimVolumePath` | string? | 卷 GUID 路径，PE 端优先使用（盘符会漂移） |
| `wimIndex` | int | 镜像索引，`<=0` 时回落为 1 |
| `targetDrive` | string | 目标系统分区盘符 |
| `targetVolume` | string? | 目标系统分区卷 GUID 路径 |
| `systemDrive` | string | 引导分区盘符 |
| `systemVolume` | string? | 引导分区卷 GUID 路径 |
| `firmware` | string | `UEFI` 或 `BIOS`，缺省 `UEFI` |
| `bootEntryGuid` | string | 临时 BCD 项 GUID，部署完成后用于清理 |
| `logPath` | string | PE 端日志路径，缺省 `<exe 目录>\deploy.log` |
| `autoReboot` | bool | 成功后是否 `shutdown /r /t 15`，缺省 true |

布尔值接受 `true`、`1`、`yes`（大小写不敏感）。

## 核心流程

1. 桌面端发布后先跑三条只读命令：`--version`、`--wim-init-check`、`--adk-status`，全部通过再做介质制作。
2. 需要界面证据时用 `--demo-image <ISO或WIM>`，进程会停在识别结果页，适合截图与人工核对。
3. 需要验证无人值守时，手工准备 `deploy.json`，放到目标机的 `C:\FireWinPE\`，在 PE 里执行 `FireWinPE.exe /autodeploy`。
4. 读取 `deploy.log` 与 `%TEMP%\FireWinPE-install.log` 判断失败阶段（见 firewin-troubleshooting）。
5. 脚本必须同时检查退出码和关键输出文本：未知参数也返回 0，单看退出码会漏掉拼写错误。

## 约束与坑

1. 现象：`& FireWin.exe --version` 什么都抓不到。原因：`OutputType=WinExe` 默认没有控制台，未重定向时输出进了附加的控制台。正确做法：用 `2>&1` 或 `|` 重定向捕获，脚本里不要依赖“直接打印”。
2. 现象：输出重定向后反而全丢。原因：进程在重定向场景仍调用 `AttachConsole`。正确做法：保持 `Console.IsOutputRedirected` 判断，重定向时不要附加控制台。
3. 现象：参数拼错脚本仍判定成功。原因：未知参数走 `default` 分支打印用法并返回 0。正确做法：脚本同时校验输出首行或关键字。
4. 现象：传了 `--demo-image` 却没有界面。原因：参数数不足 2，或它不在第 1 位。正确做法：`FireWin.exe --demo-image "D:\win.iso"`，路径含空格要加引号。
5. 现象：想开 GUI 却进了 CLI。原因：只要带了任意参数就走 `RunCli`。正确做法：验证完成后不带参数启动。
6. 现象：`--adk-install` 失败。原因：`app.manifest` 要求 `requireAdministrator`，未提升时 UAC 被拒或 `winget` 不可用。正确做法：管理员终端执行，并先跑 `--adk-status` 确认缺口。
7. 现象：PE 里只想看帮助却开始部署。原因：`/?` 等帮助参数在部署分支之前处理，但 `/config:` 单独出现就会部署。正确做法：只想看帮助就只传 `/help`。
8. 现象：`/autodeploy` 部署了错误的目标。原因：未传 `/config:` 时会按默认顺序全局搜索 `deploy.json`，可能命中旧文件。正确做法：无人值守务必显式 `/config:<绝对路径>`。
9. 现象：`deploy.json` 解析失败退出 3。原因：写成了嵌套对象，或带 BOM。正确做法：保持扁平键值，UTF-8 无 BOM；桌面端写出的文件已满足。
10. 现象：部署成功后立刻重启，来不及收集日志。原因：`autoReboot` 缺省 true。正确做法：调试期在配置里写 `"autoReboot": false`。
11. 现象：无人值守仍被弹窗卡住。原因：失败路径用 `MessageBoxW` 报错，`ShowResult` 也会弹窗。正确做法：无人值守场景安排人工值守或改用日志判定，不要假设完全静默。
12. 现象：`/shell` 与无参数行为混淆。原因：`/shell` 在 `argc<=1` 判断之前提前 `return RunLauncher()`。正确做法：记住 `/shell` 是桌面异常时的兜底，不会读取 `deploy.json`。

## 验证清单

- [ ] `FireWin.exe --version` 输出包含 `FireWin 1.0.0 (GPL-3.0-only)`，退出码 0。
- [ ] `FireWin.exe --wim-init-check` 输出 `[OK] wimlib 已初始化` 与真实 DLL 路径。
- [ ] `FireWin.exe --adk-status` 退出码为 0 或 2，且与三行状态文本一致。
- [ ] `FireWin.exe --demo-image <镜像>` 能进入镜像页并显示识别结果。
- [ ] 未知参数输出用法文本且退出码为 0（脚本需额外文本校验）。
- [ ] `FireWinPE.exe /?` 弹窗显示用法且返回 0。
- [ ] `FireWinPE.exe /config:<路径>` 能定位配置并进入部署。
- [ ] `deploy.json` 为扁平 camelCase、UTF-8 无 BOM，键名与契约表一致。
- [ ] `deploy.log` 记录“配置文件 / 工具目录 / 自动模式”三行起始信息。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| 脚本拿不到输出 | 是否重定向、是否 WinExe | 加 `2>&1` 捕获 |
| 退出码 0 但功能没执行 | 是否未知参数走 default | 校验输出文本 |
| 进入 CLI 而非界面 | 启动参数是否为空 | 去掉参数重新启动 |
| ADK 状态 2 | `--adk-status` 三行文本 | 补装主包或 WinPE 附加组件 |
| PE 退出码 2 | `deploy.json` 是否存在、搜索路径 | 显式 `/config:` |
| PE 退出码 3 | JSON 是否扁平、是否 BOM | 用桌面端生成或重写配置 |
| 无人值守卡住 | 是否有 MessageBox | 改用日志判定，安排值守 |
| 部署后旧系统进了 PE | `bootEntryGuid` 与 `autoReboot` | 检查配置与 BCD 清理 |

## 相关 skill

- `firewin-bcd-boot`：`deploy.json` 的写入方与临时引导生命周期。
- `firewin-pe-runtime`：PE 端部署主流程、日志位置与自动重启细节。
- `firewin-troubleshooting`：按退出码与日志关键字定位失败阶段。
- `firewin-wpf-styling`：`--demo-image` 截图涉及页面显示时参考。
