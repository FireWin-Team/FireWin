using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using FireWin.Services;

namespace FireWin;

/// <summary>
/// 制作 PE 窗口，两种输出方式（同一套内置 PE）：
///   1. 写入 U 盘：清盘分区 → 写 PE → 生成独立 BCD → 写 BIOS 引导代码（<see cref="UsbBootService"/>）；
///   2. 生成 ISO：内置 ISO9660 + El Torito 写入器，UEFI / Legacy BIOS 双引导（<see cref="IsoImageService"/>）。
/// </summary>
public partial class UsbWindow : Window
{
    private enum MediaMode
    {
        Usb,
        Iso,
    }

    private MediaMode _mode = MediaMode.Usb;
    private string? _optionalImagePath;
    private bool _busy;

    public UsbWindow()
    {
        InitializeComponent();

        IsoPathBox.Text = Path.Combine(AppContext.BaseDirectory, "FireWinPE.iso");
        IsoPathBox.TextChanged += (_, _) => UpdateCreateButtonState();
        UsbModeRadio.IsChecked = true;   // 触发 SetMode(Usb)

        Loaded += async (_, _) =>
        {
            AppendLog("FireWin 启动盘制作工具已启动。");
            await RefreshUsbListAsync();
        };
    }

    #region 模式切换

    private void ModeRadio_Checked(object sender, RoutedEventArgs e)
    {
        if (_busy || IsoPanel is null)
        {
            return;
        }

        SetMode(IsoModeRadio.IsChecked == true ? MediaMode.Iso : MediaMode.Usb);
    }

    private void SetMode(MediaMode mode)
    {
        _mode = mode;
        bool usb = mode == MediaMode.Usb;

        UsbPanel.Visibility = usb ? Visibility.Visible : Visibility.Collapsed;
        IsoPanel.Visibility = usb ? Visibility.Collapsed : Visibility.Visible;

        TitleText.Text = usb ? "制作 PE 启动盘（U 盘）" : "生成 PE ISO 镜像";
        CreateButton.Content = usb ? "全新制作" : "生成 ISO";
        ImageHintText.Text = usb
            ? @"支持 ISO / WIM / ESD。镜像会复制到数据分区 \FWIMG 目录；从 U 盘启动 PE 后点「安装系统」即可选择安装，不需要联网和光驱。"
            : @"支持 ISO / WIM / ESD。镜像会作为 \FWIMG 目录写入 ISO（ISO 内单个文件上限 4 GB），启动 PE 后点「安装系统」即可安装。";

        UpdateCreateButtonState();
    }

    private void UpdateCreateButtonState()
    {
        bool usb = _mode == MediaMode.Usb;
        bool enabled = !_busy && (usb
            ? UsbList.SelectedItem is not null
            : !string.IsNullOrWhiteSpace(IsoPathBox.Text));
        CreateButton.IsEnabled = enabled;

        // 「升级启动盘」只在 U 盘模式下、且选中磁盘确实是 FireWin 启动盘时可用
        UpgradeButton.IsEnabled = !_busy && usb &&
                                  UsbList.SelectedItem is UsbDiskInfo disk &&
                                  UsbBootService.CanUpgradeInPlace(disk);
        UpgradeButton.Visibility = usb ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>按选中磁盘判断能否原地升级，并联动「升级启动盘」按钮与提示文案。</summary>
    private void UpdateUpgradeState()
    {
        if (_busy)
        {
            return;
        }

        bool canUpgrade = UsbList.SelectedItem is UsbDiskInfo disk && UsbBootService.CanUpgradeInPlace(disk);

        if (canUpgrade)
        {
            UpgradeHintText.Text = "已检测到 FireWin 启动盘：点「升级启动盘」只替换 PE 文件，数据分区与镜像保持不动；点「全新制作」则重新分区并清空数据。";
            UpgradeHintText.Foreground = (System.Windows.Media.Brush)FindResource("MutedBrush");
        }
        else if (UsbList.SelectedItem is not null)
        {
            UpgradeHintText.Text = "该 U 盘还不是 FireWin 启动盘，只能「全新制作」（会重新分区并清空数据）；升级仅适用于已用它制作过的启动盘。";
            UpgradeHintText.Foreground = (System.Windows.Media.Brush)FindResource("MutedBrush");
        }
        else
        {
            UpgradeHintText.Text = "「全新制作」会重新分区并清空 U 盘；已有 FireWin 启动盘时可用「升级启动盘」，只更新 PE，保留数据分区与镜像。";
            UpgradeHintText.Foreground = (System.Windows.Media.Brush)FindResource("MutedBrush");
        }

        UpdateCreateButtonState();
    }

    #endregion

    #region U 盘列表

    private async Task RefreshUsbListAsync()
    {
        SetBusy(true);
        try
        {
            var disks = await Task.Run(UsbBootService.ListUsbDisks);
            UsbList.ItemsSource = disks;
            UsbList.SelectedItem = disks.FirstOrDefault();

            AppendLog(disks.Count > 0
                ? $"检测到 {disks.Count} 个可移动磁盘。"
                : "未检测到可移动磁盘（U 盘）。");
        }
        finally
        {
            SetBusy(false);
        }

        UpdateUpgradeState();
    }

    private async void RefreshButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_busy)
        {
            await RefreshUsbListAsync();
        }
    }

    private void UsbList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        UpdateUpgradeState();
    }

    #endregion

    #region 选择文件

    private void BrowseImage_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Title = "选择要随介质写入的系统镜像（可选）",
            Filter = "Windows 系统镜像|*.iso;*.wim;*.esd;*.swm|所有文件|*.*",
        };
        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        _optionalImagePath = dialog.FileName;
        ImagePathBox.Text = dialog.FileName;
    }

    private void BrowseIso_Click(object sender, RoutedEventArgs e)
    {
        string current = (IsoPathBox.Text ?? string.Empty).Trim().Trim('"');
        string directory = Path.GetDirectoryName(current) ?? string.Empty;

        var dialog = new SaveFileDialog
        {
            Title = "选择 ISO 输出位置",
            Filter = "ISO 镜像|*.iso",
            DefaultExt = ".iso",
            AddExtension = true,
            FileName = string.IsNullOrEmpty(current) ? "FireWinPE.iso" : Path.GetFileName(current),
            InitialDirectory = Directory.Exists(directory) ? directory : AppContext.BaseDirectory,
        };
        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        IsoPathBox.Text = dialog.FileName;
    }

    #endregion

    #region 开始制作

    private async void CreateButton_Click(object sender, RoutedEventArgs e)
    {
        if (_busy)
        {
            return;
        }

        if (_mode == MediaMode.Iso)
        {
            await CreateIsoAsync();
        }
        else
        {
            await CreateUsbAsync(upgrade: false);
        }
    }

    private async void UpgradeButton_Click(object sender, RoutedEventArgs e)
    {
        if (_busy)
        {
            return;
        }

        await CreateUsbAsync(upgrade: true);
    }

    private async Task CreateUsbAsync(bool upgrade)
    {
        if (UsbList.SelectedItem is not UsbDiskInfo disk)
        {
            return;
        }

        // 资源与权限前置校验
        if (!IsAdministrator())
        {
            MessageBox.Show(this,
                "重新分区与写引导需要管理员权限，请右键程序选择“以管理员身份运行”。",
                "需要管理员权限", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (!BundledPayload.TryGetPePayload(out string missing))
        {
            MessageBox.Show(this,
                $"内置 PE 资源不完整（缺 {missing}），无法{(upgrade ? "升级" : "制作")}启动盘。\n请使用完整分发的程序包。",
                "内置资源缺失", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        MessageBoxResult confirm = MessageBox.Show(this,
            upgrade
                ? $"即将升级 {disk.Drive}（物理磁盘 {disk.DiskNumber}，{DeployService.FormatBytes(disk.DiskTotalBytes)}）上的 FireWin PE：\n\n" +
                  "1. 只替换启动分区里的 PE 文件（boot.wim / 引导文件 / BCD）\n" +
                  "2. 不重新分区、不清空数据分区，\\FWIMG 里已有的镜像保留\n" +
                  (_optionalImagePath is null ? "" : "3. 把新选的系统镜像复制到 \\FWIMG（同名文件会被覆盖）\n") +
                  "\n升级过程中请勿拔出 U 盘。确认继续吗？"
                : $"即将对 {disk.Drive}（物理磁盘 {disk.DiskNumber}，{DeployService.FormatBytes(disk.DiskTotalBytes)}）执行：\n\n" +
                  "1. 清除 U 盘全部数据并重新分区（隐藏启动分区 + 数据分区）\n" +
                  "2. 写入内置 WinPE（UEFI / BIOS 双引导）\n" +
                  "3. 在数据分区创建 FWIMG 镜像目录\n" +
                  (_optionalImagePath is null ? "" : "4. 复制系统镜像到 FWIMG\n") +
                  "\n确认继续吗？",
            upgrade ? "确认升级启动盘" : "确认制作启动盘",
            MessageBoxButton.OKCancel, MessageBoxImage.Warning);
        if (confirm != MessageBoxResult.OK)
        {
            return;
        }

        SetBusy(true);
        LogExpander.IsExpanded = true;
        BeginBuildProgress();
        var progress = new Progress<string>(ReportBuildProgress);
        var percentProgress = new Progress<int>(ReportBuildPercent);

        try
        {
            await Task.Run(() => upgrade
                ? UsbBootService.UpgradeBootDiskAsync(
                    disk, _optionalImagePath, progress, percentProgress, CancellationToken.None)
                : UsbBootService.CreateBootDiskAsync(
                    disk, _optionalImagePath, progress, percentProgress, CancellationToken.None));
        }
        catch (Exception ex)
        {
            AppendLog((upgrade ? "升级失败：" : "制作失败：") + ex.Message);
            FailBuildProgress((upgrade ? "升级失败" : "制作失败") + "，详见运行日志。");
            MessageBox.Show(this, ex.Message, upgrade ? "升级失败" : "制作失败",
                MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }
        finally
        {
            SetBusy(false);
            await RefreshUsbListAsync();
        }

        MessageBox.Show(this,
            upgrade
                ? "PE 启动盘升级完成。\n\n" +
                  "• 启动分区内的 PE 已更新，数据分区与镜像均未改动；\n" +
                  "• 重新从 U 盘启动即可使用新版 PE。"
                : "PE 启动盘制作完成。\n\n" +
                  "• 开机按启动菜单键（F12 / F8 / Esc）选择 U 盘启动，即可进入 FireWin PE；\n" +
                  "• 启动分区已隐藏，资源管理器中只会看到数据分区；\n" +
                  (_optionalImagePath is null
                      ? "• 把 install.wim / install.esd（或 ISO）放进数据分区 \\FWIMG 目录，PE 里点「安装系统」即可安装。"
                      : "• 系统镜像已写入数据分区 \\FWIMG，PE 里点「安装系统」即可安装。"),
            upgrade ? "升级完成" : "制作完成", MessageBoxButton.OK, MessageBoxImage.Information);
    }

    private async Task CreateIsoAsync()
    {
        string output = (IsoPathBox.Text ?? string.Empty).Trim().Trim('"');
        if (output.Length == 0)
        {
            MessageBox.Show(this, "请先选择 ISO 输出位置。", "缺少输出路径",
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (!Path.IsPathRooted(output))
        {
            output = Path.Combine(AppContext.BaseDirectory, output);
        }

        if (!string.Equals(Path.GetExtension(output), ".iso", StringComparison.OrdinalIgnoreCase))
        {
            output += ".iso";
        }

        output = Path.GetFullPath(output);
        IsoPathBox.Text = output;

        // 复用本机安装介质的 BCD 时不需要管理员；否则要用 bcdedit 新建引导配置
        if (!IsAdministrator() && !IsoImageService.HostBcdAvailable)
        {
            MessageBox.Show(this,
                "生成引导配置需要管理员权限，请右键程序选择“以管理员身份运行”。",
                "需要管理员权限", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (!BundledPayload.TryGetPePayload(out string missing))
        {
            MessageBox.Show(this,
                $"内置 PE 资源不完整（缺 {missing}），无法生成 ISO。\n请使用完整分发的程序包。",
                "内置资源缺失", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        MessageBoxResult confirm = MessageBox.Show(this,
            $"将生成可启动的 PE ISO：\n{output}\n\n" +
            "• 不改动任何磁盘，只写入这一个文件；\n" +
            "• 支持 UEFI 与 Legacy BIOS 双引导；\n" +
            (_optionalImagePath is null
                ? "• 未附带系统镜像。\n\n"
                : $"• 附带系统镜像：\n  {_optionalImagePath}\n\n") +
            "确认继续吗？",
            "确认生成 ISO", MessageBoxButton.OKCancel, MessageBoxImage.Question);
        if (confirm != MessageBoxResult.OK)
        {
            return;
        }

        SetBusy(true);
        LogExpander.IsExpanded = true;
        BeginBuildProgress();
        var progress = new Progress<string>(ReportBuildProgress);
        var percentProgress = new Progress<int>(ReportBuildPercent);

        string result;
        try
        {
            result = await Task.Run(() =>
                IsoImageService.CreatePeIsoAsync(output, _optionalImagePath, progress, percentProgress, CancellationToken.None));
        }
        catch (Exception ex)
        {
            AppendLog("生成失败：" + ex.Message);
            FailBuildProgress("生成失败，详见运行日志。");
            MessageBox.Show(this, ex.Message, "生成失败", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }
        finally
        {
            SetBusy(false);
        }

        MessageBoxResult openFolder = MessageBox.Show(this,
            $"ISO 生成完成：\n{result}\n\n" +
            "可刻录光盘、挂载到虚拟机（VMware / Hyper-V / VirtualBox），\n" +
            "或用 U 盘写入工具（如 Rufus）写入移动介质后启动。\n\n是否打开所在文件夹？",
            "生成完成", MessageBoxButton.YesNo, MessageBoxImage.Information);
        if (openFolder == MessageBoxResult.Yes)
        {
            RevealInExplorer(result);
        }
    }

    #endregion

    private void CloseButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_busy)
        {
            Close();
        }
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        if (busy)
        {
            UpgradeButton.IsEnabled = false;
        }

        UsbModeRadio.IsEnabled = !busy;
        IsoModeRadio.IsEnabled = !busy;
        RefreshButton.IsEnabled = !busy;
        BrowseButton.IsEnabled = !busy;
        BrowseIsoButton.IsEnabled = !busy;
        IsoPathBox.IsReadOnly = busy;
        UpdateCreateButtonState();
    }

    #region 制作进度

    /// <summary>开始新的制作任务：显示进度区并归零。</summary>
    private void BeginBuildProgress()
    {
        ProgressPanel.Visibility = Visibility.Visible;
        BuildProgressBar.Value = 0;
        ProgressPercentText.Text = "0%";
        ProgressStepText.Text = "正在准备…";
    }

    /// <summary>进度文本既写日志，也作为进度条上方的当前步骤显示。</summary>
    private void ReportBuildProgress(string message)
    {
        AppendLog(message);
        ProgressStepText.Text = message.Trim();
    }

    /// <summary>更新百分比；进度只前进不后退（各阶段映射可能有细微回退）。</summary>
    private void ReportBuildPercent(int percent)
    {
        double value = Math.Clamp(percent, 0, 100);
        if (value > BuildProgressBar.Value)
        {
            BuildProgressBar.Value = value;
        }

        ProgressPercentText.Text = $"{(int)BuildProgressBar.Value}%";
    }

    private void FailBuildProgress(string message)
    {
        ProgressStepText.Text = message;
    }

    #endregion

    private void AppendLog(string message)
    {
        if (!Dispatcher.CheckAccess())
        {
            Dispatcher.Invoke(() => AppendLog(message));
            return;
        }

        LogBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        LogBox.ScrollToEnd();
    }

    private static void RevealInExplorer(string path)
    {
        try
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"")
            {
                UseShellExecute = true,
            });
        }
        catch
        {
            // 打开资源管理器失败不影响生成结果
        }
    }

    private static bool IsAdministrator()
    {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }
}
