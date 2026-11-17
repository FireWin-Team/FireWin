---
name: firewin-driver-injection
description: Extract base drivers from the local DriverStore and inject them into FireWin boot.wim with DISM. Use when WinPE lacks network, storage or touchpad drivers. Not for runtime drvload or app packaging.
---

# 驱动提取与 boot.wim 注入

extract-drivers.ps1 把本机 DriverStore 里的基础驱动复制到 `pe\drivers`，build-minipe.ps1 再用 DISM 递归注入 boot.wim；PE 启动后还会用 drvload 补载外置驱动。

## 适用范围

- 笔记本在 PE 里看不到硬盘、网卡或触控板，需要补基础驱动。
- 调整 `-Include`、`-OnlyInf`、`-SkipInf` 控制提取范围。
- 控制驱动体积，避免 boot.wim 超过 1200 MB。
- 排查 `dism /Add-Driver` 整批失败、缺文件、签名问题。
- 处理 DriverStore 同驱动多版本、类扩展 INF、非管理员提取。
- 核对 `pe\drivers\<Class>\<包名>` 结构与运行时 drvload 的二次加载路径。

## 不适用范围

- 提取显卡、蓝牙、无线网卡等默认排除的大包或 PE 无法使用的驱动。
- 制作启动盘、打包内置工具，见 firewin-usb-boot 与 firewin-pe-apps。

## 关键文件与符号

| 路径 | 作用 | 关键符号 |
|---|---|---|
| scripts\extract-drivers.ps1 | 从 DriverStore 筛选并复制驱动 | `Get-DismPackages`、`Get-DriverStorePackages`、`Get-InfClass`、`Get-MissingPackageFiles`、`Get-InfDriverVerDate` |
| scripts\extract-drivers.ps1 | 规则匹配与体积控制 | `ConvertTo-InfRules`、`Test-InfRules`、`Test-InfOnlyAllowed`、`$MaxPackageMB`、`$MaxTotalMB` |
| scripts\build-minipe.ps1 | 挂载后的 DISM 注入 | `Invoke-Dism`、`/Add-Driver`、`/Recurse`、`$DriversDirectory` |
| scripts\build-minipe.ps1 | 提交后体积门禁 | `$wimLimitMb = 1200`、`/Unmount-Image /Commit` |
| pe\drivers\说明.txt | 人工放置驱动的约定与推荐范围 | 目录约定、x64 要求、`\Drivers` 运行时可扫描 |
| pe\main.cpp | 开机自动加载 | `CollectInboxDriverInfs`、`CollectBootDriverInfs`、`drvload.exe` |
| pe\main.cpp | 手动「加载驱动…」 | `FindInfFilesOnVolumes`、`BrowseForInf`、`StartDriverLoader` |
| pe\drivers | 构建输入，按 INF Class 分目录 | `Net`、`USB`、`SCSIAdapter`、`HIDClass`、`Mouse`、`Keyboard`、`System` |

## 核心流程

1. 前置检查：`$env:WINDIR\System32\DriverStore\FileRepository` 必须存在，否则脚本直接 throw。
2. 判断管理员：是管理员先用 `Get-WindowsDriver -Online -All` 读 DISM 清单；导入 DISM 模块或读取失败时回退到直接遍历 DriverStore 各包目录。
3. 直接读取模式对每个包目录只取第一个 `*.inf`，用 `Get-InfClass` 读 `[Version]` 的 `Class`；读不到 Class 的包丢弃。
4. 过滤顺序固定：先按 `-Include`（转小写、`-contains` 比较）选类，再按 `-SkipInf` 排除，最后按 `-OnlyInf` 收窄。
5. `-OnlyInf` 的语义是「某类别只要出现过规则，该类就只收命中规则的包」；没写规则的类别全收。默认只给 `System` 写规则。
6. 去重键为 `Class|INF 文件名去扩展名`，同键按 `Get-InfDriverVerDate` 倒序取第一个；日期只解析 `MM/dd/yyyy`，解析失败为 `MinValue`。
7. 逐包处理：按 DriverStore 目录名 `Package` 去重，计算 `Get-FolderSize`，校验 INF 的 `[SourceDisksFiles]` 所列文件是否都能在包目录递归索引中找到。
8. 体积判断：单包超过 `-MaxPackageMB`（默认 20）跳过；累计超过 `-MaxTotalMB`（默认 120）跳过后续包。
9. 目标已存在且未指定 `-Force` 时不复制，但仍把源包体积累加到 `$totalBytes`，状态记为「已存在」。
10. 复制用 `Copy-Item <源>\* -Destination <目标> -Recurse -Force`，保留整个包目录，输出为 `pe\drivers\<Class>\<DriverStore 包名>`。
11. 构建时 `$driversSource` 默认为 `pe\drivers`，可用 `-DriversDirectory` 覆盖；递归统计 `*.inf`，数量大于 0 才注入。
12. 注入命令为 `dism /Add-Driver /Image:<挂载目录> /Driver:<driversSource> /Recurse`，刻意不加 `/ForceUnsigned`。
13. 注入发生在挂载阶段；`/Unmount-Image /Commit` 之后才检查 `boot.wim` 是否超过 1200 MB，超过直接 throw，成品已被提交。
14. PE 开机时先 drvload 内置 DriverStore 的存储与 HID 栈，再加载 `X:\Tools\Drivers`、各盘 `\Drivers`、各盘 `\FWIMG\Drivers`；手动「加载驱动…」只扫描各盘 `\Drivers`，无结果时弹文件选择框。

常用命令：

```powershell
# 默认提取到 pe\drivers
.\scripts\extract-drivers.ps1
# 只补存储与 USB，强制刷新
.\scripts\extract-drivers.ps1 -Include SCSIAdapter,USB -Force
# 输出到临时目录再人工挑选
.\scripts\extract-drivers.ps1 -OutputDirectory $env:TEMP\fwdrv -MaxTotalMB 80
# 构建时覆盖驱动源（默认 pe\drivers）
.\scripts\build-minipe.ps1 -AssemblePayload -DriversDirectory D:\drv
```

| 阶段 | 输入位置 | 谁读取 | 时机 |
|---|---|---|---|
| 构建注入 | `pe\drivers\<Class>\<包名>` | `dism /Add-Driver /Recurse` | 挂载 boot.wim 期间 |
| 内置栈 | `X:\Windows\System32\DriverStore\FileRepository` | `CollectInboxDriverInfs` | 每次 PE 启动 14% 起 |
| PE 同目录 | `X:\Tools\Drivers` | `CollectBootDriverInfs` | 每次 PE 启动 |
| 任意分区 | `<盘符>\Drivers` | `CollectBootDriverInfs` / 手动加载 | 启动或点按钮 |
| FWIMG 分区 | `<盘符>\FWIMG\Drivers` | `CollectBootDriverInfs` | 仅启动自动加载 |

## 参数或清单表

| 参数 | 默认值 | 作用 |
|---|---|---|
| `-OutputDirectory` | `pe\drivers` | 输出根目录，脚本按 Class 建子目录 |
| `-Include` | `Net,USB,SCSIAdapter,HIDClass,Mouse,Keyboard,System` | 需要提取的 INF Class，大小写不敏感 |
| `-OnlyInf` | `System:^(iagpio\|ialpss\|iai2c\|amdi2c\|amdgpio\|hidi2c\|hidspi\|hidinterrupt\|ishhid\|mshid)` | 类别内白名单正则，只对写了规则的类别生效 |
| `-SkipInf` | `*:^c_` 加 Net/HIDClass/USB 三组正则 | `*` 表示所有类别，命中即排除 |
| `-MaxPackageMB` | `20` | 单包上限，显卡、无线包通常直接超限 |
| `-MaxTotalMB` | `120` | 本次提取累计上限，含「已存在」包 |
| `-Clean` | 关 | 先删除整个输出目录，`说明.txt` 也会被删 |
| `-Force` | 关 | 已存在的目标包也重新复制 |

### 驱动筛选表

| Class | 默认收录 | 默认规则要点 | 用途 |
|---|---|---|---|
| `Net` | 是 | 排除 `netwtw`、`netwbw`、`netwlv`、`rtwlanu`、`netathr`、`bcmdhd`、`net8192`、`wintun`、`bthpan` 等无线/蓝牙/虚拟 WLAN | 有线、USB、虚拟网卡 |
| `USB` | 是 | 排除 `usbaapl64`、`qdbusb`、`qcfilter` | xHCI、USB 3.x 控制器 |
| `SCSIAdapter` | 是 | 无额外规则 | Intel RST、AMD RAID、NVMe 相关 |
| `HIDClass` | 是 | 排除 `bthoob`、`hidbth` | I2C HID、触摸屏 |
| `Mouse` | 是 | 无额外规则 | 精密触控板、鼠标过滤驱动 |
| `Keyboard` | 是 | 无额外规则 | 特殊键盘与快捷键 |
| `System` | 是 | 仅收 `iagpio`、`ialpss`、`iai2c`、`amdi2c`、`amdgpio`、`hidi2c`、`hidspi`、`hidinterrupt`、`ishhid`、`mshid` 前缀 INF | GPIO/I2C/SPI 触控板依赖链 |
| `Display` | 否 | 不在默认 Include | 需手动加入，单包可达 GB 级 |

## 约束与坑

1. 现象：提取后 PE 仍无 Wi-Fi。原因：默认 `SkipInf` 排除全部无线 WLAN 包，且 PE 没有 wlansvc，装了也无法连接。正确做法：改用有线或 USB 网卡；不要为 Wi-Fi 放开 `netwtw`，只会白增几十 MB。`pe\drivers\说明.txt` 里「网卡含 Wi-Fi」的推荐与脚本默认策略冲突，以脚本为准。
2. 现象：`dism /Add-Driver /Recurse` 一个包失败后整批中断。原因：`c_*` 类扩展 INF 在 PE 中缺对应 SYS，脚本默认用 `*:^c_` 排除；手工把整包复制进 `pe\drivers` 会绕过过滤。正确做法：不要把 `c_*` 目录直接放进构建输入，只保留运行时内置 DriverStore 的 `c_hidclass.inf`。
3. 现象：`pe\drivers` 只有几十 MB，但注入后启动仍缺触控板。原因：I2C 精密触控板需要 GPIO + I2C + HID 三件套，`-OnlyInf` 只放行 System 白名单，Mouse/Keyboard 全收但可能仍缺厂商 I2C 控制器。正确做法：在同型号正常机器上运行脚本，核对 System 下是否同时有 `iagpio`/`ialpss2i_i2c`/`hidi2c` 三类。
4. 现象：某包在 DriverStore 里明明完整，脚本却报「驱动包不完整」，或反之 DISM 报缺文件。原因：`Get-MissingPackageFiles` 只解析 `[SourceDisksFiles]`，且按文件名叶子在全目录索引；INF 未列出的依赖发现不了，局部副本也会被判完整。正确做法：始终整目录复制，不手工裁剪 `.sys`、`.cat`、`.dll`。
5. 现象：同名驱动留下旧版本。原因：去重键只有 `Class|INF 名`，日期只认 `MM/dd/yyyy`；本地化日期或 `yyyy-MM-dd` 都变成 `MinValue`，排序结果不确定。正确做法：更新驱动后加 `-Force` 并人工确认目标目录版本，或先 `-Clean` 重提。
6. 现象：总量没到 120 MB，后面的包却被跳过。原因：目标已存在且未 `-Force` 时仍把源包体积计入 `$totalBytes`。正确做法：更新驱动用 `-Force`；不需要的旧目录先删除。
7. 现象：运行 `-Clean` 后构建提示驱动目录为空，`说明.txt` 也没了。原因：脚本 `Remove-Item -Recurse -Force` 清空整个输出根目录。正确做法：从版本库恢复 `pe\drivers\说明.txt`，或改用临时 `-OutputDirectory` 再合并。
8. 现象：DISM 注入报签名错误。原因：构建命令没有 `/ForceUnsigned`，测试签名或未签名驱动会被拒绝。正确做法：使用 WHQL 签名包；不要为过构建而加 `/ForceUnsigned`，PE 默认也不接受未签名驱动。
9. 现象：boot.wim 超过 1200 MB 才发现，构建失败但 WIM 已提交。原因：体积检查在 `/Unmount-Image /Commit` 之后。正确做法：先精简 `pe\drivers` 与 `pe\apps`，重新挂载或重跑构建；不要只删源目录就以为 WIM 会变小。
10. 现象：手动「加载驱动…」看不到放在 `\FWIMG\Drivers` 的驱动。原因：手动入口走 `FindInfFilesOnVolumes`，只收集各盘 `\Drivers`；`\FWIMG\Drivers` 仅参与开机自动加载。正确做法：把驱动放任意分区 `\Drivers`，或在文件选择框里直接选 INF。
11. 现象：在 ARM64 机器上提取的驱动注入后全部无效。原因：脚本不校验 CPU 架构，DISM/DriverStore 返回 arm64 包，而 PE 是 amd64。正确做法：在同架构 x64 机器上提取，或核对包名与 INF 中的 `NTamd64` 段。
12. 现象：非管理员运行结果与管理员的 DISM 清单不一致。原因：回退模式每个包只取第一个 INF，多 INF 包可能归错 Class；DISM 模式以 `ClassName` 为准。正确做法：尽量以管理员运行；对比输出类别与包名。
13. 现象：`-Include SCSIAdapter` 有效，写 `-Include Storage` 却一个包都不提。原因：`-Include` 只匹配 INF `[Version]` 的 `Class` 原值，不认常用别名。正确做法：用 `Net`、`USB`、`SCSIAdapter`、`HIDClass`、`Mouse`、`Keyboard`、`System` 这些精确类名。
14. 现象：脚本校验通过，DISM 仍报 `CatalogFile` 缺失。原因：缺文件检查只看 `[SourceDisksFiles]`，不检查 `[Version]` 的 `CatalogFile` 或 `DriverVer` 引用的文件。正确做法：确认每个包都有同名 `.cat`，不要手工删目录里的证书文件。
15. 现象：重复构建后 boot.wim 只增不减，同一驱动出现多个版本。原因：每次都在已有 boot.wim 上叠加 `/Add-Driver`，未从原始 winre.wim 重新 `copype` 时旧驱动仍在。正确做法：重建时先清理挂载目录或从干净 boot.wim 起步，再注入一次。
16. 现象：USB 类一个包都没进去，System 却接近上限。原因：按驱动清单枚举顺序累计 `$totalBytes`，先遇到的类别先占预算，没有按类配额。正确做法：分批运行，例如先 `-Include SCSIAdapter,USB` 再单独提 `System`，最后合并 `pe\drivers`。
17. 现象：执行 `-Clean` 后 `pe\drivers` 只剩空根目录，构建提示没有 `.inf`。原因：脚本总是重建输出根目录，但类别子目录只在真正复制时创建，本次全部跳过就为空。正确做法：看汇总里的跳过原因，放宽限制或换输出目录后重跑。

构建输出判读：出现 `发现 N 个 INF` 才说明驱动源被识别；`dism` 退出码非 0 必须当失败；提交后的体积行是唯一且滞后的 1200 MB 门禁。

## 验证清单

- [ ] 先用默认参数在干净输出目录跑一遍，确认汇总打印各类别包数与 MB，且没有「不完整」告警。
- [ ] `pe\drivers` 下不存在任何 `c_*` 目录，Net 下没有无线大包。
- [ ] 每个复制包内同时存在 INF、SYS 与 CAT，`[SourceDisksFiles]` 文件无缺失。
- [ ] 总大小不超过 `-MaxTotalMB`，且预留 apps 后仍低于 1200 MB。
- [ ] 构建日志出现 `发现 N 个 INF，递归注入`，DISM 无 0x800F024B 等签名错误。
- [ ] boot.wim 提交后体积行显示低于 1200 MB。
- [ ] 目标机型实机启动，存储、网卡、触控板在设备管理器无黄色感叹号。
- [ ] PE 内 `%TEMP%\FireWinPE-boot.log` 显示驱动加载成功数符合预期。

## 排障索引

| 现象 | 先查什么 | 处置 |
|---|---|---|
| DISM 注入整批失败 | 是否有 `c_*` 或缺 SYS 的包 | 删除问题包，整目录重提 |
| PE 看不到硬盘 | 是否含 `iastorav`、`stornvme`、`lsi_sas` | 补对应存储包后重建 |
| 触控板无反应 | System 下 GPIO/I2C/HID 三件套 | 在同型号机器重提 |
| 驱动包被跳过 | 汇总里的跳过原因与大小 | 调整 `-MaxPackageMB`、`-MaxTotalMB` |
| 同名驱动仍是旧版 | 目标目录 DriverVer 与日期格式 | 加 `-Force` 或 `-Clean` |
| build.wim 超 1200 MB | `pe\apps` 与 `pe\drivers` 体积占比 | 精简后重跑构建 |
| 手动加载列表为空 | 各盘 `\Drivers` 是否存在 INF | 放驱动或浏览选择 INF |

## 相关 skill

- `firewin-pe-runtime`：负责开机 drvload 顺序、`\FWIMG\Drivers` 与手动加载入口。
- `firewin-pe-apps`：与驱动共同占用 boot.wim 1200 MB 预算。
- `firewin-usb-boot`：桌面端制作 PE 与 payload 分发，使用构建产物。
