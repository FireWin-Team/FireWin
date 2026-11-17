# FireWin 宣传图自动截图脚本
# 阶段 1：启动应用拍欢迎页
# 阶段 2：带 --demo-image 启动（自动填路径并识别），UIA 驱动翻页拍剩余 4 张
# 前提：以管理员运行（应用带 requireAdministrator）
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$exe     = "C:\Users\almmo\Desktop\openwininstall\dist\desktop\FireWin.exe"
$outDir  = "C:\Users\almmo\Desktop\openwininstall\docs\images"
$demoWim = "$env:USERPROFILE\Downloads\Win11_24H2_x64_zh-CN.wim"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (-not (Test-Path $demoWim)) { throw "演示镜像不存在：$demoWim" }

Add-Type -Namespace Native -Name Win32Shot -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
public struct RECT { public int Left, Top, Right, Bottom; }
'@

function Stop-App {
    Get-Process -Name FireWin -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
}

function Start-App([string[]]$args2) {
    Stop-App
    if ($args2.Count -gt 0) { Start-Process $exe -ArgumentList $args2 } else { Start-Process $exe }
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $deadline) {
        $p = Get-Process -Name FireWin -ErrorAction SilentlyContinue |
             Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($p) { return $p }
        Start-Sleep -Milliseconds 500
    }
    throw "等待主窗口超时"
}

function Shot([string]$path) {
    # PrintWindow(PW_RENDERFULLCONTENT) 直接渲染窗口自身内容：不含 DWM 阴影，不受遮挡影响
    $p = Get-Process -Name FireWin | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    Start-Sleep -Milliseconds 400
    $r = New-Object 'Native.Win32Shot+RECT'
    [Native.Win32Shot]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap(($r.Right - $r.Left), ($r.Bottom - $r.Top))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [Native.Win32Shot]::PrintWindow($p.MainWindowHandle, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "已截图：$path"
}

function Invoke-Button([string]$automationId) {
    # 翻页后元素才进入 UIA 树（Collapsed 页面元素不在树中），因此轮询等待
    $p = Get-Process -Name FireWin | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    $root = [Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
    $cond = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty, $automationId)
    $el = $null
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and -not $el) {
        $el = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $cond)
        if (-not $el) { Start-Sleep -Milliseconds 400 }
    }
    if (-not $el) { throw "未找到按钮：$automationId" }
    ($el.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern)).Invoke()
}

# ---- 阶段 1：欢迎页 ----
$null = Start-App @()
Start-Sleep -Seconds 3   # 等欢迎页状态检测完成
Shot "$outDir\01-welcome.png"

# ---- 阶段 2：镜像页 + 后续翻页 ----
$null = Start-App @("--demo-image", $demoWim)
Start-Sleep -Seconds 4   # 等镜像识别完成（结果卡片出现）
Shot "$outDir\02-image.png"

Invoke-Button 'NextButton'   # 识别镜像版本
Start-Sleep -Seconds 4
Shot "$outDir\03-versions.png"

Invoke-Button 'NextButton'   # 选择分区
Start-Sleep -Seconds 1
Shot "$outDir\04-partition.png"

Invoke-Button 'NextButton'   # 确认部署
Start-Sleep -Seconds 1
Shot "$outDir\05-confirm.png"

Stop-App
Write-Host "全部截图完成。"
