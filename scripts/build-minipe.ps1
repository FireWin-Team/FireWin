#requires -Version 5.1
<#
.SYNOPSIS
    构建 FireWin 的迷你 PE（boot.wim + ISO）。

.DESCRIPTION
    流程：
      1. 定位 ADK（含 WinPE 附加组件）；
      2. copype amd64 生成基础 PE；
      3. 挂载 boot.wim，按依赖顺序注入 WinPE 可选组件；
      4. 把 PE 端程序与第三方工具复制到 \Tools\；
      5. 把常用工具（-AppsDirectory，默认 pe\apps）递归复制到 \Tools\Apps\；
      6. 修改 startnet.cmd，在 wpeinit 之后启动图形启动器（先选工具，再点「开始安装」）；
      7. 提交卸载并用 MakeWinPEMedia 导出 ISO；
      8. 可选地把 boot.wim / boot.sdi / Tools 组装成桌面端可直接使用的 PE 源目录。

.EXAMPLE
    .\build-minipe.ps1 -ToolsSource ..\pe\build\bin -VendorToolsDirectory ..\tools -OutputIso ..\dist\FireWinPE.iso

.NOTES
    许可证：GPL-3.0-only
    需要：管理员权限；已安装 Windows ADK 与 WinPE 附加组件（可用 install-adk.ps1）。
#>
[CmdletBinding()]
param(
    # 体系结构，目前仅支持 amd64
    [ValidateSet('amd64')]
    [string]$Architecture = 'amd64',

    # PE 工作目录（copype 输出）
    [string]$WorkDirectory = "$env:SystemDrive\WinPE_amd64",

    # WinPE 可选组件的语言包后缀，例如 en-us / zh-cn
    [string]$Language = 'en-us',

    # 编译产物所在目录（包含 FireWinPE.exe）
    [string]$ToolsSource,

    # 第三方工具目录（包含 wimlib-imagex.exe、libwim-15.dll、7za.exe）
    [string]$VendorToolsDirectory,

    # 常用工具目录（Explorer++ / 7-Zip / Notepad++ 等），递归复制到 PE 的 \Tools\Apps\。
    # 默认使用仓库内的 pe\apps；该目录不存在时只告警并跳过，不影响 PE 构建。
    [string]$AppsDirectory,

    # ISO 输出路径
    [string]$OutputIso,

    # 是否在构建完成后组装桌面端使用的 PE 源目录（boot.wim + boot.sdi + Tools）
    [switch]$AssemblePayload,

    # 组装输出目录
    [string]$PayloadOutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot

function Write-Step {
    param([string]$Message)
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要管理员权限：挂载 WIM 与写入引导目录都必须提权。'
    }
}

function Get-KitsRoot {
    $paths = @(
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
    )

    foreach ($path in $paths) {
        if (Test-Path $path) {
            $value = (Get-ItemProperty -Path $path -Name 'KitsRoot10' -ErrorAction SilentlyContinue).KitsRoot10
            if ($value) {
                return $value.TrimEnd('\') + '\'
            }
        }
    }

    throw '未找到 ADK（注册表 KitsRoot10 不存在）。请先运行 install-adk.ps1。'
}

function Find-AdkTool {
    param(
        [string]$SearchRoot,
        [string]$Name
    )

    $direct = Join-Path $SearchRoot $Name
    if (Test-Path $direct) {
        return (Resolve-Path $direct).Path
    }

    $found = Get-ChildItem -Path $SearchRoot -Filter $Name -Recurse -File -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $found) {
        throw "在 $SearchRoot 下找不到 $Name，请确认 ADK 安装完整。"
    }

    return $found.FullName
}

function Resolve-ToolsSource {
    param([string]$Explicit)

    if ($Explicit) {
        $resolved = Resolve-Path $Explicit -ErrorAction SilentlyContinue
        if ($null -eq $resolved) {
            throw "ToolsSource 不存在：$Explicit"
        }

        return $resolved.Path
    }

    $candidates = @(
        (Join-Path $repoRoot 'pe\build\bin\Release'),
        (Join-Path $repoRoot 'pe\build\bin'),
        (Join-Path $repoRoot 'pe\build\Release'),
        (Join-Path $repoRoot 'pe\build')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate 'FireWinPE.exe')) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw '找不到 FireWinPE.exe。请先编译 pe\ 工程，或用 -ToolsSource 指定产物目录。'
}

function Resolve-VendorTools {
    param([string]$Explicit)

    $directory = if ($Explicit) { $Explicit } else { Join-Path $repoRoot 'tools' }
    $resolved = Resolve-Path $directory -ErrorAction SilentlyContinue
    if ($null -eq $resolved) {
        throw "第三方工具目录不存在：$directory（需包含 wimlib-imagex.exe、libwim-15.dll、7za.exe）"
    }

    foreach ($name in @('wimlib-imagex.exe', 'libwim-15.dll', '7za.exe')) {
        if (-not (Test-Path (Join-Path $resolved.Path $name))) {
            throw "第三方工具缺少 $name（目录：$($resolved.Path)）"
        }
    }

    return $resolved.Path
}

function Invoke-Dism {
    param(
        [string]$Dism,
        [string[]]$Arguments
    )

    Write-Host "  dism $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $Dism @Arguments | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) {
        throw "DISM 失败（退出码 $LASTEXITCODE）：$($Arguments -join ' ')"
    }
}

function Add-WinPePackage {
    param(
        [string]$Dism,
        [string]$MountDirectory,
        [string]$PackageDirectory,
        [string]$PackageName,
        [string]$Language
    )

    $cab = Join-Path $PackageDirectory "$PackageName.cab"
    if (-not (Test-Path $cab)) {
        throw "缺少可选组件：$cab，请确认 WinPE 附加组件安装完整。"
    }

    Invoke-Dism -Dism $Dism -Arguments @("/Add-Package", "/Image:$MountDirectory", "/PackagePath:$cab")

    # 语言包是可选的：有的语言没有对应 cab，跳过即可
    $languageCab = Join-Path $PackageDirectory "${PackageName}_$Language.cab"
    if (Test-Path $languageCab) {
        Invoke-Dism -Dism $Dism -Arguments @("/Add-Package", "/Image:$MountDirectory", "/PackagePath:$languageCab")
    }
    else {
        Write-Host "  跳过语言包（不存在）：$languageCab" -ForegroundColor DarkYellow
    }
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
Assert-Administrator

Write-Step '定位 ADK'
$kitsRoot = Get-KitsRoot
$adkRoot = Join-Path $kitsRoot 'Assessment and Deployment Kit'
$deploymentTools = Join-Path $adkRoot 'Deployment Tools'
$winPeRoot = Join-Path $adkRoot "Windows Preinstallation Environment\$Architecture\WinPE_OCs"

if (-not (Test-Path $deploymentTools)) {
    throw "ADK Deployment Tools 不存在：$deploymentTools"
}
if (-not (Test-Path $winPeRoot)) {
    throw "WinPE 可选组件目录不存在：$winPeRoot（WinPE 附加组件未安装？）"
}

# copype.cmd / MakeWinPEMedia.cmd 由 WinPE 附加组件安装在 ADK 根目录下，不在 Deployment Tools 里
$copype = Find-AdkTool -SearchRoot $adkRoot -Name 'copype.cmd'
$makeWinPeMedia = Find-AdkTool -SearchRoot $adkRoot -Name 'MakeWinPEMedia.cmd'

# copype 依赖 ADK 的一整套环境变量（平时由 DandISetEnv.bat 设置），从普通 PowerShell 调用时必须自己补齐
$env:WinPERoot = Join-Path $adkRoot 'Windows Preinstallation Environment'
$env:WinPERootNoArch = $env:WinPERoot
$env:OSCDImgRoot = Join-Path $deploymentTools "$Architecture\Oscdimg"
$env:DISMRoot = Join-Path $deploymentTools "$Architecture\DISM"
$env:BCDBootRoot = Join-Path $deploymentTools "$Architecture\BCDBoot"
$env:ImagingRoot = Join-Path $deploymentTools "$Architecture\Imaging"
$env:WdsmcastRoot = Join-Path $deploymentTools "$Architecture\Wdsmcast"
$env:WindowsSetupRootNoArch = Join-Path $adkRoot 'Windows Setup'
$env:USMTRootNoArch = Join-Path $adkRoot 'User State Migration Tool'
$env:PATH = "$env:DISMRoot;$env:OSCDImgRoot;$env:BCDBootRoot;$env:ImagingRoot;$env:PATH"

$dism = Join-Path $deploymentTools "$Architecture\DISM\dism.exe"
if (-not (Test-Path $dism)) {
    $dism = Find-AdkTool -SearchRoot $deploymentTools -Name 'dism.exe'
}

$toolsSource = Resolve-ToolsSource -Explicit $ToolsSource
$vendorTools = Resolve-VendorTools -Explicit $VendorToolsDirectory
$mountDirectory = Join-Path $WorkDirectory 'mount'

Write-Step "生成基础 PE：$WorkDirectory"
if (Test-Path $WorkDirectory) {
    Remove-Item -Path $WorkDirectory -Recurse -Force
}
& cmd.exe /c "`"$copype`" $Architecture `"$WorkDirectory`""
if ($LASTEXITCODE -ne 0) {
    throw "copype 失败（退出码 $LASTEXITCODE）"
}

$bootWim = Join-Path $WorkDirectory 'media\sources\boot.wim'
if (-not (Test-Path $bootWim)) {
    throw "copype 未生成 boot.wim：$bootWim"
}

$mounted = $false
try {
    Write-Step '挂载 boot.wim'
    New-Item -ItemType Directory -Path $mountDirectory -Force | Out-Null
    Invoke-Dism -Dism $dism -Arguments @("/Mount-Image", "/ImageFile:$bootWim", '/Index:1', "/MountDir:$mountDirectory")
    $mounted = $true

    Write-Step '注入 WinPE 可选组件'
    # 依赖关系：WinPE-NetFx 需要 WinPE-WMI；WinPE-PowerShell 需要 WinPE-NetFx 与 WinPE-Scripting。
    # 因此这里的顺序为 WMI → NetFx → Scripting → PowerShell。
    # （WinPE-NetFx 只对需要 .NET 组件的工具才有意义，纯原生 exe 可以删掉这一项以减小体积。）
    foreach ($package in @('WinPE-WMI', 'WinPE-NetFx', 'WinPE-Scripting', 'WinPE-PowerShell')) {
        Write-Host "  注入 $package" -ForegroundColor Yellow
        Add-WinPePackage -Dism $dism -MountDirectory $mountDirectory -PackageDirectory $winPeRoot -PackageName $package -Language $Language
    }

    # 中文界面还需要字体支持包，否则启动器与工具里的中文会显示成方块
    if ($Language -like 'zh-*') {
        $fontPackage = 'WinPE-FontSupport-' + $Language.ToUpperInvariant()
        $fontCab = Join-Path $winPeRoot "$fontPackage.cab"
        if (Test-Path $fontCab) {
            Write-Host "  注入 $fontPackage" -ForegroundColor Yellow
            Invoke-Dism -Dism $dism -Arguments @('/Add-Package', "/Image:$mountDirectory", "/PackagePath:$fontCab")
        }
        else {
            Write-Warning "缺少中文字体包：$fontCab，启动器中文可能显示异常。"
        }
    }

    Write-Step '复制 PE 端程序与第三方工具到 \Tools'
    $targetTools = Join-Path $mountDirectory 'Tools'
    New-Item -ItemType Directory -Path $targetTools -Force | Out-Null

    Copy-Item -Path (Join-Path $toolsSource 'FireWinPE.exe') -Destination $targetTools -Force
    # 独立引导工具（写 PE 引导 / 修复系统引导）：可选，缺失只告警
    $bootTool = Join-Path $toolsSource 'FireBoot.exe'
    if (Test-Path $bootTool) {
        Copy-Item -Path $bootTool -Destination $targetTools -Force
    }
    else {
        Write-Warning "未找到 FireBoot.exe（$bootTool），启动器里的『引导工具』按钮将不可用。"
    }
    foreach ($name in @('wimlib-imagex.exe', 'libwim-15.dll', '7za.exe')) {
        Copy-Item -Path (Join-Path $vendorTools $name) -Destination $targetTools -Force
    }

    Write-Step '复制常用工具到 \Tools\Apps'
    # 图形启动器的按钮直接调用 X:\Tools\Apps\<工具>\*.exe，因此保持原有子目录结构；
    # 目录不存在时只告警并跳过（例如裁剪过的仓库），不影响 PE 主流程。
    $appsSource = if ($AppsDirectory) { $AppsDirectory } else { Join-Path $repoRoot 'pe\apps' }
    if (Test-Path $appsSource) {
        $targetApps = Join-Path $targetTools 'Apps'
        New-Item -ItemType Directory -Path $targetApps -Force | Out-Null
        Copy-Item -Path (Join-Path $appsSource '*') -Destination $targetApps -Recurse -Force
        Write-Host "  常用工具：$appsSource -> $targetApps" -ForegroundColor Yellow
    }
    else {
        Write-Warning "常用工具目录不存在，已跳过：$appsSource"
    }

    Write-Step '修改 startnet.cmd（wpeinit 之后进入图形启动器）'
    $startnet = Join-Path $mountDirectory 'Windows\System32\startnet.cmd'
    $content = if (Test-Path $startnet) { Get-Content -Path $startnet -Raw } else { "wpeinit`r`n" }

    # 以 exe 名（而不是完整参数）作为已注入标记：旧的 /autodeploy 版本同样能识别，避免重复注入
    if ($content -notmatch 'FireWinPE\.exe') {
        $block = @(
            '',
            'rem ---- FireWin launcher entry ----',
            'if exist X:\Tools\FireWinPE.exe (',
            '    X:\Tools\FireWinPE.exe',
            ')'
        ) -join "`r`n"

        if ($content -notmatch '(?im)^\s*wpeinit\s*$') {
            $content = "wpeinit`r`n" + $content
        }

        Set-Content -Path $startnet -Value ($content.TrimEnd() + "`r`n" + $block + "`r`n") -Encoding ASCII
    }

    Write-Step '提交并卸载 boot.wim'
    Invoke-Dism -Dism $dism -Arguments @('/Unmount-Image', "/MountDir:$mountDirectory", '/Commit')
    $mounted = $false

    if ($OutputIso) {
        Write-Step "导出 ISO：$OutputIso"
        $isoDirectory = Split-Path -Parent $OutputIso
        if ($isoDirectory -and -not (Test-Path $isoDirectory)) {
            New-Item -ItemType Directory -Path $isoDirectory -Force | Out-Null
        }

        & cmd.exe /c "`"$makeWinPeMedia`" /ISO `"$WorkDirectory`" `"$OutputIso`""
        if ($LASTEXITCODE -ne 0) {
            throw "MakeWinPEMedia 失败（退出码 $LASTEXITCODE）"
        }
    }

    if ($AssemblePayload) {
        Write-Step '组装桌面端 PE 源目录'
        # 默认直接落到桌面端随包目录 payload\pe，构建完即可随 exe 分发
        $payload = if ($PayloadOutputDirectory) { $PayloadOutputDirectory } else { Join-Path $repoRoot 'payload\pe' }
        New-Item -ItemType Directory -Path $payload -Force | Out-Null

        Copy-Item -Path $bootWim -Destination (Join-Path $payload 'boot.wim') -Force
        $bootSdi = Join-Path $WorkDirectory 'media\Boot\boot.sdi'
        if (-not (Test-Path $bootSdi)) {
            throw "找不到 boot.sdi：$bootSdi"
        }
        Copy-Item -Path $bootSdi -Destination (Join-Path $payload 'boot.sdi') -Force

        $payloadTools = Join-Path $payload 'Tools'
        New-Item -ItemType Directory -Path $payloadTools -Force | Out-Null
        Copy-Item -Path (Join-Path $toolsSource 'FireWinPE.exe') -Destination $payloadTools -Force
        $payloadBootTool = Join-Path $toolsSource 'FireBoot.exe'
        if (Test-Path $payloadBootTool) {
            Copy-Item -Path $payloadBootTool -Destination $payloadTools -Force
        }
        foreach ($name in @('wimlib-imagex.exe', 'libwim-15.dll', '7za.exe')) {
            Copy-Item -Path (Join-Path $vendorTools $name) -Destination $payloadTools -Force
        }

        Write-Host "  PE 源目录：$payload" -ForegroundColor Green
    }

    Write-Step '完成'
    Write-Host "  boot.wim：$bootWim" -ForegroundColor Green
    if ($OutputIso) { Write-Host "  ISO：$OutputIso" -ForegroundColor Green }
}
finally {
    # 任何失败都要卸载，否则下一次 DISM 会报「映像已挂载」
    if ($mounted) {
        Write-Warning '构建失败，正在丢弃挂载的映像……'
        & $dism /Unmount-Image "/MountDir:$mountDirectory" /Discard | Out-Null
    }
}
