using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace FireWin.Services;

/// <summary>固件类型。</summary>
public enum FirmwareType
{
    Unknown = 0,
    Bios = 1,
    Uefi = 2,
}

/// <summary>
/// 桌面端写入 deploy.json、供 PE 端读取的部署参数。
/// 字段名使用 camelCase，与 pe/main.cpp 的解析键名一一对应。
/// </summary>
public sealed class DeployConfiguration
{
    /// <summary>镜像路径（桌面端原始路径）。</summary>
    public string WimPath { get; set; } = string.Empty;

    /// <summary>
    /// 镜像的卷 GUID 路径（\\?\Volume{...}\...）。
    /// PE 环境盘符与桌面端不一致，卷 GUID 路径不依赖盘符，优先使用。
    /// </summary>
    public string? WimVolumePath { get; set; }

    /// <summary>要应用的镜像索引（install.wim 内通常 1 = 第一个版本）。</summary>
    public int WimIndex { get; set; } = 1;

    /// <summary>目标系统分区盘符，如 D:</summary>
    public string TargetDrive { get; set; } = string.Empty;

    /// <summary>目标系统分区的卷 GUID 路径（PE 端据此重新解析盘符）。</summary>
    public string? TargetVolume { get; set; }

    /// <summary>系统分区（引导分区）盘符，如 C: 或 ESP 的临时盘符。</summary>
    public string SystemDrive { get; set; } = string.Empty;

    /// <summary>系统分区的卷 GUID 路径。</summary>
    public string? SystemVolume { get; set; }

    /// <summary>UEFI 或 BIOS。</summary>
    public string Firmware { get; set; } = "UEFI";

    /// <summary>本工具创建的临时 BCD 项 GUID，PE 端部署完成后用它清理。</summary>
    public string BootEntryGuid { get; set; } = string.Empty;

    /// <summary>PE 端日志路径。</summary>
    public string LogPath { get; set; } = string.Empty;

    /// <summary>部署成功后是否自动重启。</summary>
    public bool AutoReboot { get; set; } = true;
}

/// <summary>准备 PE 引导所需的输入。</summary>
public sealed class PeBootRequest
{
    /// <summary>PE 源目录：必须包含 boot.wim、boot.sdi 以及 Tools\FireWinPE.exe。</summary>
    public required string PeSourceDirectory { get; init; }

    /// <summary>PE 文件在目标磁盘上的落地目录，默认 C:\FireWinPE。</summary>
    public string StagingDirectory { get; init; } = BcdBootService.DefaultStagingDirectory;

    /// <summary>落地目录所在分区盘符（boot.wim 的实际存放分区），如 C:。</summary>
    public required string SystemDrive { get; init; }

    /// <summary>部署参数（镜像、目标分区等）。</summary>
    public required DeployConfiguration Deployment { get; init; }
}

/// <summary>准备完成后的会话信息，用于重启前做最后确认或失败回滚。</summary>
public sealed record PeBootSession(
    string BootEntryGuid,
    string StagingDirectory,
    string DeployConfigPath,
    FirmwareType Firmware,
    bool RamdiskOptionsCreated);

/// <summary>
/// 用 bcdedit 写入一次性 PE 引导（bootsequence），以及相关的部署参数落盘。
/// 关键点：
///   - /bootsequence 只影响下一次启动，不改变默认启动项；
///   - ramdisk 方式必须同时配置 {ramdiskoptions} 与 boot.sdi；
///   - 部署完成后由 PE 端删除临时项，系统回到正常启动流程。
/// </summary>
public sealed class BcdBootService
{
    public const string DefaultStagingDirectory = @"C:\FireWinPE";

    private const string RamdiskOptionsAlias = "{ramdiskoptions}";

    private static readonly JsonSerializerOptions DeployJsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        WriteIndented = true,
    };

    private static readonly Regex GuidRegex = new(
        @"\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}",
        RegexOptions.Compiled);

    #region 公共 API

    /// <summary>
    /// 写入一次性 PE 引导并生成 deploy.json。
    /// 成功后调用 <see cref="RestartComputerAsync"/> 重启即进入 PE。
    /// </summary>
    public async Task<PeBootSession> PreparePeBootAsync(PeBootRequest request, CancellationToken cancellationToken = default)
    {
        EnsureAdministrator();

        if (!OperatingSystem.IsWindows())
        {
            throw new BcdOperationException("仅支持 Windows。");
        }

        ValidateRequest(request);

        string staging = Path.GetFullPath(request.StagingDirectory);
        string systemDrive = NormalizeDrive(request.SystemDrive);

        // boot.wim 落在哪个分区，ramdisk 的 [X:] 就必须写哪个分区
        string? stagingRoot = GetRootPath(staging);
        if (stagingRoot is null || !string.Equals(stagingRoot.TrimEnd('\\'), systemDrive, StringComparison.OrdinalIgnoreCase))
        {
            throw new BcdOperationException($"落地目录 {staging} 不在系统分区 {systemDrive} 上。");
        }

        // staging 相对分区根目录的路径，例如 C:\FireWinPE → \FireWinPE
        string stagingRelative = "\\" + staging.Substring(stagingRoot.Length).Trim('\\');

        FirmwareType firmware = DetectFirmwareType();

        // 1. 把 PE 与 boot.wim 复制到目标磁盘，PE 启动时会以 ramdisk 方式从这里加载
        await Task.Run(() => CopyPePayload(request.PeSourceDirectory, staging), cancellationToken).ConfigureAwait(false);

        // 2. 组织部署参数（补齐卷 GUID 路径，避免 PE 盘符漂移）
        DeployConfiguration deploy = request.Deployment;
        deploy.WimVolumePath ??= TryBuildVolumePath(deploy.WimPath);
        deploy.TargetVolume ??= TryGetVolumeRoot(deploy.TargetDrive);
        deploy.SystemVolume ??= TryGetVolumeRoot(systemDrive);
        deploy.Firmware = firmware == FirmwareType.Uefi ? "UEFI" : "BIOS";
        deploy.LogPath = Path.Combine(staging, "deploy.log");

        string? createdEntry = null;
        bool ramdiskOptionsCreated = false;

        try
        {
            // 3. 配置 ramdisk 选项（boot.sdi 所在设备与路径）
            if (!await EntryExistsAsync(RamdiskOptionsAlias, cancellationToken).ConfigureAwait(false))
            {
                await RunBcdEditAsync(new[] { "/create", RamdiskOptionsAlias, "/d", "FireWin RAMDisk Options" }, cancellationToken).ConfigureAwait(false);
                ramdiskOptionsCreated = true;
            }
            else
            {
                // 已有 ramdisk 选项时复用，但会覆盖其指向；仅在本次会话内有效
                // （若该项不是本工具创建的，清理时不会删除它）
            }

            await RunBcdEditAsync(new[] { "/set", RamdiskOptionsAlias, "ramdisksdidevice", $"partition={systemDrive}" }, cancellationToken).ConfigureAwait(false);
            await RunBcdEditAsync(new[] { "/set", RamdiskOptionsAlias, "ramdisksdipath", $@"{stagingRelative}\boot.sdi" }, cancellationToken).ConfigureAwait(false);

            // 4. 创建 OSLOADER 项
            string entry = await CreateOsLoaderEntryAsync(cancellationToken).ConfigureAwait(false);
            createdEntry = entry;

            string wimOnDisk = $@"ramdisk=[{systemDrive}]{stagingRelative}\boot.wim,{RamdiskOptionsAlias}";
            await RunBcdEditAsync(new[] { "/set", entry, "device", wimOnDisk }, cancellationToken).ConfigureAwait(false);
            await RunBcdEditAsync(new[] { "/set", entry, "osdevice", wimOnDisk }, cancellationToken).ConfigureAwait(false);

            // 5. 引导文件路径：UEFI 用 winload.efi，Legacy 用 winload.exe
            string loaderPath = firmware == FirmwareType.Uefi
                ? @"\Windows\System32\boot\winload.efi"
                : @"\Windows\System32\winload.exe";
            await RunBcdEditAsync(new[] { "/set", entry, "path", loaderPath }, cancellationToken).ConfigureAwait(false);
            await RunBcdEditAsync(new[] { "/set", entry, "systemroot", @"\Windows" }, cancellationToken).ConfigureAwait(false);
            await RunBcdEditAsync(new[] { "/set", entry, "winpe", "yes" }, cancellationToken).ConfigureAwait(false);

            if (firmware == FirmwareType.Bios)
            {
                await RunBcdEditAsync(new[] { "/set", entry, "detecthal", "yes" }, cancellationToken).ConfigureAwait(false);
            }

            // 6. 写入 deploy.json（PE 端启动后读取）
            deploy.BootEntryGuid = entry;
            string configPath = Path.Combine(staging, "deploy.json");
            string json = JsonSerializer.Serialize(deploy, DeployJsonOptions);
            await File.WriteAllTextAsync(configPath, json, new UTF8Encoding(false), cancellationToken).ConfigureAwait(false);

            // 7. 仅下一次启动生效
            await RunBcdEditAsync(new[] { "/bootsequence", createdEntry }, cancellationToken).ConfigureAwait(false);

            return new PeBootSession(entry, staging, configPath, firmware, ramdiskOptionsCreated);
        }
        catch
        {
            // 任一环节失败都要回滚，避免机器下次启动卡在一个坏掉的 PE 项上
            await RollbackAsync(createdEntry, ramdiskOptionsCreated).ConfigureAwait(false);
            throw;
        }
    }

    /// <summary>删除本工具创建的临时 BCD 项（PE 端部署完成后调用等价逻辑）。</summary>
    public async Task<bool> RemoveTemporaryEntryAsync(string bootEntryGuid, bool deleteRamdiskOptions, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(bootEntryGuid))
        {
            return false;
        }

        bool ok = await TryRunBcdEditAsync(new[] { "/delete", bootEntryGuid, "/f" }, cancellationToken).ConfigureAwait(false);
        if (deleteRamdiskOptions)
        {
            await TryRunBcdEditAsync(new[] { "/delete", RamdiskOptionsAlias, "/f" }, cancellationToken).ConfigureAwait(false);
        }

        return ok;
    }

    /// <summary>立即重启（部署流程最后一步）。</summary>
    public static async Task RestartComputerAsync(CancellationToken cancellationToken = default)
    {
        await RunProcessAsync("shutdown.exe", new[] { "/r", "/t", "0" }, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>检测固件类型；GetFirmwareType 不可用时回退到 bcdedit 输出。</summary>
    public static FirmwareType DetectFirmwareType()
    {
        try
        {
            if (NativeMethods.GetFirmwareType(out NativeMethods.FirmwareTypeNative native) &&
                native is NativeMethods.FirmwareTypeNative.Bios or NativeMethods.FirmwareTypeNative.Uefi)
            {
                return native == NativeMethods.FirmwareTypeNative.Uefi ? FirmwareType.Uefi : FirmwareType.Bios;
            }
        }
        catch (EntryPointNotFoundException)
        {
            // Windows 7 等老系统没有该 API，走下方回退
        }

        try
        {
            string output = RunProcessAsync("bcdedit.exe", new[] { "/enum", "{current}" }, CancellationToken.None)
                .GetAwaiter().GetResult().Output;
            if (output.Contains("winload.efi", StringComparison.OrdinalIgnoreCase))
            {
                return FirmwareType.Uefi;
            }

            if (output.Contains("winload.exe", StringComparison.OrdinalIgnoreCase))
            {
                return FirmwareType.Bios;
            }
        }
        catch
        {
            // 忽略，返回 Unknown
        }

        return FirmwareType.Unknown;
    }

    /// <summary>把任意路径转换为卷 GUID 根路径（\\?\Volume{...}\），失败返回 null。</summary>
    public static string? TryGetVolumeRoot(string pathOrDrive)
    {
        string? root = GetRootPath(pathOrDrive);
        if (root is null)
        {
            return null;
        }

        var buffer = new StringBuilder(64);
        return NativeMethods.GetVolumeNameForVolumeMountPoint(root, buffer, buffer.Capacity)
            ? buffer.ToString()
            : null;
    }

    /// <summary>把文件路径转换为「卷 GUID 根 + 相对路径」，PE 端无需依赖盘符即可访问。</summary>
    public static string? TryBuildVolumePath(string filePath)
    {
        if (string.IsNullOrWhiteSpace(filePath))
        {
            return null;
        }

        string? root = GetRootPath(filePath);
        if (root is null)
        {
            return null;
        }

        string? volumeRoot = TryGetVolumeRoot(filePath);
        if (volumeRoot is null)
        {
            return null;
        }

        string relative = Path.GetRelativePath(root, Path.GetFullPath(filePath));
        return Path.Combine(volumeRoot, relative);
    }

    #endregion

    #region 内部实现

    private static void ValidateRequest(PeBootRequest request)
    {
        string source = Path.GetFullPath(request.PeSourceDirectory);
        if (!Directory.Exists(source))
        {
            throw new BcdOperationException($"PE 源目录不存在：{source}");
        }

        string bootWim = Path.Combine(source, "boot.wim");
        if (!File.Exists(bootWim))
        {
            throw new BcdOperationException($"缺少 boot.wim：{bootWim}");
        }

        // boot.sdi 是 ramdisk 引导必需的「磁盘映像」，缺它必然启动失败
        string bootSdi = Path.Combine(source, "boot.sdi");
        if (!File.Exists(bootSdi))
        {
            throw new BcdOperationException(
                $"缺少 boot.sdi：{bootSdi}。请从 ADK 的 Windows Preinstallation Environment\\amd64\\Media\\Boot\\ 复制。");
        }

        string peExe = Path.Combine(source, "Tools", "FireWinPE.exe");
        if (!File.Exists(peExe))
        {
            throw new BcdOperationException($"缺少 PE 端程序：{peExe}，请先编译 pe/ 工程。");
        }
    }

    /// <summary>把 boot.wim / boot.sdi / Tools 复制到落地目录。</summary>
    private static void CopyPePayload(string sourceDir, string stagingDir)
    {
        Directory.CreateDirectory(stagingDir);

        foreach (string fileName in new[] { "boot.wim", "boot.sdi" })
        {
            string src = Path.Combine(sourceDir, fileName);
            string dst = Path.Combine(stagingDir, fileName);
            if (File.Exists(dst) && new FileInfo(dst).Length == new FileInfo(src).Length)
            {
                continue; // 同尺寸视为已复制过，避免每次重复拷几百 MB
            }

            File.Copy(src, dst, overwrite: true);
        }

        string srcTools = Path.Combine(sourceDir, "Tools");
        if (Directory.Exists(srcTools))
        {
            CopyDirectory(srcTools, Path.Combine(stagingDir, "Tools"));
        }
    }

    private static void CopyDirectory(string sourceDir, string destinationDir)
    {
        Directory.CreateDirectory(destinationDir);
        foreach (string file in Directory.GetFiles(sourceDir))
        {
            File.Copy(file, Path.Combine(destinationDir, Path.GetFileName(file)), overwrite: true);
        }

        foreach (string dir in Directory.GetDirectories(sourceDir))
        {
            CopyDirectory(dir, Path.Combine(destinationDir, Path.GetFileName(dir)));
        }
    }

    private static async Task<bool> EntryExistsAsync(string entry, CancellationToken cancellationToken)
    {
        return await TryRunBcdEditAsync(new[] { "/enum", entry }, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>创建 OSLOADER 项并返回其 GUID（形如 {xxxxxxxx-...}）。</summary>
    private static async Task<string> CreateOsLoaderEntryAsync(CancellationToken cancellationToken)
    {
        ProcessResult result = await RunBcdEditAsync(
            new[] { "/create", "/d", "FireWin PE", "/application", "osloader" },
            cancellationToken).ConfigureAwait(false);

        Match match = GuidRegex.Match(result.Output);
        if (!match.Success)
        {
            throw new BcdOperationException($"无法从 bcdedit 输出中解析启动项 GUID：{result.Output}");
        }

        return match.Value;
    }

    private async Task RollbackAsync(string? entry, bool deleteRamdiskOptions)
    {
        if (!string.IsNullOrWhiteSpace(entry))
        {
            await TryRunBcdEditAsync(new[] { "/delete", entry, "/f" }, CancellationToken.None).ConfigureAwait(false);
        }

        if (deleteRamdiskOptions)
        {
            await TryRunBcdEditAsync(new[] { "/delete", RamdiskOptionsAlias, "/f" }, CancellationToken.None).ConfigureAwait(false);
        }
    }

    private static void EnsureAdministrator()
    {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
        {
            throw new BcdOperationException("bcdedit 需要管理员权限，请以管理员身份运行。");
        }
    }

    private static async Task<ProcessResult> RunBcdEditAsync(string[] args, CancellationToken cancellationToken)
    {
        ProcessResult result = await RunProcessAsync("bcdedit.exe", args, cancellationToken).ConfigureAwait(false);
        if (result.ExitCode != 0)
        {
            throw new BcdOperationException(
                $"bcdedit {string.Join(' ', args)} 失败（退出码 {result.ExitCode}）：{result.Output}");
        }

        return result;
    }

    private static async Task<bool> TryRunBcdEditAsync(string[] args, CancellationToken cancellationToken)
    {
        try
        {
            ProcessResult result = await RunProcessAsync("bcdedit.exe", args, cancellationToken).ConfigureAwait(false);
            return result.ExitCode == 0;
        }
        catch
        {
            return false;
        }
    }

    private static async Task<ProcessResult> RunProcessAsync(string fileName, string[] args, CancellationToken cancellationToken)
    {
        var psi = new ProcessStartInfo
        {
            FileName = fileName,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        foreach (string arg in args)
        {
            psi.ArgumentList.Add(arg);
        }

        var output = new StringBuilder();
        using var process = new Process { StartInfo = psi };
        process.OutputDataReceived += (_, e) => Append(output, e.Data);
        process.ErrorDataReceived += (_, e) => Append(output, e.Data);

        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        return new ProcessResult(process.ExitCode, output.ToString().Trim());
    }

    private static void Append(StringBuilder builder, string? line)
    {
        if (line is not null)
        {
            builder.AppendLine(line);
        }
    }

    private static string NormalizeDrive(string drive)
    {
        string value = drive.Trim().TrimEnd('\\');
        if (value.Length == 1)
        {
            value += ":";
        }

        return value.ToUpperInvariant();
    }

    private static string? GetRootPath(string pathOrDrive)
    {
        if (string.IsNullOrWhiteSpace(pathOrDrive))
        {
            return null;
        }

        string normalized = pathOrDrive.Trim();
        string? root = Path.GetPathRoot(Path.GetFullPath(normalized));
        if (string.IsNullOrEmpty(root))
        {
            return null;
        }

        // GetPathRoot("C:") 返回 "C:"，GetVolumeNameForVolumeMountPoint 需要 "C:\"
        if (root.Length == 2 && root[1] == ':')
        {
            root += "\\";
        }

        return root;
    }

    private readonly record struct ProcessResult(int ExitCode, string Output);

    /// <summary>kernel32 原生声明。</summary>
    private static class NativeMethods
    {
        internal enum FirmwareTypeNative
        {
            Unknown = 0,
            Bios = 1,
            Uefi = 2,
            Max = 3,
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetFirmwareType(out FirmwareTypeNative firmwareType);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetVolumeNameForVolumeMountPoint(
            string volumeMountPoint,
            StringBuilder volumeName,
            int bufferLength);
    }

    #endregion
}

/// <summary>BCD / 引导操作异常。</summary>
public sealed class BcdOperationException : Exception
{
    public BcdOperationException(string message) : base(message)
    {
    }
}
