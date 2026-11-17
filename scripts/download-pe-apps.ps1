#requires -Version 5.1
<#
.SYNOPSIS
    下载市面常见 PE 工具到 pe\apps\（构建 PE 时随 boot.wim 打包）。

.DESCRIPTION
    每个工具独立 try/catch：某一个下载失败不影响其它工具。
    下载来源全部为官方渠道：
      · 固定直链（voidtools / Sysinternals / cpuid / eassos / cgsecurity / disktool / todesk）
      · GitHub Releases 最新版（Dism++ / System Informer / RustDesk）
      · winget-pkgs 官方清单里登记的下载地址（GPU-Z / HWiNFO）
      · Chrome for Testing 官方 JSON（Google 提供的免安装 Chromium/Chrome 包）
      · 官网下载页解析（BOOTICE / WinNTSetup / HWiNFO）
    已存在目录默认跳过（-Force 可覆盖重新下载）。

.EXAMPLE
    .\download-pe-apps.ps1              # 下载缺失的工具
    .\download-pe-apps.ps1 -Force       # 全部重新下载
    .\download-pe-apps.ps1 -Only Chrome,RustDesk   # 只下载指定目录

.NOTES
    许可证：GPL-3.0-only。各工具版权与授权条款见其官网，自用无妨，对外分发请自行确认。
    本 PE 没有 WoW64，只收录 64 位版本的可执行文件；32 位工具（OSFMount / FastCopy / ToDesk Lite）不再下载。
#>
[CmdletBinding()]
param(
    # 工具落地目录（默认 pe\apps）
    [string]$AppsDirectory,

    # 已存在时也重新下载
    [switch]$Force,

    # 只处理这些工具目录（逗号分隔，便于补单个失败的包）
    [string[]]$Only,

    # 单个文件下载超时（秒）：源站不可达时尽早失败，避免整个脚本卡死
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repoRoot = Split-Path -Parent $PSScriptRoot
$apps = if ($AppsDirectory) { $AppsDirectory } else { Join-Path $repoRoot 'pe\apps' }
$tempRoot = Join-Path $env:TEMP 'firewin-pe-apps'
$userAgent = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 Safari/537.36'
$githubHeaders = @{ 'User-Agent' = 'FireWin'; 'Accept' = 'application/vnd.github+json' }

function Write-Result {
    param([string]$Name, [string]$State, [string]$Detail = '')
    $color = switch ($State) {
        'OK' { 'Green' }
        'SKIP' { 'DarkYellow' }
        default { 'Red' }
    }
    Write-Host ("  [{0}] {1}{2}" -f $State, $Name, $(if ($Detail) { " - $Detail" })) -ForegroundColor $color
}

# 严格模式下直接读不存在的键会抛异常，可选字段统一走这个取值函数
function Get-ToolValue {
    param($Tool, [string]$Key, $Default = $null)
    if ($Tool.ContainsKey($Key)) {
        return $Tool[$Key]
    }

    return $Default
}

# ---------------------------------------------------------------------------
# 下载地址解析
# ---------------------------------------------------------------------------
function Get-GitHubLatestAssetUrl {
    param([string]$Repository, [regex]$AssetPattern)
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases/latest" `
                                 -Headers $githubHeaders -UseBasicParsing -TimeoutSec 60
    $asset = $release.assets | Where-Object { $AssetPattern.IsMatch($_.name) } | Select-Object -First 1
    if ($null -eq $asset) {
        throw "GitHub 最新版中没有匹配的附件（$Repository）"
    }

    return $asset.browser_download_url
}

# winget-pkgs 仓库里每个包都登记了官方下载地址，用它拿最新版直链最省心
function Get-WingetInstallerUrl {
    param([string]$PackageId)
    $parts = $PackageId -split '\.'
    $path = 'manifests/' + $parts[0].Substring(0, 1).ToLower() + '/' + ($parts -join '/')
    $entries = Invoke-RestMethod -Uri "https://api.github.com/repos/microsoft/winget-pkgs/contents/$path" `
                                 -Headers $githubHeaders -UseBasicParsing -TimeoutSec 60
    $versions = @($entries | Where-Object { $_.type -eq 'dir' -and $_.name -match '^\d' } |
        ForEach-Object { $_.name } |
        Sort-Object { try { [version]($_ -replace '[^\d\.]', '') } catch { [version]'0.0' } } -Descending)
    if ($versions.Count -eq 0) {
        throw "winget 清单里没有找到版本目录：$PackageId"
    }

    $version = $versions[0]
    $files = Invoke-RestMethod -Uri "https://api.github.com/repos/microsoft/winget-pkgs/contents/$path/$version" `
                                -Headers $githubHeaders -UseBasicParsing -TimeoutSec 60
    $manifests = @($files | Where-Object { $_.name -like '*.yaml' } |
        Sort-Object { if ($_.name -like '*installer.yaml') { 0 } else { 1 } })
    foreach ($manifest in $manifests) {
        $text = (Invoke-WebRequest -Uri $manifest.download_url -UseBasicParsing -TimeoutSec 60).Content
        $urls = @([regex]::Matches($text, '(?m)^\s*InstallerUrl:\s*(\S+)\s*$') | ForEach-Object { $_.Groups[1].Value })
        $pick = @($urls | Where-Object { $_ -notmatch '(?i)arm64|/x86/|x86\.' }) | Select-Object -First 1
        if (-not $pick) {
            $pick = $urls | Select-Object -First 1
        }
        if ($pick) {
            return $pick
        }
    }

    throw "winget 清单里没有 InstallerUrl：$PackageId"
}

# 官网下载页里抓第一个匹配的链接（BOOTICE / WinNTSetup 这类没有固定直链的工具）
function Get-PageLink {
    param([string]$PageUrl, [string]$Pattern)
    $html = (Invoke-WebRequest -Uri $PageUrl -UseBasicParsing -MaximumRedirection 5 -TimeoutSec 60 `
                               -Headers @{ 'User-Agent' = $userAgent }).Content
    foreach ($match in [regex]::Matches($html, $Pattern)) {
        $link = $match.Groups[1].Value
        if ($link -match '^https?://') {
            return $link
        }
        return ([Uri]::new([Uri]$PageUrl, $link)).AbsoluteUri
    }

    throw "下载页中没有匹配的链接：$PageUrl"
}

# Google 官方的 Chrome for Testing 免安装包（win64 zip 里是 chrome-win64 目录）
function Get-ChromeForTestingUrl {
    $json = Invoke-RestMethod -Uri 'https://googlechromelabs.github.io/chrome-for-testing/last-known-good-versions-with-downloads.json' -UseBasicParsing -TimeoutSec 60
    $asset = $json.channels.Stable.downloads.chrome | Where-Object { $_.platform -eq 'win64' } | Select-Object -First 1
    if ($null -eq $asset) {
        throw 'Chrome for Testing 清单里没有 win64 包'
    }

    return $asset.url
}

# 按工具条目解析出候选下载地址列表（越靠前优先）
function Get-ToolCandidates {
    param($Tool)
    $list = New-Object System.Collections.Generic.List[object]

    # 严格模式下读不存在的键会抛异常，可选字段统一用 ContainsKey 判断
    if ($Tool.ContainsKey('ChromeForTesting') -and $Tool.ChromeForTesting) {
        $list.Add(@{ Url = (Get-ChromeForTestingUrl); Headers = @{} })
    }
    if ($Tool.ContainsKey('WingetId') -and $Tool.WingetId) {
        $list.Add(@{ Url = (Get-WingetInstallerUrl -PackageId $Tool.WingetId); Headers = @{} })
    }
    if ($Tool.ContainsKey('GitHubRepo') -and $Tool.GitHubRepo) {
        $list.Add(@{
            Url     = (Get-GitHubLatestAssetUrl -Repository $Tool.GitHubRepo -AssetPattern ([regex]$Tool.GitHubPattern))
            Headers = @{}
        })
    }
    if ($Tool.ContainsKey('PageUrl') -and $Tool.PageUrl) {
        try {
            $list.Add(@{
                Url     = (Get-PageLink -PageUrl $Tool.PageUrl -Pattern $Tool.PagePattern)
                Headers = @{ 'Referer' = $Tool.PageUrl }
            })
        }
        catch {
            Write-Host "  解析下载页失败：$($_.Exception.Message)" -ForegroundColor DarkYellow
        }
    }
    if ($Tool.ContainsKey('Url') -and $Tool.Url) {
        $referer = if ($Tool.ContainsKey('Referer')) { $Tool.Referer } else { '' }
        foreach ($url in @($Tool.Url)) {
            $list.Add(@{ Url = $url; Headers = $(if ($referer) { @{ 'Referer' = $referer } } else { @{} }) })
        }
    }

    return $list
}

# ---------------------------------------------------------------------------
# 下载 / 解压
# ---------------------------------------------------------------------------
function Expand-ZipKeeping {
    param([string]$ZipPath, [string]$Destination, [string]$SubPath = '')
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $extractRoot = Join-Path $tempRoot ('extract-' + [IO.Path]::GetFileNameWithoutExtension($ZipPath))
    if (Test-Path $extractRoot) {
        Remove-Item -Path $extractRoot -Recurse -Force
    }

    Expand-Archive -Path $ZipPath -DestinationPath $extractRoot -Force
    $source = if ($SubPath) { Join-Path $extractRoot $SubPath } else { $extractRoot }

    # 压缩包内只有一层同名目录时（TestDisk / PeaZip / Chrome / 分区助手都是这种），直接下钻，避免多套一层
    if (-not $SubPath) {
        $entries = @(Get-ChildItem -Path $source -Force)
        if ($entries.Count -eq 1 -and $entries[0].PSIsContainer) {
            $source = $entries[0].FullName
        }
    }
    if (-not (Test-Path $source)) {
        throw "解压包内找不到 $SubPath"
    }

    Copy-Item -Path (Join-Path $source '*') -Destination $Destination -Recurse -Force
}

# rar / 7z 用系统自带 bsdtar 解（Win10 1803 以后都有）
function Expand-ArchiveKeeping {
    param([string]$ArchivePath, [string]$Destination, [string]$SubPath = '')
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $extractRoot = Join-Path $tempRoot ('extract-' + [IO.Path]::GetFileNameWithoutExtension($ArchivePath))
    if (Test-Path $extractRoot) {
        Remove-Item -Path $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null

    & tar.exe -xf $ArchivePath -C $extractRoot
    if ($LASTEXITCODE -ne 0) {
        throw "解压失败（tar 退出码 $LASTEXITCODE）：$ArchivePath"
    }

    $source = if ($SubPath) { Join-Path $extractRoot $SubPath } else { $extractRoot }
    if (-not $SubPath) {
        $entries = @(Get-ChildItem -Path $source -Force)
        if ($entries.Count -eq 1 -and $entries[0].PSIsContainer) {
            $source = $entries[0].FullName
        }
    }
    if (-not (Test-Path $source)) {
        throw "解压包内找不到 $SubPath"
    }

    Copy-Item -Path (Join-Path $source '*') -Destination $Destination -Recurse -Force
}

function Test-HasExe {
    param([string]$Directory)
    return @(Get-ChildItem -Path $Directory -Filter '*.exe' -Recurse -File -ErrorAction SilentlyContinue).Count -gt 0
}

function Get-PackageKind {
    param([string]$Url)
    $extension = [IO.Path]::GetExtension(($Url -split '\?')[0]).ToLowerInvariant()
    switch ($extension) {
        '.zip' { 'zip' }
        '.rar' { 'archive' }
        '.7z' { 'archive' }
        default { 'exe' }
    }
}

New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
New-Item -ItemType Directory -Path $apps -Force | Out-Null
Write-Host "==> 下载 PE 工具到 $apps" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# 工具清单（Directory 必须与 pe\main.cpp 里 kKnownApps 的 folder 一致）
# ---------------------------------------------------------------------------
$tools = @(
    @{
        Name      = 'Everything（Everything 搜索）'
        Directory = 'Everything'
        Url       = 'https://www.voidtools.com/Everything-1.4.1.1028.x64.zip'
    },
    @{
        Name          = 'Dism++（系统精简）'
        Directory     = 'Dism++'
        GitHubRepo    = 'Chuyu-Team/Dism-Multi-language'
        GitHubPattern = '^Dism\+\+.*\.zip$'
    },
    @{
        # SourceForge 直链在部分网络环境会被拦截（403/HTML），失败时请手动从
        # crystalmark.info 下载 portable zip 解压到 pe\apps\CrystalDiskInfo
        Name      = 'CrystalDiskInfo（硬盘健康检测）'
        Directory = 'CrystalDiskInfo'
        Url       = 'https://downloads.sourceforge.net/project/crystaldiskinfo/9.9.5/CrystalDiskInfo9_9_5.zip'
    },
    @{
        Name      = 'ProcessExplorer（任务管理器）'
        Directory = 'ProcExp'
        Url       = 'https://live.sysinternals.com/procexp64.exe'
        ExeName   = 'procexp64.exe'
    },
    @{
        # 官方站点（GPL）：http://www.cdslow.org.ru/en/ntpwedit/
        Name      = 'NTPWEdit（Windows 密码清除）'
        Directory = 'NTPWEdit'
        Url       = 'http://www.cdslow.org.ru/files/ntpwedit/ntpwed07.zip'
    },
    @{
        Name      = 'CPU-Z（硬件检测）'
        Directory = 'CPU-Z'
        Url       = 'https://download.cpuid.com/cpu-z/cpu-z_2.15-en.zip'
    },
    @{
        # 免费版（Lite）直链，官网 download.php 解析出的固定地址；分区 / 数据恢复 / 备份
        Name      = 'DiskGenius Lite（分区工具）'
        Directory = 'DiskGenius'
        Url       = 'https://download_cn.eassos.com/DGLite6201829_x64.zip'
    },
    @{
        # 官方 win64 包内含 testdisk_win.exe / photorec_win.exe；PhotoRec 直接用这个目录
        Name      = 'TestDisk & PhotoRec（数据恢复）'
        Directory = 'TestDisk'
        Url       = 'https://www.cgsecurity.org/testdisk-7.3-WIP.win64.zip'
    },
    @{
        # 原 Process Hacker，进程 / 句柄 / 服务 / 网络的高级查看器
        Name          = 'System Informer（高级进程管理）'
        Directory     = 'SystemInformer'
        GitHubRepo    = 'winsiderss/systeminformer'
        GitHubPattern = '-bin\.zip$'
    },
    @{
        Name      = 'Q-Dir（四窗格文件管理器）'
        Directory = 'Q-Dir'
        Url       = 'https://www.softwareok.com/Download/Q-Dir_Portable_x64.zip'
    },
    @{
        Name      = 'Autoruns（启动项管理）'
        Directory = 'Autoruns'
        Url       = 'https://download.sysinternals.com/files/Autoruns.zip'
    },
    @{
        Name      = 'TCPView（网络连接查看）'
        Directory = 'TCPView'
        Url       = 'https://download.sysinternals.com/files/TCPView.zip'
    },
    @{
        Name      = 'SDelete（安全擦除）'
        Directory = 'SDelete'
        Url       = 'https://download.sysinternals.com/files/SDelete.zip'
    },
    @{
        # 官方 Chrome for Testing（免安装，解压即用）
        Name             = 'Chrome（浏览器）'
        Directory        = 'Chrome'
        ChromeForTesting = $true
    },
    @{
        # 官方 Release 里的 x86_64.exe 为免安装单文件
        Name          = 'RustDesk（远程协助）'
        Directory     = 'RustDesk'
        GitHubRepo    = 'rustdesk/rustdesk'
        GitHubPattern = 'x86_64\.exe$'
    },
    @{
        # ToDesk 官方安装包是 NSIS 自解压，32 位的 ToDesk_Lite.exe 在本 PE（无 WoW64）里跑不了；
        # 真正的 64 位程序在包内 app64.7z，解出来即可免安装运行
        Name          = 'ToDesk（远程协助 x64）'
        Directory     = 'ToDesk'
        Url           = 'https://dl.todesk.com/windows/ToDesk_Setup.exe'
        NestedArchive = 'app64.7z'
    },
    @{
        # 傲梅官网「分区助手绿色版」直链（需要 Referer，否则 404）。
        # 该链接实际返回 7z 格式（扩展名却是 .zip），并且同时打包了 32/64 位，
        # 本 PE 没有 WoW64，只取包内 PA_Green\x64。
        Name      = '傲梅分区助手（绿色版 x64）'
        Directory = 'AOMEI'
        Url       = 'https://www.disktool.cn/go/download/PAGreen.zip'
        Referer   = 'https://www.disktool.cn/download.html'
        Kind      = 'archive'
        SubPath   = 'PA_Green\x64'
    },
    @{
        # 作者站点没有固定直链，从下载页解析；页面挂掉时用下面的历史直链兜底
        Name        = 'BOOTICE（引导维护）'
        Directory   = 'BOOTICE'
        PageUrl     = 'http://www.ipauly.com/'
        PagePattern = '(?i)([^"''<>\s]*BOOTICE[\w\.\-]*\.(?:zip|rar|7z))'
        Url         = @(
            'http://www.ipauly.com/download/BOOTICE_v1.3.4.0_en.zip',
            'https://www.ipauly.com/download/BOOTICE_v1.3.4.0_en.zip'
        )
        Referer     = 'http://www.ipauly.com/'
    },
    @{
        # 官方包是 rar，用 bsdtar 解开
        Name        = 'WinNTSetup（系统安装器）'
        Directory   = 'WinNTSetup'
        PageUrl     = 'https://www.winntsetup.com/'
        PagePattern = '(?i)([^"''<>\s]*WinNTSetup[\w\.\-]*\.(?:rar|zip|7z))'
    },
    @{
        # 官网下载页里的 hwi64_xxx.exe 是免安装单文件
        Name        = 'HWiNFO（硬件信息）'
        Directory   = 'HWiNFO'
        PageUrl     = 'https://www.hwinfo.com/download/'
        PagePattern = '([^"''<>\s]*hwi64_\d+\.exe)'
    },
    @{
        # winget 清单里登记的官方 CDN 地址（免安装单文件）
        Name      = 'GPU-Z（显卡检测）'
        Directory = 'GPU-Z'
        WingetId  = 'TechPowerUp.GPU-Z'
    }
)

foreach ($tool in $tools) {
    if ($Only -and ($Only -notcontains $tool.Directory)) {
        continue
    }

    $target = Join-Path $apps $tool.Directory
    if ((-not $Force) -and (Test-Path $target) -and (Test-HasExe $target)) {
        Write-Result $tool.Name 'SKIP' '已存在'
        continue
    }

    try {
        $candidates = @(Get-ToolCandidates -Tool $tool)
        $fallback = Get-ToolValue $tool 'UrlFallback'
        if ($fallback) {
            foreach ($url in @($fallback)) {
                $candidates += @{ Url = $url; Headers = @{} }
            }
        }
        if ($candidates.Count -eq 0) {
            throw '没有可用的下载地址'
        }

        $downloaded = $null
        $lastError = $null
        foreach ($candidate in $candidates) {
            try {
                $fileName = [IO.Path]::GetFileName(($candidate.Url -split '\?')[0])
                if (-not $fileName) {
                    $fileName = 'download.bin'
                }
                $downloadPath = Join-Path $tempRoot $fileName
                Write-Host "  下载 $($candidate.Url)" -ForegroundColor DarkGray
                Invoke-WebRequest -Uri $candidate.Url -OutFile $downloadPath -UseBasicParsing `
                                  -Headers $candidate.Headers -UserAgent $userAgent -TimeoutSec $TimeoutSeconds
                $downloaded = $downloadPath
                break
            }
            catch {
                $lastError = $_.Exception.Message
                Write-Host "  失败：$lastError" -ForegroundColor DarkYellow
            }
        }
        if (-not $downloaded) {
            throw $lastError
        }

        # NSIS 自解压安装包（ToDesk）：真正的程序在内嵌的 app64.7z 里
        $nested = Get-ToolValue $tool 'NestedArchive' ''
        if ($nested) {
            $sevenZip = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\7za.exe'
            if (-not (Test-Path $sevenZip)) {
                throw "缺少 $sevenZip，无法解开内嵌的 $nested"
            }

            $nestedRoot = Join-Path $tempRoot ('nested-' + [IO.Path]::GetFileNameWithoutExtension($downloaded))
            if (Test-Path $nestedRoot) {
                Remove-Item -Path $nestedRoot -Recurse -Force
            }
            New-Item -ItemType Directory -Path $nestedRoot -Force | Out-Null
            & $sevenZip x $downloaded "-o$nestedRoot" -y | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "解压安装包失败：$downloaded"
            }

            $inner = Get-ChildItem -Path $nestedRoot -Filter $nested -Recurse -File | Select-Object -First 1
            if ($null -eq $inner) {
                throw "安装包内找不到 $nested"
            }

            $innerRoot = Join-Path $tempRoot ('nested-inner-' + [IO.Path]::GetFileNameWithoutExtension($nested))
            if (Test-Path $innerRoot) {
                Remove-Item -Path $innerRoot -Recurse -Force
            }
            New-Item -ItemType Directory -Path $target -Force | Out-Null
            New-Item -ItemType Directory -Path $innerRoot -Force | Out-Null
            & $sevenZip x $inner.FullName "-o$innerRoot" -y | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "解压 $nested 失败"
            }

            Copy-Item -Path (Join-Path $innerRoot '*') -Destination $target -Recurse -Force
            # 安装器 / 自动更新 / 崩溃上报在 PE 里用不上，删掉以控制 boot.wim 体积
            foreach ($junk in @('uninst.exe', 'updater.exe', 'CrashReport.exe', 'codec_checker.exe')) {
                $junkPath = Join-Path $target $junk
                if (Test-Path $junkPath) {
                    Remove-Item -Path $junkPath -Force
                }
            }
            if (-not (Test-HasExe $target)) {
                throw '解压成功但没有 exe'
            }

            Write-Result $tool.Name 'OK' $tool.Directory
            continue
        }

        $kind = if (Get-ToolValue $tool 'Kind') { Get-ToolValue $tool 'Kind' } else { Get-PackageKind -Url $downloaded }
        switch ($kind) {
            'exe' {
                New-Item -ItemType Directory -Path $target -Force | Out-Null
                $exeName = Get-ToolValue $tool 'ExeName'
                if (-not $exeName) {
                    $exeName = Split-Path -Leaf $downloaded
                }
                Copy-Item -Path $downloaded -Destination (Join-Path $target $exeName) -Force
            }
            'archive' {
                Expand-ArchiveKeeping -ArchivePath $downloaded -Destination $target -SubPath (Get-ToolValue $tool 'SubPath' '')
            }
            default {
                Expand-ZipKeeping -ZipPath $downloaded -Destination $target -SubPath (Get-ToolValue $tool 'SubPath' '')
            }
        }

        if (-not (Test-HasExe $target)) {
            throw '下载解压成功但没有 exe'
        }

        Write-Result $tool.Name 'OK' $tool.Directory
    }
    catch {
        Write-Result $tool.Name 'FAIL' $_.Exception.Message
    }
}

Write-Host "`n==> 完成。构建 PE 时 pe\apps 会被整体复制进 boot.wim 的 \Tools\Apps。" -ForegroundColor Cyan
Write-Host "    也可直接复制到启动盘 \Tools\Apps\，启动器运行时会自动扫描。" -ForegroundColor Cyan
