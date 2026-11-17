#requires -Version 5.1
<#
.SYNOPSIS
    从本机 Windows 的 DriverStore 里提取常用基础驱动，输出到 pe\drivers\。

.DESCRIPTION
    为什么要这一步：WinPE 自带的驱动只有通用的存储/输入/网络，装到大部分笔记本上
    会出现没有网卡（连不上网下载镜像）、USB 3 不识别、触控板不工作（I2C 精密触控板
    缺 hidi2c 与厂商驱动）等问题。本脚本把「当前这台机器」上已安装好的驱动包复制出来，
    构建 PE 时由 build-minipe.ps1 用 dism /Add-Driver /Recurse 注入 boot.wim。

    默认挑选策略（面向 WinPE，控制 boot.wim 体积）：
      · 网卡只收有线 / 虚拟 / USB 网卡：PE 里没有 wlansvc，Wi-Fi 驱动装了也连不上，
        而 Intel/Realtek 的无线包动辄 10~100MB，会白白撑大镜像；
      · System 类只收 GPIO / I2C / SPI / HID 相关（触控板、触摸屏的依赖链）；
        其余 System 包（芯片组、ME、音频、传感器等）PE 用不到，共占 300MB+；
      · 存储控制器、USB、HIDClass、鼠标、键盘全部收（合计约 30MB）；
      · 单包上限 -MaxPackageMB（默认 20MB），总量上限 -MaxTotalMB（默认 120MB）；
      · 显卡默认不收（单个 Intel 显卡包就有 2GB），PE 用微软基础显示驱动即可。

    不需要管理员权限：没有 DISM / 不是管理员时，直接读 DriverStore 里每个包目录的
    INF 判断驱动类别（只读，不改动系统）。

.EXAMPLE
    .\extract-drivers.ps1                        # 默认精简包，输出到 pe\drivers
    .\extract-drivers.ps1 -Clean                 # 先清空 pe\drivers 再提取（会删掉 说明.txt）
    .\extract-drivers.ps1 -Include Net,USB       # 只提取网卡和 USB
    .\extract-drivers.ps1 -SkipInf @() -OnlyInf @()   # 不加过滤，按类别全收
    .\extract-drivers.ps1 -Include Display -MaxPackageMB 2500   # 额外收显卡（谨慎，会超 1.2GB）

.NOTES
    许可证：GPL-3.0-only。驱动版权属于各自厂商，仅用于自用部署，请勿对外分发。
#>
[CmdletBinding()]
param(
    # 驱动输出目录（默认 pe\drivers）
    [string]$OutputDirectory,

    # 需要提取的驱动类（INF 里的 Class 字段）
    # HIDClass / Mouse / Keyboard 用于触控板、触摸屏和特殊键鼠（I2C 精密触控板还需要厂商 I2C 驱动）
    [string[]]$Include = @('Net', 'USB', 'SCSIAdapter', 'HIDClass', 'Mouse', 'Keyboard', 'System'),

    # 按 INF 文件名过滤：格式 'Class:正则'，命中的才收（只对指定类别生效）
    [string[]]$OnlyInf = @('System:^(iagpio|ialpss|iai2c|amdi2c|amdgpio|hidi2c|hidspi|hidinterrupt|ishhid|mshid)'),

    # 按 INF 文件名排除：格式 'Class:正则'（Class 写 * 表示所有类别）
    #   c_*      微软「类扩展」INF，PE 里没有对应 SYS（本机 c_hidclass 就缺文件），dism 注入会整批失败
    #   bth*     PE 没有蓝牙栈，蓝牙 HID / PAN 收了也用不上
    #   其余     苹果 USB、高通 USB、InfiniBand、内核调试网卡、虚拟 WLAN 等都是无用的死重量
    [string[]]$SkipInf = @('*:^c_',
                           'Net:^(netwtw|netwbw|netwlv|netwns|netwsw|netwew|netrtwlan|rtwlanu|netr28|netathr|athw8x|netbc|bcmdhd|bcmwdidhdpcie|mrvlpcie|net8192|net8187|qcwlan|mtkmbim|netvwwan|qcwwan|net9500|net7500|net7800|net7400|ykinx64|netadapter|netvchannel|netvwifimp|msdri|netavpna|netimm|ndisimplatform|wceisvista|rndiscmp|netsstpa|netrasa|netloop|wintun|tetherxp|bthpan|ipoib6x|kdnic|wvms_mp_windows)',
                           'HIDClass:^(bthoob|hidbth)',
                           'USB:^(usbaapl64|qdbusb|qcfilter)'),

    # 单个驱动包大小上限（MB），超过就跳过
    [int]$MaxPackageMB = 20,

    # 总量上限（MB），超出后剩余的驱动包不再复制
    [int]$MaxTotalMB = 120,

    # 先清空输出目录再提取
    [switch]$Clean,

    # 已存在的驱动包也重新复制
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$driversRoot = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $repoRoot 'pe\drivers' }
$fileRepository = Join-Path $env:WINDIR 'System32\DriverStore\FileRepository'

function Write-Line {
    param([string]$Text, [string]$Color = 'Gray')
    Write-Host $Text -ForegroundColor $Color
}

function Get-FolderSize {
    param([string]$Path)
    $sum = (Get-ChildItem -Path $Path -Recurse -File -Force -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum).Sum
    if ($null -eq $sum) { 0 } else { $sum }
}

# INF 的 [Version] 段里读 Class（如 Net / USB / SCSIAdapter）
function Get-InfClass {
    param([string]$InfPath)
    $line = Select-String -LiteralPath $InfPath -Pattern '^\s*Class\s*=' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $line) { return '' }
    return ($line.Line -split '=', 2)[1].Trim()
}

# 把 'Class:正则' 形式的参数解析成规则表
function ConvertTo-InfRules {
    param([string[]]$Entries)
    $rules = @()
    foreach ($entry in $Entries) {
        if (-not $entry) { continue }
        $index = $entry.IndexOf(':')
        if ($index -lt 1) { continue }
        $rules += [pscustomobject]@{
            Class   = $entry.Substring(0, $index).Trim()
            Pattern = $entry.Substring($index + 1)
        }
    }
    return $rules
}

function Test-InfRules {
    param([string]$ClassName, [string]$InfBase, $Rules)
    foreach ($rule in $Rules) {
        if (($rule.Class -eq '*') -or ($ClassName -ieq $rule.Class)) {
            if ($InfBase -imatch $rule.Pattern) { return $true }
        }
    }
    return $false
}

# OnlyInf 的语义：某个类别只要写了规则，就只收命中规则的包；没写规则的类别全收
function Test-InfOnlyAllowed {
    param([string]$ClassName, [string]$InfBase, $Rules)
    $classRules = @($Rules | Where-Object { $_.Class -ieq $ClassName })
    if ($classRules.Count -eq 0) { return $true }

    foreach ($rule in $classRules) {
        if ($InfBase -imatch $rule.Pattern) { return $true }
    }
    return $false
}

# 校验驱动包完整性：INF 的 [SourceDisksFiles] 里列出的文件必须都在包目录里，
# 缺文件时 dism /Add-Driver 会直接失败，导致整批驱动注入中断
function Get-MissingPackageFiles {
    param([string]$InfPath)

    $dir = Split-Path -Parent $InfPath
    $index = @{}
    Get-ChildItem -LiteralPath $dir -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
        $index[$_.Name.ToLowerInvariant()] = $true
    }

    $missing = New-Object System.Collections.Generic.List[string]
    $inSection = $false
    foreach ($line in (Get-Content -LiteralPath $InfPath -Encoding Default -ErrorAction SilentlyContinue)) {
        $text = $line.Trim()
        if ($text -match '^\[(.+)\]$') {
            $inSection = ($matches[1].Trim() -ieq 'SourceDisksFiles')
            continue
        }
        if (-not $inSection -or -not $text -or $text.StartsWith(';')) { continue }

        $name = ($text -split '\s*=\s*')[0].Trim()
        if (-not $name) { continue }

        $leaf = Split-Path -Leaf $name
        if (-not $index.ContainsKey($leaf.ToLowerInvariant())) { $missing.Add($leaf) }
    }
    return $missing
}

# INF 的 [Version] 段里读 DriverVer 日期，用于同一驱动多版本时挑最新
function Get-InfDriverVerDate {
    param([string]$InfPath)
    $line = Select-String -LiteralPath $InfPath -Pattern '^\s*DriverVer\s*=' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $line) { return [datetime]::MinValue }

    $value = ($line.Line -split '=', 2)[1].Trim()
    $dateText = ($value -split ',')[0].Trim()
    $parsed = [datetime]::MinValue
    $style = [Globalization.DateTimeStyles]::None
    if ([datetime]::TryParseExact($dateText, 'MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture, $style, [ref]$parsed)) {
        return $parsed
    }
    return [datetime]::MinValue
}

# 直接读 DriverStore：不需要管理员 / DISM，851 个包目录扫一遍
function Get-DriverStorePackages {
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($dir in Get-ChildItem -LiteralPath $fileRepository -Directory -ErrorAction SilentlyContinue) {
        $inf = Get-ChildItem -LiteralPath $dir.FullName -Filter '*.inf' -File -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $inf) { continue }

        $className = Get-InfClass -InfPath $inf.FullName
        if (-not $className) { continue }

        $rows.Add([pscustomobject]@{
            Class     = $className
            Package   = $dir.Name
            Directory = $dir.FullName
            InfName   = $inf.Name
        })
    }
    return $rows
}

# 通过 DISM 读（需要管理员，更权威）
function Get-DismPackages {
    Import-Module DISM -ErrorAction Stop
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($driver in (Get-WindowsDriver -Online -All)) {
        if (-not $driver.Driver -or -not $driver.ClassName) { continue }
        $dir = Split-Path -Parent $driver.Driver
        if (-not $dir) { continue }
        $rows.Add([pscustomobject]@{
            Class     = $driver.ClassName
            Package   = Split-Path -Leaf $dir
            Directory = $dir
            InfName   = Split-Path -Leaf $driver.Driver
        })
    }
    return $rows
}

# ---------------------------------------------------------------------------
# 前置检查
# ---------------------------------------------------------------------------
if (-not (Test-Path $fileRepository)) {
    throw "找不到 DriverStore 目录：$fileRepository"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($Clean -and (Test-Path $driversRoot)) {
    Write-Line "清空 $driversRoot" 'DarkYellow'
    Remove-Item -Path $driversRoot -Recurse -Force
    # 重建空目录；注意 pe\drivers\说明.txt 也会被一起删掉，需要时从版本库恢复
    if (-not (Test-Path $driversRoot)) { New-Item -ItemType Directory -Path $driversRoot -Force | Out-Null }
}
New-Item -ItemType Directory -Path $driversRoot -Force | Out-Null

$allPackages = $null
if ($isAdmin) {
    Write-Line '==> 读取本机驱动清单（DISM，首次执行需要几十秒）' 'Cyan'
    try {
        $allPackages = Get-DismPackages
    }
    catch {
        Write-Line ("    DISM 读取失败（{0}），改用直接读取 DriverStore" -f $_.Exception.Message) 'DarkYellow'
    }
}
if ($null -eq $allPackages) {
    Write-Line '==> 读取本机驱动清单（直接扫描 DriverStore，无需管理员）' 'Cyan'
    $allPackages = Get-DriverStorePackages
}

$wanted = @($Include | ForEach-Object { $_.ToLowerInvariant() })
$onlyRules = ConvertTo-InfRules -Entries $OnlyInf
$skipRules = ConvertTo-InfRules -Entries $SkipInf

$candidates = @($allPackages | Where-Object {
    $_.Class -and ($wanted -contains $_.Class.ToLowerInvariant())
} | Where-Object {
    $infBase = $_.InfName -replace '\.inf$', ''
    -not (Test-InfRules -ClassName $_.Class -InfBase $infBase -Rules $skipRules)
} | Where-Object {
    $infBase = $_.InfName -replace '\.inf$', ''
    Test-InfOnlyAllowed -ClassName $_.Class -InfBase $infBase -Rules $onlyRules
})

Write-Line ("    驱动总数 {0}，筛选后 {1} 个驱动包" -f $allPackages.Count, $candidates.Count)

# 同一个 INF 在 DriverStore 里可能有新旧多个版本目录，只留 DriverVer 最新的一个，
# 否则 dism 会把多份都注入进去，白占空间还可能装错版本
$candidates = @($candidates | Group-Object -Property {
        '{0}|{1}' -f $_.Class, ($_.InfName -replace '\.inf$', '')
    } | ForEach-Object {
        $_.Group |
            Sort-Object -Property @{ Expression = { Get-InfDriverVerDate -InfPath (Join-Path $_.Directory $_.InfName) }; Descending = $true } |
            Select-Object -First 1
    })

Write-Line ("    去重后 {0} 个驱动包" -f $candidates.Count)

# ---------------------------------------------------------------------------
# 逐个复制驱动包
# ---------------------------------------------------------------------------
$copied = New-Object System.Collections.Generic.List[object]
$skipped = New-Object System.Collections.Generic.List[object]
$seen = New-Object System.Collections.Generic.HashSet[string]   # 同一个驱动包可能被多个设备共用
$totalBytes = [long]0
$maxBytes = [long]$MaxPackageMB * 1MB
$maxTotalBytes = [long]$MaxTotalMB * 1MB

foreach ($driver in $candidates) {
    $sourceDir = $driver.Directory
    if (-not $sourceDir -or -not (Test-Path $sourceDir)) {
        continue
    }

    $packageName = $driver.Package
    if (-not $seen.Add($packageName)) {
        continue
    }

    $className = $driver.Class
    $targetDir = Join-Path (Join-Path $driversRoot $className) $packageName
    $size = [long](Get-FolderSize -Path $sourceDir)

    $infPath = Join-Path $sourceDir $driver.InfName
    if (Test-Path -LiteralPath $infPath) {
        $missing = @(Get-MissingPackageFiles -InfPath $infPath)
        if ($missing.Count -gt 0) {
            $skipped.Add([pscustomobject]@{
                '类别'   = $className
                '驱动包' = $packageName
                '大小MB' = [math]::Round($size / 1MB, 1)
                '原因'   = "驱动包不完整：缺 $($missing -join ', ')"
            })
            continue
        }
    }

    if ($size -gt $maxBytes) {
        $skipped.Add([pscustomobject]@{
            '类别'   = $className
            '驱动包' = $packageName
            '大小MB' = [math]::Round($size / 1MB, 1)
            '原因'   = "超过 $MaxPackageMB MB"
        })
        continue
    }

    if (($totalBytes + $size) -gt $maxTotalBytes) {
        $skipped.Add([pscustomobject]@{
            '类别'   = $className
            '驱动包' = $packageName
            '大小MB' = [math]::Round($size / 1MB, 1)
            '原因'   = "超过总量 $MaxTotalMB MB"
        })
        continue
    }

    if ((Test-Path $targetDir) -and (-not $Force)) {
        $totalBytes += $size
        $copied.Add([pscustomobject]@{
            '类别'   = $className
            '驱动包' = $packageName
            '大小MB' = [math]::Round($size / 1MB, 1)
            '状态'   = '已存在'
        })
        continue
    }

    Write-Line ("    复制 [{0}] {1}（{2} MB）" -f $className, $packageName, [math]::Round($size / 1MB, 1)) 'DarkGray'
    try {
        New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
        Copy-Item -Path (Join-Path $sourceDir '*') -Destination $targetDir -Recurse -Force -ErrorAction Stop
    }
    catch {
        $skipped.Add([pscustomobject]@{
            '类别'   = $className
            '驱动包' = $packageName
            '大小MB' = [math]::Round($size / 1MB, 1)
            '原因'   = "复制失败：$($_.Exception.Message)"
        })
        continue
    }

    $totalBytes += $size
    $copied.Add([pscustomobject]@{
        '类别'   = $className
        '驱动包' = $packageName
        '大小MB' = [math]::Round($size / 1MB, 1)
        '状态'   = '已复制'
    })
}

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
Write-Host ''
if ($copied.Count -gt 0) {
    Write-Line "==> 已提取 $($copied.Count) 个驱动包，合计 $([math]::Round($totalBytes / 1MB, 1)) MB" 'Green'
    $copied | Group-Object '类别' | Sort-Object Name | ForEach-Object {
        $sizeMB = [math]::Round((($_.Group | Measure-Object -Property '大小MB' -Sum).Sum), 1)
        Write-Line ("    {0,-12} {1,2} 个   {2,7} MB" -f $_.Name, $_.Count, $sizeMB)
    }
}
else {
    Write-Line '==> 没有复制任何驱动包' 'DarkYellow'
}

if ($skipped.Count -gt 0) {
    Write-Host ''
    Write-Line "==> 跳过 $($skipped.Count) 个驱动包" 'DarkYellow'
    $skipped | Group-Object '原因' | ForEach-Object {
        Write-Line ("    {0}：{1} 个" -f $_.Name, $_.Count) 'DarkYellow'
    }
    Write-Line '    （需要时可放宽 -MaxPackageMB / -MaxTotalMB / -SkipInf 后重跑）' 'DarkYellow'
}

Write-Host ''
Write-Line "输出目录：$driversRoot" 'Cyan'
Write-Line '构建 PE 时 build-minipe.ps1 会对该目录执行：dism /Add-Driver /Recurse' 'Cyan'
