using System;
using System.Runtime.InteropServices;
using System.Windows;
using FireWin.Services;

namespace FireWin;

/// <summary>
/// 应用程序入口：
/// 无参数双击运行时打开任务选择页（一键安装系统 / 制作 PE）；
/// 带参数运行时走命令行模式（便于脚本化验证，输出会附加到调用方控制台）。
/// </summary>
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // 内部演示参数：--demo-image <路径> 启动后直接进入镜像页并完成识别（供自动化截图使用）
        if (e.Args.Length >= 2 && e.Args[0].Equals("--demo-image", StringComparison.OrdinalIgnoreCase))
        {
            MainWindow = new MainWindow();
            MainWindow.Show();
            ((MainWindow)MainWindow).DemoSelectImage(e.Args[1]);
            return;
        }

        if (e.Args.Length > 0)
        {
            int exitCode = RunCli(e.Args);
            Shutdown(exitCode);
            return;
        }

        MainWindow = new LauncherWindow();
        MainWindow.Show();
    }

    /// <summary>命令行模式，与原 Program.cs 的开关保持一致。</summary>
    private static int RunCli(string[] args)
    {
        // 输出被重定向（脚本/管道捕获）时标准句柄已经可用，不能再附加控制台，否则输出会丢到控制台缓冲区
        bool attached = false;
        if (!Console.IsOutputRedirected)
        {
            attached = NativeConsole.AttachToParent();
        }

        try
        {
            string command = args[0].ToLowerInvariant();
            switch (command)
            {
                case "--wim-init-check":
                {
                    using var wim = new WimInitService();
                    wim.EnsureInitialized();
                    Console.WriteLine($"[OK] wimlib 已初始化，原生库：{wim.LoadedLibraryPath}");
                    return 0;
                }

                case "--adk-status":
                {
                    AdkStatus status = new AdkInstaller().Detect();
                    Console.WriteLine($"ADK 主包：{(status.AdkInstalled ? "已安装" : "未安装")}");
                    Console.WriteLine($"WinPE 附加组件：{(status.WinPeAddonInstalled ? "已安装" : "未安装")}");
                    Console.WriteLine($"KitsRoot10：{status.KitsRoot ?? "(未找到)"}");
                    return status.IsReady ? 0 : 2;
                }

                case "--adk-install":
                {
                    var installer = new AdkInstaller();
                    installer.EnsureInstalledAsync(ConsoleProgress.Instance).GetAwaiter().GetResult();
                    Console.WriteLine("[OK] ADK 与 WinPE 附加组件已就绪");
                    return 0;
                }

                case "--version":
                    Console.WriteLine("FireWin 1.0.0 (GPL-3.0-only)");
                    return 0;

                default:
                    Console.WriteLine("""
                        FireWin 桌面端
                        用法：
                          FireWin.exe                        打开图形界面
                          FireWin.exe --wim-init-check       校验 libwim-15.dll 与 wimlib 初始化
                          FireWin.exe --adk-status           查询 ADK / WinPE 附加组件安装状态
                          FireWin.exe --adk-install          静默安装 ADK 主包 + WinPE 附加组件（需管理员）
                          FireWin.exe --version              显示版本
                        许可证：GPL-3.0-only
                        """);
                    return 0;
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[错误] {ex.Message}");
            return 1;
        }
        finally
        {
            Console.Out.Flush();
            if (attached)
            {
                NativeConsole.Detach();
            }
        }
    }

    /// <summary>把进度文本直接打到控制台。</summary>
    private sealed class ConsoleProgress : IProgress<string>
    {
        public static readonly ConsoleProgress Instance = new();

        public void Report(string value) => Console.WriteLine(value);
    }

    /// <summary>控制台附着：WinExe 默认没有控制台，附加到父进程后才能看到命令行输出。</summary>
    private static class NativeConsole
    {
        private const int AttachParentProcess = -1;

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AttachConsole(int processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FreeConsole();

        public static bool AttachToParent() => AttachConsole(AttachParentProcess);

        public static void Detach() => FreeConsole();
    }
}
