using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

namespace FireWin.Services;

/// <summary>ADK 安装状态。</summary>
/// <param name="AdkInstalled">ADK 主包（Deployment Tools）是否可用。</param>
/// <param name="WinPeAddonInstalled">WinPE 附加组件是否可用（独立安装包）。</param>
/// <param name="KitsRoot">注册表中的 KitsRoot10 实际路径。</param>
public sealed record AdkStatus(bool AdkInstalled, bool WinPeAddonInstalled, string? KitsRoot)
{
    /// <summary>两者都装好才算就绪：缺少任何一个都无法执行 copype / MakeWinPEMedia。</summary>
    public bool IsReady => AdkInstalled && WinPeAddonInstalled;
}

/// <summary>
/// ADK 与 WinPE 附加组件的检测与静默安装。
/// 策略：winget 优先；winget 不可用或安装失败时回退到官方 adksetup.exe / adkwinpesetup.exe。
/// 注意：ADK 主包与 WinPE 附加组件是两个独立安装程序，必须分别安装。
/// </summary>
public sealed class AdkInstaller
{
    private const string KitsRootValueName = "KitsRoot10";
    private const string KitsKeyPath = @"SOFTWARE\Microsoft\Windows Kits\Installed Roots";
    private const string KitsKeyPathWow = @"SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots";

    private const string WingetAdkId = "Microsoft.WindowsADK";
    private const string WingetWinPeAddonId = "Microsoft.WindowsADK.WinPEAddon";

    // 官方 ADK / WinPE 附加组件直链（ADK 10.1.26100 系列）。
    // 微软会更新链接指向；若下载失效，请替换为 learn.microsoft.com 上最新的 ADK 下载地址。
    private const string AdkSetupUrl = "https://go.microsoft.com/fwlink/?linkid=2289980";
    private const string WinPeAddonSetupUrl = "https://go.microsoft.com/fwlink/?linkid=2289981";

    /// <summary>检测 ADK 主包与 WinPE 附加组件是否已安装。</summary>
    public AdkStatus Detect()
    {
        string? kitsRoot = GetKitsRoot();
        if (kitsRoot is null)
        {
            return new AdkStatus(false, false, null);
        }

        string adkRoot = Path.Combine(kitsRoot, "Assessment and Deployment Kit");
        // Deployment Tools 是 copype / dism / MakeWinPEMedia 的落点，缺它等于没有可用 ADK
        bool adkInstalled = Directory.Exists(Path.Combine(adkRoot, "Deployment Tools"));
        // WinPE 附加组件的文件落在 ADK 目录下的 Windows Preinstallation Environment
        bool winPeAddonInstalled = Directory.Exists(Path.Combine(adkRoot, "Windows Preinstallation Environment"));

        return new AdkStatus(adkInstalled, winPeAddonInstalled, kitsRoot);
    }

    /// <summary>
    /// 确保 ADK 主包 + WinPE 附加组件均已安装；已安装的组件会被跳过。
    /// </summary>
    public async Task<AdkStatus> EnsureInstalledAsync(IProgress<string>? progress = null, CancellationToken cancellationToken = default)
    {
        if (!IsRunningAsAdministrator())
        {
            throw new AdkInstallException("安装 ADK 需要管理员权限，请以管理员身份重新运行。");
        }

        AdkStatus status = Detect();
        if (status.IsReady)
        {
            progress?.Report("[ADK] 主包与 WinPE 附加组件均已安装。");
            return status;
        }

        if (!status.AdkInstalled)
        {
            progress?.Report("[ADK] 未检测到 ADK 主包，开始安装……");
            await InstallPackageAsync(
                WingetAdkId,
                "Windows ADK",
                AdkSetupUrl,
                "adksetup.exe",
                // 只装部署工具，避免拉入不需要的评估工具包
                new[] { "/quiet", "/norestart", "/ceip", "off", "/features", "OptionId.DeploymentTools" },
                progress,
                cancellationToken).ConfigureAwait(false);
        }

        status = Detect();
        if (!status.AdkInstalled)
        {
            throw new AdkInstallException("ADK 主包安装后仍未检测到 Deployment Tools，请重启后重试或查看安装日志。");
        }

        if (!status.WinPeAddonInstalled)
        {
            progress?.Report("[ADK] 未检测到 WinPE 附加组件，开始安装……");
            await InstallPackageAsync(
                WingetWinPeAddonId,
                "Windows ADK WinPE Addon",
                WinPeAddonSetupUrl,
                "adkwinpesetup.exe",
                new[] { "/quiet", "/norestart" },
                progress,
                cancellationToken).ConfigureAwait(false);
        }

        status = Detect();
        if (!status.IsReady)
        {
            throw new AdkInstallException("WinPE 附加组件安装后仍未检测到 Windows Preinstallation Environment 目录，请重启后重试。");
        }

        progress?.Report("[ADK] 安装完成。");
        return status;
    }

    /// <summary>读取注册表 KitsRoot10（同时兼容 32/64 位视图与 WOW6432Node 显式路径）。</summary>
    public static string? GetKitsRoot()
    {
        (RegistryHive hive, string path)[] candidates =
        {
            (RegistryHive.LocalMachine, KitsKeyPath),
            (RegistryHive.LocalMachine, KitsKeyPathWow),
        };

        foreach ((RegistryHive hive, string path) in candidates)
        {
            foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
            {
                try
                {
                    using RegistryKey baseKey = RegistryKey.OpenBaseKey(hive, view);
                    using RegistryKey? key = baseKey.OpenSubKey(path);
                    if (key?.GetValue(KitsRootValueName) is string value && !string.IsNullOrWhiteSpace(value))
                    {
                        return value.TrimEnd('\\') + "\\";
                    }
                }
                catch (Exception ex) when (ex is UnauthorizedAccessException or System.Security.SecurityException)
                {
                    // 读不到就换下一个视图
                }
            }
        }

        return null;
    }

    /// <summary>当前进程是否以管理员身份运行（bcdedit / ADK 静默安装都需要）。</summary>
    public static bool IsRunningAsAdministrator()
    {
        if (!OperatingSystem.IsWindows())
        {
            return false;
        }

        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    /// <summary>单个安装包的安装流程：先 winget，失败再回退官方安装程序。</summary>
    private async Task InstallPackageAsync(
        string wingetId,
        string displayName,
        string setupUrl,
        string setupFileName,
        string[] setupArguments,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        if (await TryInstallWithWingetAsync(wingetId, displayName, progress, cancellationToken).ConfigureAwait(false))
        {
            return;
        }

        progress?.Report($"[ADK] winget 安装 {displayName} 失败，回退到官方安装程序。");

        string setupPath = Path.Combine(Path.GetTempPath(), setupFileName);
        if (!File.Exists(setupPath))
        {
            progress?.Report($"[ADK] 正在下载 {displayName} 安装程序……");
            using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
            byte[] data = await http.GetByteArrayAsync(setupUrl, cancellationToken).ConfigureAwait(false);
            await File.WriteAllBytesAsync(setupPath, data, cancellationToken).ConfigureAwait(false);
        }

        ProcessResult result = await RunProcessAsync(setupPath, setupArguments, progress, cancellationToken).ConfigureAwait(false);

        // 3010 = 安装成功但需要重启
        if (result.ExitCode != 0 && result.ExitCode != 3010)
        {
            throw new AdkInstallException($"{displayName} 安装失败（退出码 {result.ExitCode}）。输出：{result.Output}");
        }
    }

    /// <summary>尝试用 winget 静默安装；winget 不存在或返回非 0 时返回 false。</summary>
    private static async Task<bool> TryInstallWithWingetAsync(
        string wingetId,
        string displayName,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        string? wingetPath = FindWinget();
        if (wingetPath is null)
        {
            progress?.Report("[ADK] 未找到 winget，跳过 winget 安装方式。");
            return false;
        }

        string[] args =
        {
            "install", "--id", wingetId, "--exact", "--silent",
            "--accept-package-agreements", "--accept-source-agreements", "--disable-interactivity",
        };

        progress?.Report($"[ADK] winget install {wingetId} ……");
        try
        {
            ProcessResult result = await RunProcessAsync(wingetPath, args, progress, cancellationToken).ConfigureAwait(false);
            if (result.ExitCode == 0)
            {
                return true;
            }

            progress?.Report($"[ADK] winget 返回退出码 {result.ExitCode}（{displayName}）。");
            return false;
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            progress?.Report($"[ADK] winget 调用异常：{ex.Message}");
            return false;
        }
    }

    private static string? FindWinget()
    {
        string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string[] candidates =
        {
            Path.Combine(localAppData, "Microsoft", "WindowsApps", "winget.exe"),
            Path.Combine(Environment.SystemDirectory, "winget.exe"),
        };

        foreach (string candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        // 走 PATH 兜底
        string? pathVar = Environment.GetEnvironmentVariable("PATH");
        if (!string.IsNullOrEmpty(pathVar))
        {
            foreach (string dir in pathVar.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                string candidate = Path.Combine(dir.Trim(), "winget.exe");
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
        }

        return null;
    }

    /// <summary>执行外部程序并捕获输出（安装类命令耗时较长，超时设为 90 分钟）。</summary>
    private static async Task<ProcessResult> RunProcessAsync(
        string fileName,
        IEnumerable<string> arguments,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        var psi = new ProcessStartInfo
        {
            FileName = fileName,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        foreach (string arg in arguments)
        {
            psi.ArgumentList.Add(arg);
        }

        var output = new StringBuilder();
        using var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
        process.OutputDataReceived += (_, e) => AppendLine(output, e.Data, progress);
        process.ErrorDataReceived += (_, e) => AppendLine(output, e.Data, progress);

        if (!process.Start())
        {
            throw new AdkInstallException($"无法启动进程：{fileName}");
        }

        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromMinutes(90));

        try
        {
            await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch
            {
                // 进程可能已自行退出
            }

            throw;
        }

        return new ProcessResult(process.ExitCode, output.ToString().Trim());
    }

    private static void AppendLine(StringBuilder builder, string? line, IProgress<string>? progress)
    {
        if (line is null)
        {
            return;
        }

        builder.AppendLine(line);
        progress?.Report("  " + line);
    }

    private readonly record struct ProcessResult(int ExitCode, string Output);
}

/// <summary>ADK 安装相关异常。</summary>
public sealed class AdkInstallException : Exception
{
    public AdkInstallException(string message) : base(message)
    {
    }
}
