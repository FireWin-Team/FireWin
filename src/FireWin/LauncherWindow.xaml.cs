using System;
using System.IO;
using System.Security.Principal;
using System.Windows;
using FireWin.Services;

namespace FireWin;

/// <summary>
/// 启动选择页：系统安装与 PE 制作在同一个程序、同一个目录里，
/// 用户在这里二选一进入 <see cref="MainWindow"/> 或 <see cref="UsbWindow"/>。
/// </summary>
public partial class LauncherWindow : Window
{
    public LauncherWindow()
    {
        InitializeComponent();

        FooterText.Text = BuildStatusText();
        VersionText.Text = "v" + (typeof(LauncherWindow).Assembly.GetName().Version?.ToString(3) ?? "1.0.0");
    }

    private static string BuildStatusText()
    {
        string pe;
        if (BundledPayload.TryGetPePayload(out string missing))
        {
            long bytes = 0;
            try
            {
                var info = new FileInfo(Path.Combine(BundledPayload.PeDirectory, "boot.wim"));
                bytes = info.Exists ? info.Length : 0;
            }
            catch
            {
                // 读取失败只影响展示，不影响功能
            }

            pe = $"内置 PE 就绪（boot.wim {DeployService.FormatBytes((ulong)bytes)}）";
        }
        else
        {
            pe = $"内置 PE 缺失：{missing}";
        }

        string admin = IsAdministrator()
            ? "已以管理员身份运行"
            : "未以管理员身份运行（安装系统与制作 PE 都需要管理员权限）";
        return $"{pe} · {admin}";
    }

    private void InstallButton_Click(object sender, RoutedEventArgs e)
    {
        Open(new MainWindow());
    }

    private void PeButton_Click(object sender, RoutedEventArgs e)
    {
        Open(new UsbWindow());
    }

    /// <summary>先切换主窗口再关闭选择页，避免 ShutdownMode=OnMainWindowClose 直接退出程序。</summary>
    private void Open(Window window)
    {
        Application.Current.MainWindow = window;
        window.Show();
        Close();
    }

    private static bool IsAdministrator()
    {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }
}
