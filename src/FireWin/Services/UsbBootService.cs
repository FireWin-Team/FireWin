using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;

namespace FireWin.Services;

/// <summary>可移动磁盘（U 盘）信息，按物理磁盘去重后的制作候选。</summary>
public sealed record UsbDiskInfo(
    string Drive,
    string VolumeLabel,
    ulong PartitionTotalBytes,
    ulong PartitionFreeBytes,
    int DiskNumber,
    ulong DiskTotalBytes)
{
    /// <summary>界面显示用的容量摘要。</summary>
    public string SummaryText =>
        $"物理磁盘 {DiskNumber} · 磁盘总容量 {DeployService.FormatBytes(DiskTotalBytes)}" +
        $" · 当前分区 {DeployService.FormatBytes(PartitionTotalBytes)}（可用 {DeployService.FormatBytes(PartitionFreeBytes)}）";
}

/// <summary>
/// 制作 PE 启动盘（U 盘）服务：
///   1. 枚举可移动磁盘（IOCTL 按物理磁盘去重）；
///   2. diskpart 清盘重新分区：FAT32 启动分区（UEFI 必需，写完后去掉盘符隐藏）+ exFAT 数据分区（存大文件）；
///      已经是 FireWin 启动盘时可选「升级」：只替换启动分区里的 PE 文件，数据分区与镜像保持不动；
///   3. 写入 PE 引导文件（bootmgr / bootmgfw.efi / boot.sdi / boot.wim）；
///   4. bcdedit /store 生成 U 盘独立 BCD（device boot 别名，与盘符无关）；
///   5. bootsect 写 BIOS 引导代码（缺失时仅 UEFI 可启动，给出警告）；
///   6. 在数据分区创建 FWIMG 镜像目录（PE 优先扫描），兼容 \Images。
/// 制成的 U 盘：UEFI / Legacy BIOS 双引导，数据分区可当普通 U 盘使用；
/// 把 install.wim / install.esd（或 ISO）放进数据分区 \FWIMG，PE 端「安装系统」即可直接安装。
/// </summary>
public static class UsbBootService
{
    private const string BootPartitionLabel = "FireWinPE";
    private const string DataPartitionLabel = "FireData";

    /// <summary>数据分区中存放系统镜像的目录名（PE 端优先扫描此目录）。</summary>
    public const string ImageFolderName = "FWIMG";

    private const int MinBootPartitionMb = 1536;   // 启动分区下限 1.5GB（FAT32 + 引导文件）
    private const int DataPartitionReserveMb = 4096; // 双分区时给数据分区预留的空间

    private static readonly Regex GuidRegex = new(
        @"\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}",
        RegexOptions.Compiled);

    #region U 盘枚举

    /// <summary>枚举所有可移动磁盘（同一物理盘多分区只显示一条）。</summary>
    public static List<UsbDiskInfo> ListUsbDisks()
    {
        var byDisk = new Dictionary<int, UsbDiskInfo>();

        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            if (!drive.IsReady || drive.DriveType != DriveType.Removable)
            {
                continue;
            }

            string root = Path.GetPathRoot(drive.Name) ?? drive.Name;
            if (root.Length < 3)
            {
                continue;
            }

            int? diskNumber = TryGetDiskNumber(root);
            if (diskNumber is null || byDisk.ContainsKey(diskNumber.Value))
            {
                continue;
            }

            ulong diskBytes = GetDiskTotalBytes(diskNumber.Value);
            byDisk[diskNumber.Value] = new UsbDiskInfo(
                root[..2].ToUpperInvariant(),
                string.IsNullOrWhiteSpace(drive.VolumeLabel) ? "(无卷标)" : drive.VolumeLabel,
                (ulong)drive.TotalSize,
                (ulong)drive.AvailableFreeSpace,
                diskNumber.Value,
                diskBytes == 0 ? (ulong)drive.TotalSize : diskBytes);
        }

        return byDisk.Values.OrderBy(d => d.DiskNumber).ToList();
    }

    /// <summary>IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS：盘符 → 物理磁盘号。</summary>
    private static int? TryGetDiskNumber(string rootPath)
    {
        using SafeFileHandleVolume handle = OpenVolumeHandle(@"\\.\" + rootPath[..2]);
        if (handle.IsInvalid)
        {
            return null;
        }

        var extents = new NativeMethods.VolumeDiskExtents();
        if (!NativeMethods.DeviceIoControl(
                handle,
                NativeMethods.IoctlVolumeGetVolumeDiskExtents,
                IntPtr.Zero, 0,
                out extents, Marshal.SizeOf<NativeMethods.VolumeDiskExtents>() * 8,
                out _, IntPtr.Zero))
        {
            return null;
        }

        return extents.NumberOfDiskExtents > 0 ? extents.Extent.DiskNumber : null;
    }

    /// <summary>IOCTL_DISK_GET_DRIVE_GEOMETRY：物理磁盘总容量。</summary>
    private static ulong GetDiskTotalBytes(int diskNumber)
    {
        using SafeFileHandleVolume handle = OpenVolumeHandle(@"\\.\PhysicalDrive" + diskNumber);
        if (handle.IsInvalid)
        {
            return 0;
        }

        if (!NativeMethods.DeviceIoControl(
                handle,
                NativeMethods.IoctlDiskGetDriveGeometry,
                IntPtr.Zero, 0,
                out NativeMethods.DiskGeometry geometry, Marshal.SizeOf<NativeMethods.DiskGeometry>(),
                out _, IntPtr.Zero))
        {
            return 0;
        }

        ulong geometryBytes =
            (ulong)geometry.Cylinders *
            (ulong)geometry.TracksPerCylinder *
            (ulong)geometry.SectorsPerTrack *
            (ulong)geometry.BytesPerSector;
        return geometryBytes;
    }

    private static SafeFileHandleVolume OpenVolumeHandle(string path)
    {
        return NativeMethods.CreateFileW(
            path,
            NativeMethods.GenericRead,
            (int)(FileShare.Read | FileShare.Write),
            IntPtr.Zero,
            FileMode.Open,
            0,
            IntPtr.Zero);
    }

    #endregion

    #region 制作启动盘

    /// <summary>制作启动盘主流程（U 盘将被完全清空并重新分区）；percentProgress 为总体进度 0–100。</summary>
    public static async Task CreateBootDiskAsync(
        UsbDiskInfo disk,
        string? optionalImagePath,
        IProgress<string> progress,
        IProgress<int>? percentProgress,
        CancellationToken cancellationToken)
    {
        // 1. 按内置 PE 的实际大小估算启动分区；剩余空间足够时默认双分区（隐藏启动分区 + 数据分区）
        int bootPartitionMb = EstimateBootPartitionMb();
        bool dualPartition = disk.DiskTotalBytes >
            (ulong)(bootPartitionMb + DataPartitionReserveMb) * 1024 * 1024;

        progress.Report($"正在对磁盘 {disk.DiskNumber} 重新分区（所有数据将被清除）…");
        percentProgress?.Report(2);
        progress.Report(dualPartition
            ? $"分区方案：启动分区 {bootPartitionMb} MB（写完后隐藏）+ 数据分区（卷标 {DataPartitionLabel}，含 {ImageFolderName} 目录）。"
            : $"分区方案：磁盘容量较小，启动分区独占整盘 {DeployService.FormatBytes(disk.DiskTotalBytes)}（不隐藏，可直接放镜像）。");

        await RunDiskpartScriptAsync(disk, bootPartitionMb, dualPartition, cancellationToken).ConfigureAwait(false);
        percentProgress?.Report(12);

        // 2. 等待新卷挂载并按卷标定位盘符
        string bootLetter = await WaitForVolumeAsync(BootPartitionLabel, cancellationToken).ConfigureAwait(false)
            ?? throw new DeployException("分区完成但未找到启动分区（卷标 FireWinPE），diskpart 输出见日志。");
        progress.Report($"启动分区就绪：{bootLetter}");
        percentProgress?.Report(15);

        string? dataLetter = null;
        if (dualPartition)
        {
            dataLetter = await WaitForVolumeAsync(DataPartitionLabel, cancellationToken).ConfigureAwait(false)
                ?? throw new DeployException("分区完成但未找到数据分区（卷标 FireData），diskpart 输出见日志。");
            progress.Report($"数据分区就绪：{dataLetter}");
            percentProgress?.Report(17);
        }

        // 3. 写入 PE 引导文件（镜像落到数据分区 FWIMG）
        await WritePeFilesAsync(bootLetter, dataLetter, optionalImagePath, progress, percentProgress, cancellationToken)
            .ConfigureAwait(false);

        // 4. 生成 U 盘独立 BCD
        await CreateBcdStoreAsync(bootLetter, progress, percentProgress, cancellationToken).ConfigureAwait(false);

        // 5. BIOS 引导代码（bootsect，可选）
        await TryWriteBiosBootCodeAsync(bootLetter, disk.DiskNumber, progress, cancellationToken).ConfigureAwait(false);
        percentProgress?.Report(96);

        // 6. 隐藏启动分区：去掉盘符后资源管理器中不再显示
        if (dualPartition)
        {
            await HideBootPartitionAsync(bootLetter, progress, cancellationToken).ConfigureAwait(false);
        }

        progress.Report("✓ PE 启动盘制作完成。");
        percentProgress?.Report(100);
    }

    /// <summary>
    /// 判断选中磁盘是否已经是 FireWin 启动盘（可原地升级）。
    /// 两种情况：双分区布局（数据分区 FireData 可见，启动分区 FireWinPE 被隐藏），
    /// 或单分区布局（整盘一个 FAT32 分区，根目录已有 \sources\boot.wim）。
    /// </summary>
    public static bool CanUpgradeInPlace(UsbDiskInfo disk)
    {
        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            if (!drive.IsReady || !IsOwnFireWinPartition(drive))
            {
                continue;
            }

            string root = Path.GetPathRoot(drive.Name) ?? drive.Name;
            if (root.Length < 3 || TryGetDiskNumber(root) != disk.DiskNumber)
            {
                continue;
            }

            return true;
        }

        return File.Exists(Path.Combine(disk.Drive + @"\", "sources", "boot.wim"));
    }

    /// <summary>卷标是我们自己的（FireWinPE / FireData），才可能是 FireWin 启动盘。</summary>
    private static bool IsOwnFireWinPartition(DriveInfo drive)
    {
        return string.Equals(drive.VolumeLabel, BootPartitionLabel, StringComparison.OrdinalIgnoreCase) ||
               string.Equals(drive.VolumeLabel, DataPartitionLabel, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// 升级已有启动盘：不重新分区、不清空数据分区，只替换隐藏启动分区里的 PE 文件并重建引导配置。
    /// 启动分区空间不足或格式不对时提示改用全新制作。
    /// </summary>
    public static async Task UpgradeBootDiskAsync(
        UsbDiskInfo disk,
        string? optionalImagePath,
        IProgress<string> progress,
        IProgress<int>? percentProgress,
        CancellationToken cancellationToken)
    {
        string pe = BundledPayload.PeDirectory;
        string bootWim = Path.Combine(pe, "boot.wim");
        if (!File.Exists(bootWim) || !File.Exists(Path.Combine(pe, "boot.sdi")))
        {
            throw new DeployException("内置 PE 资源不完整（缺 boot.wim / boot.sdi），无法升级启动盘。");
        }

        // 1. 定位数据分区（可见）与启动分区（双分区布局下没有盘符，需要临时分配）
        string? dataLetter = FindOwnVolumeLetter(DataPartitionLabel, disk.DiskNumber);
        string? bootLetter = FindOwnVolumeLetter(BootPartitionLabel, disk.DiskNumber);
        bool assignedBootLetter = false;

        progress.Report("正在定位 U 盘上的 FireWin 分区…");
        percentProgress?.Report(2);

        if (bootLetter is null)
        {
            int? volumeNumber = await FindVolumeNumberAsync(BootPartitionLabel, cancellationToken).ConfigureAwait(false);
            if (volumeNumber is null)
            {
                throw new DeployException("未找到 FireWinPE 启动分区，无法原地升级；请改用全新制作。");
            }

            string letter = PickFreeDriveLetter();
            await RunDiskpartAsync($"select volume {volumeNumber}\r\nassign letter={letter}", cancellationToken)
                .ConfigureAwait(false);
            bootLetter = await WaitForVolumeAsync(BootPartitionLabel, cancellationToken).ConfigureAwait(false);
            if (bootLetter is null || TryGetDiskNumber(bootLetter) != disk.DiskNumber)
            {
                if (bootLetter is not null)
                {
                    await RunDiskpartAsync($"select volume {bootLetter}\r\nremove letter={bootLetter}", cancellationToken)
                        .ConfigureAwait(false);
                }

                throw new DeployException("无法挂载启动分区（可能属于其他磁盘）。请重新插拔 U 盘后重试，或改用全新制作。");
            }

            assignedBootLetter = true;
        }

        progress.Report($"启动分区：{bootLetter}；数据分区：{(dataLetter ?? "（无，单分区布局）")}");
        percentProgress?.Report(6);

        try
        {
            // 2. 前置校验：必须是 FAT32，且放得下新的 PE 文件（FAT32 分区固定大小，不能像重新分区那样扩容）
            string bootRoot = bootLetter + @"\";
            var bootDrive = new DriveInfo(bootRoot);
            if (!string.Equals(bootDrive.DriveFormat, "FAT32", StringComparison.OrdinalIgnoreCase))
            {
                throw new DeployException(
                    $"启动分区格式为 {bootDrive.DriveFormat}，UEFI 启动要求 FAT32，无法原地升级；请改用全新制作。");
            }

            long existingWimBytes = 0;
            var existingWim = new FileInfo(Path.Combine(bootRoot, "sources", "boot.wim"));
            if (existingWim.Exists)
            {
                existingWimBytes = existingWim.Length;
            }

            long requiredBytes = new FileInfo(bootWim).Length - existingWimBytes + 64L * 1024 * 1024;
            if (bootDrive.AvailableFreeSpace < requiredBytes)
            {
                throw new DeployException(
                    $"启动分区剩余空间不足（可用 {DeployService.FormatBytes((ulong)bootDrive.AvailableFreeSpace)}，" +
                    $"需要约 {DeployService.FormatBytes((ulong)Math.Max(requiredBytes, 0))}）。" +
                    "请改用全新制作（会按新镜像大小重新划分启动分区）。");
            }

            // 3. 覆盖 PE 文件（boot.wim / boot.sdi / bootmgr / bootmgfw / 字体），并刷新 FWIMG 目录
            await WritePeFilesAsync(bootLetter, dataLetter, optionalImagePath, progress, percentProgress, cancellationToken)
                .ConfigureAwait(false);

            // 4. 重建独立 BCD（与盘符无关，重写一遍最稳妥）
            await CreateBcdStoreAsync(bootLetter, progress, percentProgress, cancellationToken).ConfigureAwait(false);

            // 5. BIOS 引导代码（bootsect，可选）
            await TryWriteBiosBootCodeAsync(bootLetter, disk.DiskNumber, progress, cancellationToken).ConfigureAwait(false);
            percentProgress?.Report(96);
        }
        finally
        {
            // 6. 恢复隐藏：只移除本次临时分配的盘符
            if (assignedBootLetter)
            {
                try
                {
                    await HideBootPartitionAsync(bootLetter!, progress, cancellationToken).ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    progress.Report("警告：启动分区盘符未能移除（不影响启动）：" + ex.Message);
                }
            }
        }

        progress.Report("✓ PE 启动盘升级完成（数据分区未改动）。");
        percentProgress?.Report(100);
    }

    /// <summary>在指定物理磁盘上按卷标找可见分区盘符（找不到或不在该盘返回 null）。</summary>
    private static string? FindOwnVolumeLetter(string label, int diskNumber)
    {
        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            if (!drive.IsReady || !string.Equals(drive.VolumeLabel, label, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            string root = Path.GetPathRoot(drive.Name) ?? drive.Name;
            if (root.Length >= 3 && TryGetDiskNumber(root) == diskNumber)
            {
                return root[..2].ToUpperInvariant();
            }
        }

        return null;
    }

    /// <summary>diskpart list volume 里按卷标找卷号（用于给隐藏分区临时分配盘符）。</summary>
    private static async Task<int?> FindVolumeNumberAsync(string label, CancellationToken cancellationToken)
    {
        string output = await RunDiskpartAsync("list volume", cancellationToken).ConfigureAwait(false);
        foreach (string line in output.Split('\n'))
        {
            if (line.IndexOf(label, StringComparison.OrdinalIgnoreCase) < 0)
            {
                continue;
            }

            Match match = Regex.Match(line, @"(\d+)");
            if (match.Success && int.TryParse(match.Groups[1].Value, out int number))
            {
                return number;
            }
        }

        return null;
    }

    /// <summary>按内置 PE 体积估算启动分区大小（boot.wim + boot.sdi + 引导文件 + FAT32 开销）。</summary>
    private static int EstimateBootPartitionMb()
    {
        long bytes = 0;
        string pe = BundledPayload.PeDirectory;
        foreach (string name in new[] { "boot.wim", "boot.sdi" })
        {
            try
            {
                var info = new FileInfo(Path.Combine(pe, name));
                bytes += info.Exists ? info.Length : 0;
            }
            catch
            {
                // 读取失败按 0 计，后面有下限兜底
            }
        }

        bytes += 512L * 1024 * 1024;                     // 引导文件 + FAT32 表/簇开销余量
        int mb = (int)((bytes + 1024 * 1024 - 1) / (1024 * 1024));
        mb = (mb + 511) / 512 * 512;                     // 512 MB 对齐
        return Math.Max(MinBootPartitionMb, mb);
    }

    /// <summary>生成 diskpart 脚本并执行：MBR + FAT32 启动分区 +（可选）exFAT 数据分区。</summary>
    private static async Task RunDiskpartScriptAsync(
        UsbDiskInfo disk, int bootPartitionMb, bool dualPartition, CancellationToken cancellationToken)
    {
        string bootLetter = PickFreeDriveLetter();
        string dataLetter = PickFreeDriveLetter(new[] { bootLetter });

        var lines = new List<string>
        {
            "select disk " + disk.DiskNumber,
            "clean",
        };

        // clean 后 create partition primary 会自动以 MBR 初始化磁盘（UEFI 可移动介质 + BIOS 引导都要求 MBR）

        if (dualPartition)
        {
            lines.Add($"create partition primary size={bootPartitionMb}");
        }
        else
        {
            lines.Add("create partition primary");
        }

        lines.Add($"format fs=fat32 quick label={BootPartitionLabel}");
        lines.Add("active");
        lines.Add($"assign letter={bootLetter}");

        if (dualPartition)
        {
            lines.Add("create partition primary");
            lines.Add($"format fs=exfat quick label={DataPartitionLabel}");
            lines.Add($"assign letter={dataLetter}");
        }

        await RunDiskpartAsync(string.Join("\r\n", lines), cancellationToken).ConfigureAwait(false);
    }

    /// <summary>去掉启动分区盘符，使其在资源管理器中隐藏（引导文件与 BCD 均使用 device boot 别名，与盘符无关）。</summary>
    private static async Task HideBootPartitionAsync(string bootLetter, IProgress<string> progress, CancellationToken cancellationToken)
    {
        progress.Report("正在隐藏启动分区（移除盘符）…");

        // 只移除挂载点，不改变分区类型，UEFI 固件仍可正常识别并启动
        await RunDiskpartAsync($"select volume {bootLetter}\r\nremove letter={bootLetter}", cancellationToken).ConfigureAwait(false);
        progress.Report("✓ 启动分区已隐藏，仅数据分区可见。");
    }

    /// <summary>执行一段 diskpart 脚本，返回其完整输出。</summary>
    private static async Task<string> RunDiskpartAsync(string script, CancellationToken cancellationToken)
    {
        string tempFile = Path.Combine(Path.GetTempPath(), $"firewin-usb-{Guid.NewGuid():N}.txt");
        await File.WriteAllTextAsync(tempFile, script + "\r\n", new UTF8Encoding(false), cancellationToken).ConfigureAwait(false);

        var output = new StringBuilder();
        try
        {
            var psi = new ProcessStartInfo("diskpart.exe")
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            psi.ArgumentList.Add("/s");
            psi.ArgumentList.Add(tempFile);

            using var process = new Process { StartInfo = psi };
            process.OutputDataReceived += (_, e) => { if (e.Data is not null) { lock (output) { output.AppendLine(e.Data); } } };
            process.ErrorDataReceived += (_, e) => { if (e.Data is not null) { lock (output) { output.AppendLine(e.Data); } } };
            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

            string text;
            lock (output) { text = output.ToString().Trim(); }
            if (process.ExitCode != 0)
            {
                throw new DeployException($"diskpart 执行失败（退出码 {process.ExitCode}）：\n{text}");
            }

            return text;
        }
        finally
        {
            try { File.Delete(tempFile); } catch { /* 清理临时脚本失败不影响结果 */ }
        }
    }

    /// <summary>按卷标轮询等待新分区挂载（diskpart 完成后卷挂载可能有延迟）。</summary>
    private static async Task<string?> WaitForVolumeAsync(string label, CancellationToken cancellationToken)
    {
        for (int attempt = 0; attempt < 40; attempt++)
        {
            string? letter = FindDriveByLabel(label);
            if (letter is not null)
            {
                return letter;
            }

            await Task.Delay(500, cancellationToken).ConfigureAwait(false);
        }

        return null;
    }

    private static string? FindDriveByLabel(string label)
    {
        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            if (drive.IsReady &&
                string.Equals(drive.VolumeLabel, label, StringComparison.OrdinalIgnoreCase))
            {
                return Path.GetPathRoot(drive.Name)?.TrimEnd('\\').ToUpperInvariant();
            }
        }

        return null;
    }

    private static string PickFreeDriveLetter(string[]? taken = null)
    {
        var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            string? root = Path.GetPathRoot(drive.Name);
            if (root is not null && root.Length >= 2)
            {
                used.Add(root[..2]);
            }
        }

        if (taken is not null)
        {
            foreach (string letter in taken)
            {
                used.Add(letter);
            }
        }

        for (char letter = 'D'; letter <= 'Z'; letter++)
        {
            string candidate = letter + ":";
            if (!used.Contains(candidate))
            {
                return candidate;
            }
        }

        throw new DeployException("没有可用的盘符，请先释放部分盘符后重试。");
    }

    /// <summary>复制 PE 引导文件到 U 盘（官方 WinPE 介质布局），并在数据分区准备 FWIMG 镜像目录。</summary>
    private static async Task WritePeFilesAsync(
        string bootLetter, string? dataLetter, string? optionalImagePath,
        IProgress<string> progress, IProgress<int>? percentProgress, CancellationToken cancellationToken)
    {
        string pe = BundledPayload.PeDirectory;
        string bootWim = Path.Combine(pe, "boot.wim");
        string bootSdi = Path.Combine(pe, "boot.sdi");
        if (!File.Exists(bootWim) || !File.Exists(bootSdi))
        {
            throw new DeployException("内置 PE 资源不完整（缺 boot.wim / boot.sdi），无法制作启动盘。");
        }

        // \sources\boot.wim（官方布局）：耗时最长，占总体 18%–60%
        string targetWim = Path.Combine(bootLetter + @"\", "sources", "boot.wim");
        Directory.CreateDirectory(Path.GetDirectoryName(targetWim)!);
        progress.Report("复制 boot.wim 到 U 盘…");
        percentProgress?.Report(18);
        await DeployService.CopyFileWithProgressAsync(
            bootWim, targetWim, progress, cancellationToken,
            MapPercent(percentProgress, 18, 60)).ConfigureAwait(false);

        // \boot\boot.sdi
        string targetSdi = Path.Combine(bootLetter + @"\", "boot", "boot.sdi");
        Directory.CreateDirectory(Path.GetDirectoryName(targetSdi)!);
        File.Copy(bootSdi, targetSdi, overwrite: true);

        // bootmgr（BIOS）与 bootmgfw.efi（UEFI）：来自当前系统的 Windows 引导文件
        string bootmgr = CopyHostBootFile(
            @"C:\Windows\Boot\PCAT\bootmgr", Path.Combine(bootLetter + @"\", "bootmgr"), progress);
        string efiDir = Path.Combine(bootLetter + @"\", "EFI", "Boot");
        Directory.CreateDirectory(efiDir);
        _ = CopyHostBootFile(
            @"C:\Windows\Boot\EFI\bootmgfw.efi", Path.Combine(efiDir, "bootx64.efi"), progress);

        // UEFI 的 bootmgfw 固定按 \EFI\Microsoft\Boot\ 找引导配置与资源，缺了就是 0xc000000f
        string efiMicrosoftBootDir = Path.Combine(bootLetter + @"\", "EFI", "Microsoft", "Boot");
        Directory.CreateDirectory(efiMicrosoftBootDir);
        _ = CopyHostBootFile(
            @"C:\Windows\Boot\EFI\bootmgfw.efi", Path.Combine(efiMicrosoftBootDir, "bootmgfw.efi"), progress);

        // 引导字体：bootmgr / bootmgfw 缺少 wgl4_boot.ttf 时只显示方块，进度条界面也依赖它
        const string bootFontSource = @"C:\Windows\Boot\Fonts\wgl4_boot.ttf";
        if (File.Exists(bootFontSource))
        {
            CopyHostBootFile(bootFontSource, Path.Combine(bootLetter + @"\", "boot", "fonts", "wgl4_boot.ttf"), progress);
            CopyHostBootFile(bootFontSource, Path.Combine(efiMicrosoftBootDir, "fonts", "wgl4_boot.ttf"), progress);
        }

        progress.Report($"引导文件已写入（bootmgr：{bootmgr}）。");
        percentProgress?.Report(63);

        // FWIMG 镜像目录：优先数据分区（exFAT，无 4GB 限制）；单分区时落在启动分区根目录
        string imageHostLetter = dataLetter ?? bootLetter;
        string imagesDir = Path.Combine(imageHostLetter + @"\", ImageFolderName);
        Directory.CreateDirectory(imagesDir);
        await File.WriteAllTextAsync(
            Path.Combine(imagesDir, "把系统镜像放到这里.txt"),
            "把 install.wim / install.esd，或完整的 Windows ISO 复制到本目录（可放多个）。\r\n" +
            "从 FireWin PE 启动后点「安装系统」，即可扫描 FWIMG 内的全部镜像；旧版 \\Images 目录也会兼容扫描。\r\n",
            new UTF8Encoding(false), cancellationToken).ConfigureAwait(false);
        progress.Report($"镜像目录已创建：{imagesDir}");
        percentProgress?.Report(66);

        // 可选：把选定的系统镜像一并复制到 FWIMG（大文件耗时最长的一段，占 66%–88%）
        if (!string.IsNullOrWhiteSpace(optionalImagePath))
        {
            string imageSource = optionalImagePath!;
            string? wimPath = imageSource;

            if (imageSource.EndsWith(".iso", StringComparison.OrdinalIgnoreCase))
            {
                progress.Report("挂载 ISO 并定位系统镜像…");
                IsoMountResult mount = await DeployService.MountIsoAsync(imageSource, cancellationToken).ConfigureAwait(false);
                wimPath = mount.InstallWimPath;
            }

            if (!File.Exists(wimPath))
            {
                throw new DeployException($"找不到系统镜像文件：{wimPath}");
            }

            string destination = Path.Combine(imagesDir, "install" + Path.GetExtension(wimPath).ToLowerInvariant());
            progress.Report($"复制系统镜像到 {imagesDir}（大文件可能需要数分钟）…");
            await DeployService.CopyFileWithProgressAsync(
                wimPath, destination, progress, cancellationToken,
                MapPercent(percentProgress, 66, 88)).ConfigureAwait(false);
        }

        percentProgress?.Report(88);
    }

    /// <summary>把一个子任务的 0–100 百分比映射到总体进度的 [from, to] 区间。</summary>
    private static IProgress<int>? MapPercent(IProgress<int>? sink, int from, int to) =>
        sink is null ? null : new SegmentPercentProgress(sink, from, to);

    private sealed class SegmentPercentProgress : IProgress<int>
    {
        private readonly IProgress<int> _sink;
        private readonly int _from;
        private readonly int _to;

        public SegmentPercentProgress(IProgress<int> sink, int from, int to)
        {
            _sink = sink;
            _from = from;
            _to = to;
        }

        public void Report(int value)
        {
            int clamped = Math.Clamp(value, 0, 100);
            _sink.Report(_from + (_to - _from) * clamped / 100);
        }
    }

    private static string CopyHostBootFile(string source, string destination, IProgress<string> progress)
    {
        if (!File.Exists(source))
        {
            throw new DeployException(
                $"当前系统缺少引导文件 {source}。\n请更换 Windows 10/11 系统运行本工具，或手动从安装介质复制后放入 U 盘。");
        }

        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        File.Copy(source, destination, overwrite: true);
        return source;
    }

    /// <summary>用 bcdedit /store 在 U 盘上创建独立 BCD（device boot 别名，与盘符无关）。</summary>
    private static async Task CreateBcdStoreAsync(
        string bootLetter, IProgress<string> progress, IProgress<int>? percentProgress, CancellationToken cancellationToken)
    {
        // UEFI 的 bootmgfw 只会去 \EFI\Microsoft\Boot\BCD 找配置，缺这一份启动时报 0xc000000f。
        // 两份 store 分开生成：BIOS 的加载器是 winload.exe，UEFI 是 winload.efi。
        percentProgress?.Report(89);
        string biosBcd = Path.Combine(bootLetter + @"\", "boot", "bcd");
        await CreateBcdStoreAtAsync(biosBcd, efi: false, progress, cancellationToken).ConfigureAwait(false);

        progress.Report("正在写入 UEFI 引导配置（EFI\\Microsoft\\Boot\\BCD）…");
        percentProgress?.Report(92);
        string efiBcd = Path.Combine(bootLetter + @"\", "EFI", "Microsoft", "Boot", "BCD");
        await CreateBcdStoreAtAsync(efiBcd, efi: true, progress, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    /// 在任意路径创建 PE 引导 BCD（U 盘与 ISO 共用同一套配置）。
    /// device boot / ramdisk=[boot] 都是别名，与盘符无关，因此复制进 ISO 后依然有效。
    /// </summary>
    /// <param name="efi">true 生成 UEFI 用的 store（加载器 winload.efi），false 生成 BIOS 用的（winload.exe）。</param>
    public static async Task CreateBcdStoreAtAsync(
        string bcdPath, bool efi, IProgress<string> progress, CancellationToken cancellationToken)
    {
        string bcd = bcdPath;
        Directory.CreateDirectory(Path.GetDirectoryName(bcd)!);

        // /createstore 只能创建新文件，而升级已有启动盘时 BCD 已经存在。
        // 先在临时文件里生成整份 store，成功后再替换目标文件，避免中途失败留下不能启动的残缺 BCD。
        string staging = bcd + ".new";
        if (File.Exists(staging))
        {
            File.SetAttributes(staging, FileAttributes.Normal);
            File.Delete(staging);
        }
        bcd = staging;

        const string ramdiskOptions = "{ramdiskoptions}";

        // 注意：/store 与 BCD 路径必须是两个独立参数，合并会被 ArgumentList 整体加引号导致 bcdedit 解析失败
        Task<string> Bcd(params string[] rest) =>
            RunBcdEditAsync(new[] { "/store", bcd }.Concat(rest).ToArray(), cancellationToken);

        progress.Report("正在生成引导配置（BCD）…");

        await RunBcdEditAsync(new[] { "/createstore", bcd }, cancellationToken).ConfigureAwait(false);
        await Bcd("/create", "{bootmgr}", "/d", "FireWin Boot Manager").ConfigureAwait(false);
        await Bcd("/set", "{bootmgr}", "device", "boot").ConfigureAwait(false);
        await Bcd("/set", "{bootmgr}", "timeout", "0").ConfigureAwait(false);

        await Bcd("/create", ramdiskOptions, "/d", "FireWin RAMDisk Options").ConfigureAwait(false);
        await Bcd("/set", ramdiskOptions, "ramdisksdidevice", "boot").ConfigureAwait(false);
        await Bcd("/set", ramdiskOptions, "ramdisksdipath", @"\boot\boot.sdi").ConfigureAwait(false);

        string entry = await Bcd("/create", "/d", "FireWin PE", "/application", "osloader").ConfigureAwait(false);
        Match match = GuidRegex.Match(entry);
        if (!match.Success)
        {
            throw new DeployException($"无法从 bcdedit 输出解析启动项 GUID：{entry}");
        }

        string loaderGuid = match.Value;
        string ramdisk = $"ramdisk=[boot]\\sources\\boot.wim,{ramdiskOptions}";
        await Bcd("/set", loaderGuid, "device", ramdisk).ConfigureAwait(false);
        await Bcd("/set", loaderGuid, "osdevice", ramdisk).ConfigureAwait(false);
        // 加载器路径必须与固件类型匹配：BIOS 走 winload.exe，UEFI 走 winload.efi
        await Bcd("/set", loaderGuid, "path",
            efi ? @"\Windows\System32\boot\winload.efi" : @"\Windows\System32\boot\winload.exe").ConfigureAwait(false);
        await Bcd("/set", loaderGuid, "systemroot", @"\Windows").ConfigureAwait(false);
        await Bcd("/set", loaderGuid, "detecthal", "yes").ConfigureAwait(false);
        await Bcd("/set", loaderGuid, "winpe", "yes").ConfigureAwait(false);

        await Bcd("/set", "{bootmgr}", "default", loaderGuid).ConfigureAwait(false);
        await Bcd("/set", "{bootmgr}", "displayorder", loaderGuid).ConfigureAwait(false);

        // 整份 store 写完后才替换正式文件：升级时旧的 BCD 一直可用，替换失败也不影响启动
        if (File.Exists(bcdPath))
        {
            File.SetAttributes(bcdPath, FileAttributes.Normal);
        }
        File.Move(bcd, bcdPath, overwrite: true);

        progress.Report("✓ 引导配置（BCD）已生成。");
    }

    /// <summary>查找 bootsect.exe 并写入 BIOS 引导代码；找不到时仅警告（UEFI 启动不受影响）。</summary>
    private static async Task TryWriteBiosBootCodeAsync(string bootLetter, int diskNumber, IProgress<string> progress, CancellationToken cancellationToken)
    {
        string? bootsect = FindBootsectExe();
        if (bootsect is null)
        {
            progress.Report("⚠ 未找到 bootsect.exe：Legacy BIOS（传统）启动可能不可用，UEFI 启动不受影响。");
            progress.Report("  安装 Windows ADK 后重新制作即可补全 BIOS 引导代码。");
            return;
        }

        progress.Report("正在写入 BIOS（Legacy）引导代码…");
        var psi = new ProcessStartInfo(bootsect)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        psi.ArgumentList.Add("/nt60");
        psi.ArgumentList.Add(bootLetter + ":");
        psi.ArgumentList.Add("/force");
        psi.ArgumentList.Add("/mbr");

        using var process = new Process { StartInfo = psi };
        process.Start();
        string output = await process.StandardOutput.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        if (process.ExitCode != 0)
        {
            progress.Report($"⚠ bootsect 失败（退出码 {process.ExitCode}）：{output.Trim()}");
            return;
        }

        progress.Report($"✓ BIOS 引导代码已写入（磁盘 {diskNumber}）。UEFI 与 Legacy BIOS 均可启动。");
    }

    private static string? FindBootsectExe()
    {
        string baseDir = AppContext.BaseDirectory;
        string[] localCandidates =
        {
            Path.Combine(baseDir, "bootsect.exe"),
            Path.Combine(baseDir, "tools", "bootsect.exe"),
        };
        foreach (string candidate in localCandidates)
        {
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        // ADK：KitsRoot10 → Deployment Tools 下搜索（目录有限，递归可接受）
        try
        {
            string? kitsRoot = Registry.GetValue(
                @"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots",
                "KitsRoot10", null) as string;
            if (!string.IsNullOrEmpty(kitsRoot))
            {
                string deploymentTools = Path.Combine(kitsRoot, "Assessment and Deployment Kit", "Deployment Tools");
                if (Directory.Exists(deploymentTools))
                {
                    return Directory.EnumerateFiles(deploymentTools, "bootsect.exe", SearchOption.AllDirectories).FirstOrDefault();
                }
            }
        }
        catch
        {
            // ADK 检测失败按缺失处理
        }

        return null;
    }

    private static async Task<string> RunBcdEditAsync(string[] args, CancellationToken cancellationToken)
    {
        var psi = new ProcessStartInfo("bcdedit.exe")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (string arg in args)
        {
            psi.ArgumentList.Add(arg);
        }

        using var process = new Process { StartInfo = psi };
        process.Start();
        string output = await process.StandardOutput.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
        string error = await process.StandardError.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        if (process.ExitCode != 0)
        {
            throw new DeployException(
                $"bcdedit {string.Join(' ', args.Where(a => a != "/store" && !a.Contains(':')))} 失败（退出码 {process.ExitCode}）：{error.Trim()}{output.Trim()}");
        }

        return output.Trim();
    }

    #endregion

    /// <summary>kernel32 原生声明（盘符 → 物理磁盘映射）。</summary>
    private static class NativeMethods
    {
        public const uint GenericRead = 0x80000000;
        public const int IoctlVolumeGetVolumeDiskExtents = 0x00560000;
        public const int IoctlDiskGetDriveGeometry = 0x00070000;

        [StructLayout(LayoutKind.Sequential)]
        public struct DiskExtent
        {
            public int DiskNumber;
            public long StartingOffset;
            public long ExtentLength;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct VolumeDiskExtents
        {
            public int NumberOfDiskExtents;
            public DiskExtent Extent;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct DiskGeometry
        {
            public long Cylinders;
            public int MediaType;
            public int TracksPerCylinder;
            public int SectorsPerTrack;
            public int BytesPerSector;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern SafeFileHandleVolume CreateFileW(
            string fileName,
            uint desiredAccess,
            int shareMode,
            IntPtr securityAttributes,
            FileMode creationDisposition,
            int flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool DeviceIoControl(
            SafeFileHandleVolume handle,
            int ioControlCode,
            IntPtr inBuffer,
            int inBufferSize,
            out VolumeDiskExtents outBuffer,
            int outBufferSize,
            out int bytesReturned,
            IntPtr overlapped);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool DeviceIoControl(
            SafeFileHandleVolume handle,
            int ioControlCode,
            IntPtr inBuffer,
            int inBufferSize,
            out DiskGeometry outBuffer,
            int outBufferSize,
            out int bytesReturned,
            IntPtr overlapped);
    }

    /// <summary>卷句柄包装（CloseHandle 由 SafeHandle 管理）。</summary>
    public sealed class SafeFileHandleVolume : Microsoft.Win32.SafeHandles.SafeHandleMinusOneIsInvalid
    {
        public SafeFileHandleVolume() : base(true)
        {
        }

        protected override bool ReleaseHandle() => CloseHandle(handle);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);
    }
}
