using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Principal;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.Win32;
using FireWin.Services;

namespace FireWin;

/// <summary>
/// 主窗口：四步部署向导（镜像 → 版本 → 分区 → 部署）。
/// 环境检测只在欢迎页做一次只读展示（权限 / 内置资源 / 固件类型），不再有独立的检测面板。
/// 实际部署：复制镜像与 PE 到 C:\FireWinPE → 写 deploy.json → 写一次性引导 → 重启进 PE 自动安装。
/// </summary>
public partial class MainWindow : Window
{
    private readonly WimInitService _wim = new();

    private string? _isoPath;               // 镜像来自 ISO 时记录原始 ISO，便于部署完成后卸载
    private string? _wimPath;               // 实际参与部署的 install.wim 路径
    private List<WimImageInfo> _images = new();
    private WimImageInfo? _selectedImage;
    private PartitionInfo? _targetPartition;
    private FirmwareType _firmware;
    private int _page;
    private bool _busy;

    public MainWindow()
    {
        InitializeComponent();
        Loaded += async (_, _) => await InitWelcomePageAsync();
        Closed += (_, _) =>
        {
            _wim.Dispose();
            if (_isoPath is not null)
            {
                DeployService.TryDismountIso(_isoPath);
            }
        };
    }

    // ---------------- 欢迎页 ----------------

    /// <summary>打开「制作 PE 启动盘」窗口。</summary>
    private void UsbEntryCard_Click(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (_busy)
        {
            return;
        }

        new UsbWindow { Owner = this }.Show();
    }

    private async Task InitWelcomePageAsync()
    {
        AppendLog("FireWin 部署向导已启动。");

        // 权限：requireAdministrator 下恒为管理员，但做兜底显示
        bool admin = IsAdministrator();
        AdminValue.Text = admin ? "已以管理员运行，可写入系统引导" : "未获得管理员权限";
        AdminIcon.Text = admin ? "✓" : "!";
        AdminIcon.Foreground = admin ? OkBrush : WarnBrush;
        AdminValue.Foreground = admin ? OkBrush : WarnBrush;

        // 内置 PE 资源
        bool payloadReady = BundledPayload.TryGetPePayload(out string missing);
        PayloadValue.Text = payloadReady
            ? "已就绪（随包内置 WinPE，无需联网）"
            : $"不完整（缺 {missing}）";
        PayloadIcon.Text = payloadReady ? "✓" : "!";
        PayloadIcon.Foreground = payloadReady ? OkBrush : WarnBrush;
        PayloadValue.Foreground = payloadReady ? OkBrush : WarnBrush;

        // 固件类型
        await Task.Run(() => { try { _firmware = BcdBootService.DetectFirmwareType(); } catch { _firmware = FirmwareType.Unknown; } });
        string firmwareText = _firmware switch
        {
            FirmwareType.Uefi => "UEFI（引导使用 winload.efi）",
            FirmwareType.Bios => "Legacy BIOS（引导使用 winload.exe）",
            _ => "未知（将按 UEFI 处理）",
        };
        FirmwareValue.Text = firmwareText;
        FirmwareValue.Foreground = _firmware == FirmwareType.Unknown ? WarnBrush : OkBrush;

        AppendLog($"运行环境就绪：{firmwareText.Split('（')[0]}，内置 PE {(payloadReady ? "已就绪" : "缺失")}。");
    }

    // ---------------- 页面切换 ----------------

    private void GoToPage(int page)
    {
        _page = page;
        PageWelcome.Visibility = page == 0 ? Visibility.Visible : Visibility.Collapsed;
        PageImage.Visibility = page == 1 ? Visibility.Visible : Visibility.Collapsed;
        PageVersion.Visibility = page == 2 ? Visibility.Visible : Visibility.Collapsed;
        PagePartition.Visibility = page == 3 ? Visibility.Visible : Visibility.Collapsed;
        PageConfirm.Visibility = page == 4 ? Visibility.Visible : Visibility.Collapsed;
        PageDone.Visibility = page == 5 ? Visibility.Visible : Visibility.Collapsed;

        BackButton.Visibility = page is 0 or 5 ? Visibility.Hidden : Visibility.Visible;
        NextButton.Visibility = page == 5 ? Visibility.Collapsed : Visibility.Visible;
        NextButton.Content = page switch
        {
            0 => "开始部署",
            1 => "识别镜像版本",
            2 => "下一步：选择分区",
            3 => "下一步：确认部署",
            4 => "开始部署",
            _ => "下一步",
        };

        UpdateStepIndicator(page);

        if (page == 3)
        {
            LoadPartitions();
        }

        if (page == 4)
        {
            FillConfirmSummary();
        }
    }

    private void UpdateStepIndicator(int page)
    {
        // page: 0 欢迎未开始；1..3 对应步骤 1..3；4/5 均为步骤 4
        int step = page switch { <= 0 => -1, 1 => 0, 2 => 1, 3 => 2, >= 4 => 3 };

        Border[] dots = { StepDot0, StepDot1, StepDot2, StepDot3 };
        TextBlock[] nums = { StepNum0, StepNum1, StepNum2, StepNum3 };
        TextBlock[] labels = { StepLabel0, StepLabel1, StepLabel2, StepLabel3 };

        for (int i = 0; i < 4; i++)
        {
            bool active = i <= step;
            dots[i].Background = active ? AccentBrush : new SolidColorBrush(Color.FromRgb(0x2E, 0x2E, 0x36));
            nums[i].Foreground = active ? Brushes.White : MutedBrush;
            labels[i].Foreground = active ? TextBrush : MutedBrush;
        }
    }

    // ---------------- 第一步：镜像 ----------------

    /// <summary>内部演示入口：跳过文件对话框，直接识别指定镜像（供自动化截图使用）。</summary>
    public async void DemoSelectImage(string path)
    {
        try
        {
            GoToPage(1);
            await ResolveImageAsync(path);
        }
        catch
        {
            // 演示失败保持界面现状
        }
    }

    private async void BrowseImage_Click(object sender, RoutedEventArgs e)
    {
        if (_busy)
        {
            return;
        }

        var dialog = new OpenFileDialog
        {
            Title = "选择 Windows 系统镜像",
            Filter = "Windows 系统镜像|*.iso;*.wim;*.esd;*.swm|所有文件|*.*",
        };
        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        await ResolveImageAsync(dialog.FileName);
    }

    private async Task ResolveImageAsync(string path)
    {
        SetBusy(true);
        ImageResultCard.Visibility = Visibility.Collapsed;
        ImageErrorCard.Visibility = Visibility.Collapsed;
        ImagePathBox.Text = path;
        _wimPath = null;
        _isoPath = null;

        try
        {
            string ext = Path.GetExtension(path).ToLowerInvariant();
            if (ext == ".iso")
            {
                AppendLog($"正在挂载 ISO 镜像：{path}");
                IsoMountResult mount = await Task.Run(() => DeployService.MountIsoAsync(path));
                _isoPath = path;
                _wimPath = mount.InstallWimPath;
                AppendLog($"ISO 已挂载到 {mount.DriveLetter}:，定位到 {mount.InstallWimPath}");

                ImageResultTitle.Text = "✓ ISO 已挂载并定位到系统镜像";
                ImageResultDetail.Text = $"挂载盘符 {mount.DriveLetter}:　镜像文件：{mount.InstallWimPath}";
            }
            else
            {
                if (!File.Exists(path))
                {
                    throw new DeployException($"找不到镜像文件：{path}");
                }

                _wimPath = path;
                ImageResultTitle.Text = "✓ 已识别系统镜像";
                ImageResultDetail.Text = path;
            }

            ImageResultCard.Visibility = Visibility.Visible;
            NextButton.IsEnabled = true;
        }
        catch (Exception ex)
        {
            ImageErrorDetail.Text = ex.Message;
            ImageErrorCard.Visibility = Visibility.Visible;
            AppendLog("镜像识别失败：" + ex.Message);
            NextButton.IsEnabled = false;
        }
        finally
        {
            SetBusy(false);
        }
    }

    // ---------------- 第二步：版本 ----------------

    private async Task EnumerateImagesAsync()
    {
        SetBusy(true);
        try
        {
            string wimPath = _wimPath!;
            AppendLog($"正在读取镜像版本信息（首次读取大镜像可能需要数十秒）…");
            List<WimImageInfo> images = await Task.Run(() =>
            {
                _wim.EnsureInitialized();
                return DeployService.ListWimImages(wimPath);
            });

            _images = images;
            VersionList.ItemsSource = images;
            VersionList.SelectedIndex = 0;
            AppendLog($"共识别到 {images.Count} 个系统版本。");
            GoToPage(2);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "读取镜像版本失败：" + ex.Message, "读取失败",
                MessageBoxButton.OK, MessageBoxImage.Error);
            AppendLog("读取镜像版本失败：" + ex.Message);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private void VersionList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_busy && _page == 2)
        {
            NextButton.IsEnabled = VersionList.SelectedItem is not null;
        }
    }

    // ---------------- 第三步：分区 ----------------

    private void LoadPartitions()
    {
        List<PartitionInfo> partitions = DeployService.GetFixedPartitions();
        PartitionList.ItemsSource = partitions;
        PartitionList.SelectedItem = partitions.FirstOrDefault(p => !p.IsSystemDrive)
                                     ?? partitions.FirstOrDefault();
        NextButton.IsEnabled = PartitionList.SelectedItem is not null;
    }

    private void PartitionList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (PartitionList.SelectedItem is PartitionInfo { IsSystemDrive: true })
        {
            // 系统盘不可作为目标（会被格式化导致当前系统损坏）
            MessageBox.Show(this,
                "当前系统盘不可作为安装目标——部署会清空该分区的全部数据。",
                "无法选择系统盘", MessageBoxButton.OK, MessageBoxImage.Warning);
            PartitionList.SelectedItem = e.RemovedItems.Count > 0 ? e.RemovedItems[0] : null;
            if (PartitionList.SelectedItem is null)
            {
                PartitionList.SelectedItem = ((List<PartitionInfo>)PartitionList.ItemsSource).FirstOrDefault(p => !p.IsSystemDrive);
            }
            return;
        }

        if (!_busy && _page == 3)
        {
            NextButton.IsEnabled = PartitionList.SelectedItem is not null;
        }
    }

    // ---------------- 第四步：确认 + 部署 ----------------

    private void FillConfirmSummary()
    {
        var target = (PartitionInfo)PartitionList.SelectedItem!;
        _targetPartition = target;

        SummaryImagePath.Text = _wimPath ?? string.Empty;
        SummaryImageVersion.Text = $"{_selectedImage!.Name}（索引 {_selectedImage.Index}）";
        SummaryTarget.Text =
            $"{target.Drive} {target.VolumeLabel}（总 {DeployService.FormatBytes(target.TotalBytes)}，可用 {DeployService.FormatBytes(target.FreeBytes)}）";
        SummaryFirmware.Text = _firmware == FirmwareType.Bios ? "Legacy BIOS" : "UEFI";
        SummaryStaging.Text = @"C:\FireWinPE（WinPE 引导文件与系统镜像的存放位置）";
    }

    private async Task DeployAsync()
    {
        var target = _targetPartition!;
        MessageBoxResult confirm = MessageBox.Show(this,
            $"即将对 {target.Drive} 部署 {_selectedImage!.Name}：\n\n" +
            "1. 复制系统镜像到 C:\\FireWinPE\n" +
            "2. 组装内置 WinPE 并写入一次性引导\n" +
            "3. 重启后自动进入安装流程\n\n" +
            "确认继续吗？",
            "确认部署", MessageBoxButton.OKCancel, MessageBoxImage.Warning);
        if (confirm != MessageBoxResult.OK)
        {
            return;
        }

        SetBusy(true);
        LogExpander.IsExpanded = true;
        var progress = new Progress<string>(AppendLog);

        try
        {
            string staging = BcdBootService.DefaultStagingDirectory;
            string systemDrive = Path.GetPathRoot(Environment.SystemDirectory)!.TrimEnd('\\');

            // 1. 把系统镜像复制到本地磁盘（PE 端从磁盘读取，不依赖光驱；重启后虚拟挂载会失效）
            string localWim = Path.Combine(staging, "install.wim");
            AppendLog($"开始部署：复制系统镜像到 {localWim}（视镜像大小可能需要数分钟）…");
            await DeployService.CopyFileWithProgressAsync(_wimPath!, localWim, progress, default);

            // 2. 组装 PE + 写 deploy.json + 写一次性引导（BcdBootService 完成并自带回滚）
            var boot = new BcdBootService();
            var request = new PeBootRequest
            {
                PeSourceDirectory = BundledPayload.PeDirectory,
                StagingDirectory = staging,
                SystemDrive = systemDrive,
                Deployment = new DeployConfiguration
                {
                    WimPath = localWim,
                    WimIndex = _selectedImage.Index,
                    TargetDrive = target.Drive,
                    AutoReboot = AutoRebootCheck.IsChecked == true,
                },
            };

            AppendLog("正在组装 WinPE 部署环境（约 600MB，请稍候）…");
            PeBootSession session = await boot.PreparePeBootAsync(request);
            AppendLog($"✓ 一次性引导已写入（启动项 {session.BootEntryGuid}）。");
            AppendLog("✓ 部署环境就绪，重启后自动进入 PE 开始安装。");

            GoToPage(5);
        }
        catch (Exception ex)
        {
            AppendLog("部署失败：" + ex.Message);
            MessageBox.Show(this, ex.Message, "部署失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            SetBusy(false);
        }
    }

    // ---------------- 完成页 ----------------

    private async void RebootButton_Click(object sender, RoutedEventArgs e)
    {
        MessageBoxResult confirm = MessageBox.Show(this,
            "确认立即重启？重启后自动进入 WinPE 开始安装 Windows。",
            "立即重启", MessageBoxButton.OKCancel, MessageBoxImage.Information);
        if (confirm != MessageBoxResult.OK)
        {
            return;
        }

        try
        {
            await BcdBootService.RestartComputerAsync();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "重启失败：" + ex.Message + "\n请手动重启。", "重启失败",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void ExitButton_Click(object sender, RoutedEventArgs e)
    {
        Close();
    }

    // ---------------- 导航 ----------------

    private void BackButton_Click(object sender, RoutedEventArgs e)
    {
        if (_busy)
        {
            return;
        }

        int target = _page switch { 1 => 0, 2 => 1, 3 => 2, 4 => 3, _ => _page };
        if (target != _page)
        {
            GoToPage(target);
        }
    }

    private async void NextButton_Click(object sender, RoutedEventArgs e)
    {
        if (_busy)
        {
            return;
        }

        switch (_page)
        {
            case 0:
                if (!IsAdministrator())
                {
                    MessageBox.Show(this,
                        "写入系统引导需要管理员权限，请右键程序选择“以管理员身份运行”。",
                        "需要管理员权限", MessageBoxButton.OK, MessageBoxImage.Warning);
                    return;
                }
                if (!BundledPayload.TryGetPePayload(out string missing))
                {
                    MessageBox.Show(this,
                        $"内置 PE 资源不完整（缺 {missing}），无法部署。\n请使用完整分发的程序包。",
                        "内置资源缺失", MessageBoxButton.OK, MessageBoxImage.Error);
                    return;
                }
                GoToPage(1);
                break;

            case 1:
                if (string.IsNullOrEmpty(_wimPath))
                {
                    MessageBox.Show(this, "请先选择系统镜像。", "提示", MessageBoxButton.OK, MessageBoxImage.Information);
                    return;
                }
                await EnumerateImagesAsync();
                break;

            case 2:
                _selectedImage = VersionList.SelectedItem as WimImageInfo;
                if (_selectedImage is null)
                {
                    return;
                }
                GoToPage(3);
                break;

            case 3:
                if (PartitionList.SelectedItem is not PartitionInfo)
                {
                    return;
                }
                GoToPage(4);
                break;

            case 4:
                await DeployAsync();
                break;
        }
    }

    // ---------------- 辅助 ----------------

    private void SetBusy(bool busy)
    {
        _busy = busy;
        NextButton.IsEnabled = !busy;
        BackButton.IsEnabled = !busy;
        BrowseButton.IsEnabled = !busy;

        if (busy)
        {
            return;
        }

        // 恢复导航可用性时按页面约束重新计算
        NextButton.IsEnabled = _page switch
        {
            1 => !string.IsNullOrEmpty(_wimPath),
            2 => VersionList.SelectedItem is not null,
            3 => PartitionList.SelectedItem is not null,
            _ => true,
        };
    }

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

    private static bool IsAdministrator()
    {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private Brush OkBrush => (Brush)FindResource("OkBrush");
    private Brush WarnBrush => (Brush)FindResource("WarnBrush");
    private Brush AccentBrush => (Brush)FindResource("AccentBrush");
    private Brush MutedBrush => (Brush)FindResource("MutedBrush");
    private Brush TextBrush => (Brush)FindResource("TextBrush");
}
