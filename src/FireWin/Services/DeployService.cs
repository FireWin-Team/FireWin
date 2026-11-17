using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using ManagedWimLib;

namespace FireWin.Services;

/// <summary>WIM 镜像中的一个索引（对应一个 Windows 版本）。</summary>
public sealed record WimImageInfo(int Index, string Name, string Description, ulong TotalBytes)
{
    /// <summary>界面显示用的大小文本。</summary>
    public string TotalBytesText => TotalBytes == 0 ? string.Empty : DeployService.FormatBytes(TotalBytes);
}

/// <summary>本地固定磁盘分区信息。</summary>
public sealed record PartitionInfo(
    string Drive,
    string VolumeLabel,
    ulong TotalBytes,
    ulong FreeBytes,
    bool IsSystemDrive)
{
    /// <summary>系统盘在界面上附加的标记文本。</summary>
    public string SystemTagText => IsSystemDrive ? "（当前系统盘，不可作为安装目标）" : string.Empty;

    /// <summary>界面显示用的容量摘要。</summary>
    public string SummaryText => $"总容量 {DeployService.FormatBytes(TotalBytes)} · 可用 {DeployService.FormatBytes(FreeBytes)}";
}

/// <summary>ISO 挂载结果。</summary>
public sealed record IsoMountResult(string DriveLetter, string IsoPath, string InstallWimPath);

/// <summary>
/// 部署准备服务（桌面端）：
///   1. 挂载 ISO 并定位 sources\install.wim；
///   2. 用 wimlib 枚举镜像索引（Windows 版本列表）；
///   3. 枚举本地固定磁盘分区（目标分区候选）；
///   4. 带进度复制系统镜像到 C:\FireWinPE\install.wim（PE 端从本地磁盘读取，不依赖光驱）。
/// 组装 PE 与写引导由 <see cref="BcdBootService"/> 完成。
/// </summary>
public static class DeployService
{
    private static readonly Regex ImageBytesRegex = new(
        "<IMAGE\\s+INDEX=\"(?<index>\\d+)\"[^>]*>.*?<TOTALBYTES>(?<bytes>\\d+)</TOTALBYTES>",
        RegexOptions.Compiled | RegexOptions.Singleline);

    #region ISO 挂载

    /// <summary>
    /// 挂载 Windows 安装 ISO，并定位其中的系统镜像（sources\install.wim / install.esd）。
    /// 返回的挂载盘符在注销/重启前有效；部署时镜像会被复制到本地磁盘，因此挂载只是读取阶段使用。
    /// </summary>
    public static async Task<IsoMountResult> MountIsoAsync(string isoPath, CancellationToken cancellationToken = default)
    {
        if (!File.Exists(isoPath))
        {
            throw new DeployException($"找不到 ISO 文件：{isoPath}");
        }

        string escaped = isoPath.Replace("'", "''");
        string script =
            "$ErrorActionPreference='Stop'; " +
            "$img = Mount-DiskImage -ImagePath '" + escaped + "' -PassThru; " +
            "($img | Get-Volume).DriveLetter";

        string output = await RunPowerShellAsync(script, cancellationToken).ConfigureAwait(false);
        string letter = output.Trim().TrimEnd(':', '\r', '\n').ToUpperInvariant();
        if (letter.Length != 1 || !char.IsAsciiLetter(letter[0]))
        {
            throw new DeployException($"ISO 挂载成功但未能获取盘符（PowerShell 输出：{output.Trim()}）。");
        }

        string driveRoot = letter + ":";
        string wimPath = FindInstallWim(driveRoot);
        return new IsoMountResult(letter, isoPath, wimPath);
    }

    /// <summary>卸载 ISO（幂等，失败不影响主流程）。</summary>
    public static void TryDismountIso(string isoPath)
    {
        try
        {
            string escaped = isoPath.Replace("'", "''");
            using var process = Process.Start(new ProcessStartInfo(
                "powershell.exe",
                $"-NoProfile -Command \"Dismount-DiskImage -ImagePath '{escaped}'\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
            });
            process?.WaitForExit(15000);
        }
        catch
        {
            // 卸载失败不影响部署结果；虚拟挂载在重启后也会自动消失
        }
    }

    /// <summary>在挂载盘内定位 Windows 安装镜像。</summary>
    private static string FindInstallWim(string driveRoot)
    {
        string[] candidates =
        {
            Path.Combine(driveRoot, "sources", "install.wim"),
            Path.Combine(driveRoot, "sources", "install.esd"),
            Path.Combine(driveRoot, "sources", "install.swm"),
        };
        foreach (string candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        // 非标准介质兜底：在 sources 目录浅搜 install.*
        string sources = Path.Combine(driveRoot, "sources");
        if (Directory.Exists(sources))
        {
            foreach (string file in Directory.EnumerateFiles(sources, "install.*"))
            {
                string ext = Path.GetExtension(file).ToLowerInvariant();
                if (ext is ".wim" or ".esd" or ".swm")
                {
                    return file;
                }
            }
        }

        throw new DeployException(
            $"ISO 已挂载到 {driveRoot}，但未找到 sources\\install.wim——该镜像可能不是 Windows 安装介质。");
    }

    private static async Task<string> RunPowerShellAsync(string script, CancellationToken cancellationToken)
    {
        var psi = new ProcessStartInfo
        {
            FileName = "powershell.exe",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        psi.ArgumentList.Add("-NoProfile");
        psi.ArgumentList.Add("-ExecutionPolicy");
        psi.ArgumentList.Add("Bypass");
        psi.ArgumentList.Add("-Command");
        psi.ArgumentList.Add(script);

        using var process = new Process { StartInfo = psi };
        process.Start();
        string output = await process.StandardOutput.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
        string error = await process.StandardError.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        if (process.ExitCode != 0)
        {
            throw new DeployException($"挂载 ISO 失败：{error.Trim()}");
        }

        return output;
    }

    #endregion

    #region 镜像枚举

    /// <summary>枚举 WIM 中的全部镜像索引（1 起始），返回名称、描述与体积。</summary>
    public static List<WimImageInfo> ListWimImages(string wimPath)
    {
        if (!File.Exists(wimPath))
        {
            throw new DeployException($"找不到镜像文件：{wimPath}");
        }

        using Wim wim = Wim.OpenWim(wimPath, OpenFlags.None);
        int count = (int)wim.GetWimInfo().ImageCount;
        if (count <= 0)
        {
            throw new DeployException("镜像中未包含任何系统版本，无法部署。");
        }

        var images = new List<WimImageInfo>(count);
        for (int index = 1; index <= count; index++)
        {
            string? name = wim.GetImageName(index);
            string? description = wim.GetImageDescription(index);
            images.Add(new WimImageInfo(
                index,
                string.IsNullOrWhiteSpace(name) ? $"镜像 {index}" : name,
                string.IsNullOrWhiteSpace(description) ? string.Empty : description,
                0));
        }

        // 每个索引的实际大小在 WIM 的 XML 元数据里
        try
        {
            string xml = wim.GetXmlData();
            Dictionary<int, ulong> sizes = ParseImageSizes(xml);
            for (int i = 0; i < images.Count; i++)
            {
                if (sizes.TryGetValue(images[i].Index, out ulong bytes))
                {
                    images[i] = images[i] with { TotalBytes = bytes };
                }
            }
        }
        catch
        {
            // 大小信息缺失不影响部署
        }

        return images;
    }

    private static Dictionary<int, ulong> ParseImageSizes(string xml)
    {
        var result = new Dictionary<int, ulong>();
        foreach (Match match in ImageBytesRegex.Matches(xml))
        {
            if (int.TryParse(match.Groups["index"].Value, out int index) &&
                ulong.TryParse(match.Groups["bytes"].Value, out ulong bytes))
            {
                result[index] = bytes;
            }
        }

        return result;
    }

    #endregion

    #region 分区枚举

    /// <summary>枚举本地固定磁盘分区（目标分区候选；系统盘标记出来供界面禁选）。</summary>
    public static List<PartitionInfo> GetFixedPartitions()
    {
        string systemRoot = Path.GetPathRoot(Environment.SystemDirectory) ?? "C:";
        var partitions = new List<PartitionInfo>();

        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            if (!drive.IsReady || drive.DriveType != DriveType.Fixed)
            {
                continue;
            }

            string root = Path.GetPathRoot(drive.Name) ?? drive.Name;
            if (root.Length < 2)
            {
                continue;
            }

            partitions.Add(new PartitionInfo(
                root[..2].ToUpperInvariant(),
                string.IsNullOrWhiteSpace(drive.VolumeLabel) ? "(无卷标)" : drive.VolumeLabel,
                (ulong)drive.TotalSize,
                (ulong)drive.AvailableFreeSpace,
                root.Equals(systemRoot, StringComparison.OrdinalIgnoreCase)));
        }

        return partitions;
    }

    #endregion

    #region 文件复制

    /// <summary>带百分比进度的大文件复制（顺序读 + 异步写，1MB 缓冲）。</summary>
    /// <param name="percentProgress">可选：本次复制的 0–100 百分比（供界面进度条使用）。</param>
    public static async Task CopyFileWithProgressAsync(
        string sourcePath, string destinationPath, IProgress<string>? progress, CancellationToken cancellationToken,
        IProgress<int>? percentProgress = null)
    {
        long total = new FileInfo(sourcePath).Length;
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);

        const int bufferSize = 1 << 20;
        await using var input = new FileStream(
            sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read,
            bufferSize, FileOptions.Asynchronous | FileOptions.SequentialScan);
        await using var output = new FileStream(
            destinationPath, FileMode.Create, FileAccess.Write, FileShare.None,
            bufferSize, FileOptions.Asynchronous);

        byte[] buffer = new byte[bufferSize];
        long copied = 0;
        int lastPercent = -1;
        int read;

        while ((read = await input.ReadAsync(buffer, cancellationToken).ConfigureAwait(false)) > 0)
        {
            await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken).ConfigureAwait(false);
            copied += read;

            int percent = (int)(copied * 100 / Math.Max(total, 1));
            if (percent != lastPercent)
            {
                lastPercent = percent;
                progress?.Report(
                    $"复制 {Path.GetFileName(sourcePath)}… {percent}%（{FormatBytes(copied)} / {FormatBytes(total)}）");
                percentProgress?.Report(percent);
            }
        }

        await output.FlushAsync(cancellationToken).ConfigureAwait(false);
        percentProgress?.Report(100);
        progress?.Report($"复制 {Path.GetFileName(sourcePath)} 完成。");
    }

    #endregion

    /// <summary>字节量格式化（B / KB / MB / GB / TB）。</summary>
    public static string FormatBytes(ulong value)
    {
        double bytes = value;
        string[] units = { "B", "KB", "MB", "GB", "TB" };
        int unit = 0;
        while (bytes >= 1024 && unit < units.Length - 1)
        {
            bytes /= 1024;
            unit++;
        }

        return $"{bytes:0.#} {units[unit]}";
    }

    /// <summary>字节量格式化（long 重载，内部复制进度用）。</summary>
    private static string FormatBytes(long value) => FormatBytes((ulong)Math.Max(value, 0));
}

/// <summary>部署准备阶段异常。</summary>
public sealed class DeployException : Exception
{
    public DeployException(string message) : base(message)
    {
    }
}
