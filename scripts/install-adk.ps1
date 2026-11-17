#requires -Version 5.1
<#
.SYNOPSIS
    安装 Windows ADK 主包与 WinPE 附加组件（静默）。

.DESCRIPTION
    两者是两个独立的安装程序，必须分别安装：
      - ADK 主包            ：提供 Deployment Tools（copype / DISM / MakeWinPEMedia）
      - WinPE 附加组件       ：提供 WinPE_OCs 与 Windows Preinstallation Environment 目录
    安装策略：winget 优先，失败则回退到官方安装程序（本地路径或在线下载）。

.EXAMPLE
    .\install-adk.ps1

.EXAMPLE
    .\install-adk.ps1 -InstallPath C:\ADK -AdkSetupPath D:\adksetup.exe -WinPeSetupPath D:\adkwinpesetup.exe

.NOTES
    许可证：GPL-3.0-only
    需要：管理员权限、网络（走下载回退时）。
#>
[CmdletBinding()]
param(
    # ADK 自定义安装目录；留空则使用微软默认目录
    [string]$InstallPath,

    # 本地 adksetup.exe 路径（可选，提供后不再走 winget / 下载）
    [string]$AdkSetupPath,

    # 本地 adkwinpesetup.exe 路径（可选）
    [string]$WinPeSetupPath,

    # 即使已安装也重新执行安装
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# 官方直链（ADK 10.1.26100 系列）。若失效，请替换为 learn.microsoft.com 上最新的下载地址。
$AdkSetupUrl = 'https://go.microsoft.com/fwlink/?linkid=2289980'
$WinPeSetupUrl = 'https://go.microsoft.com/fwlink/?linkid=2289981'

$WingetAdkId = 'Microsoft.WindowsADK'
$WingetWinPeAddonId = 'Microsoft.WindowsADK.WinPEAddon'

function Write-Step {
    param([string]$Message)
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要管理员权限：ADK 静默安装会写入系统目录与注册表。'
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

    return $null
}

function Get-AdkStatus {
    $kitsRoot = Get-KitsRoot
    if ($null -eq $kitsRoot) {
        return [pscustomobject]@{ KitsRoot = $null; AdkInstalled = $false; WinPeAddonInstalled = $false }
    }

    $adkRoot = Join-Path $kitsRoot 'Assessment and Deployment Kit'
    return [pscustomobject]@{
        KitsRoot             = $kitsRoot
        AdkInstalled         = Test-Path (Join-Path $adkRoot 'Deployment Tools')
        WinPeAddonInstalled  = Test-Path (Join-Path $adkRoot 'Windows Preinstallation Environment')
    }
}

function Invoke-Winget {
    param([string]$PackageId, [string]$DisplayName)

    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($null -eq $winget) {
        Write-Host '  未找到 winget，跳过。' -ForegroundColor DarkYellow
        return $false
    }

    Write-Host "  winget install $PackageId" -ForegroundColor DarkGray
    & $winget.Source install --id $PackageId --exact --silent `
        --accept-package-agreements --accept-source-agreements --disable-interactivity

    if ($LASTEXITCODE -eq 0) {
        return $true
    }

    Write-Host "  winget 安装 $DisplayName 失败（退出码 $LASTEXITCODE），准备回退。" -ForegroundColor DarkYellow
    return $false
}

function Invoke-Setup {
    param(
        [string]$SetupPath,
        [string]$DownloadUrl,
        [string]$FallbackFileName,
        [string[]]$Arguments,
        [string]$DisplayName
    )

    if (-not $SetupPath) {
        $SetupPath = Join-Path $env:TEMP $FallbackFileName
        if (-not (Test-Path $SetupPath)) {
            Write-Host "  下载 $DisplayName 安装程序……" -ForegroundColor DarkGray
            Invoke-WebRequest -Uri $DownloadUrl -OutFile $SetupPath -UseBasicParsing
        }
    }

    if (-not (Test-Path $SetupPath)) {
        throw "找不到安装程序：$SetupPath"
    }

    Write-Host "  运行 $SetupPath $($Arguments -join ' ')" -ForegroundColor DarkGray
    $process = Start-Process -FilePath $SetupPath -ArgumentList $Arguments -Wait -PassThru

    # 3010 = 成功但需要重启
    if ($process.ExitCode -ne 0 -and $process.ExitCode -ne 3010) {
        throw "$DisplayName 安装失败（退出码 $($process.ExitCode)）。"
    }

    if ($process.ExitCode -eq 3010) {
        Write-Warning "$DisplayName 安装完成，但需要重启后才会完全生效。"
    }
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
Assert-Administrator

$status = Get-AdkStatus
Write-Host "KitsRoot10：$(if ($status.KitsRoot) { $status.KitsRoot } else { '(未找到)' })"

if ($status.AdkInstalled -and $status.WinPeAddonInstalled -and -not $Force) {
    Write-Step 'ADK 与 WinPE 附加组件均已安装，无需处理'
    exit 0
}

if (-not $status.AdkInstalled -or $Force) {
    Write-Step '安装 Windows ADK 主包'

    $installed = $false
    if (-not $AdkSetupPath) {
        $installed = Invoke-Winget -PackageId $WingetAdkId -DisplayName 'Windows ADK'
    }

    if (-not $installed) {
        $adkArguments = @('/quiet', '/norestart', '/ceip', 'off', '/features', 'OptionId.DeploymentTools')
        if ($InstallPath) {
            $adkArguments += @('/installpath', $InstallPath)
        }

        Invoke-Setup -SetupPath $AdkSetupPath -DownloadUrl $AdkSetupUrl `
            -FallbackFileName 'adksetup.exe' -Arguments $adkArguments -DisplayName 'Windows ADK'
    }
}

$status = Get-AdkStatus
if (-not $status.AdkInstalled) {
    throw 'ADK 主包安装后仍未检测到 Deployment Tools，请重启后重试。'
}

if (-not $status.WinPeAddonInstalled -or $Force) {
    Write-Step '安装 WinPE 附加组件'

    $installed = $false
    if (-not $WinPeSetupPath) {
        $installed = Invoke-Winget -PackageId $WingetWinPeAddonId -DisplayName 'WinPE Addon'
    }

    if (-not $installed) {
        Invoke-Setup -SetupPath $WinPeSetupPath -DownloadUrl $WinPeSetupUrl `
            -FallbackFileName 'adkwinpesetup.exe' -Arguments @('/quiet', '/norestart') -DisplayName 'WinPE 附加组件'
    }
}

$status = Get-AdkStatus
if (-not $status.WinPeAddonInstalled) {
    throw 'WinPE 附加组件安装后仍未检测到 Windows Preinstallation Environment 目录，请重启后重试。'
}

Write-Step '完成'
Write-Host "  ADK：$($status.KitsRoot)Assessment and Deployment Kit" -ForegroundColor Green
Write-Host '  下一步：运行 build-minipe.ps1 构建迷你 PE。' -ForegroundColor Green
