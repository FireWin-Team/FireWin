using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace FireWin.Services;

/// <summary>
/// 生成可启动的 PE ISO 镜像：用 Windows 自带的 IMAPI2FS 组件写出 UDF + ISO9660 + Joliet 桥接盘，
/// 再补写 El Torito 引导目录实现 UEFI + Legacy BIOS 双引导，不依赖 ADK / oscdimg。
///
/// 为什么必须有 UDF：
///   VMware 的 EFI 固件只把「带 UDF 文件系统」的光盘挂载成可读文件系统；纯 ISO9660 + Joliet 的盘
///   在 EFI Shell 里只能看到空目录，cdboot 枚举不到 \bootmgr.efi，直接报 Not Found。
///   微软官方安装介质同样是 oscdimg -u2 的 UDF 桥接盘，这里用 IMAPI2FS 达到同样结构。
///
/// 目录布局与微软官方安装介质保持一致（UDF 承载长名，无需 8.3 截断）：
///   \BOOTMGR  \BOOTMGR.EFI  \BOOT\BCD  \BOOT\BOOT.SDI  \BOOT\ETFSBOOT.COM  \SOURCES\BOOT.WIM
///   \EFI\BOOT\BOOTX64.EFI  \EFI\MICROSOFT\BOOT\BOOTMGFW.EFI  \EFI\MICROSOFT\BOOT\BCD
///
/// El Torito 引导目录：
///   IMAPI2FS 只允许一个引导项，这里先写入 EFI 项，生成后再把引导目录改写成
///   「默认项（x86 / Legacy BIOS）= etfsboot.com + 最终分区（0x91 / EFI）= efisys」，
///   与官方介质一致，因此同一张盘在 UEFI 与 Legacy BIOS 下都能启动。
/// </summary>
public static class IsoImageService
{
    private const int SectorSize = 2048;

    /// <summary>El Torito 引导项的扇区计数以 512 字节为单位。</summary>
    private const int BootSectorSize = 512;

    /// <summary>IMAPI2FS 的文件系统组合：ISO9660(1) | Joliet(2) | UDF(4)。</summary>
    private const int FileSystemsToCreate = 7;

    /// <summary>本机安装介质自带的 BCD 是否可用（可用时生成 ISO 不需要管理员权限）。</summary>
    public static bool HostBcdAvailable
    {
        get
        {
            string? pcat = FindHostBcd(@"C:\Windows\Boot\DVD\PCAT\BCD");
            return pcat is not null || FindHostBcd(@"C:\Windows\Boot\DVD\EFI\BCD") is not null;
        }
    }

    /// <summary>本机 Windows 安装介质自带的引导配置：device boot / ramdisk=[boot]，与盘符无关。</summary>
    private static string? FindHostBcd(string path)
    {
        try
        {
            if (File.Exists(path) && new FileInfo(path).Length > 0)
            {
                return path;
            }
        }
        catch (IOException)
        {
            // 受保护文件读取失败时按缺失处理
        }
        catch (UnauthorizedAccessException)
        {
            // 同上
        }

        return null;
    }

    /// <summary>
    /// 生成 PE ISO。
    /// </summary>
    /// <param name="outputPath">ISO 输出路径。</param>
    /// <param name="optionalImagePath">可选：随 ISO 一起存放的系统镜像（ISO / WIM / ESD），会放到 \FWIMG。</param>
    /// <param name="progress">进度文本回调。</param>
    /// <param name="percentProgress">可选：总体进度 0–100（界面进度条）。</param>
    /// <param name="cancellationToken">取消令牌。</param>
    /// <returns>ISO 的完整路径。</returns>
    public static async Task<string> CreatePeIsoAsync(
        string outputPath,
        string? optionalImagePath,
        IProgress<string> progress,
        IProgress<int>? percentProgress,
        CancellationToken cancellationToken)
    {
        percentProgress?.Report(1);
        if (!BundledPayload.TryGetPePayload(out string missing))
        {
            throw new DeployException($"内置 PE 资源不完整（缺 {missing}），无法生成 ISO。请使用完整分发的程序包。");
        }

        string peDirectory = BundledPayload.PeDirectory;
        string bootWim = Path.Combine(peDirectory, "boot.wim");
        string bootSdi = Path.Combine(peDirectory, "boot.sdi");

        string bootmgr = RequireHostFile(@"C:\Windows\Boot\PCAT\bootmgr", "bootmgr（BIOS 引导管理器）");
        string etfsboot = RequireHostFile(@"C:\Windows\Boot\DVD\PCAT\etfsboot.com", "etfsboot.com（光盘引导扇区）");
        string bootmgfw = RequireHostFile(@"C:\Windows\Boot\EFI\bootmgfw.efi", "bootmgfw.efi（UEFI 引导管理器）");
        // 卷根 \bootmgr.efi 是 efisys 里 cdboot 唯一会加载的文件；官方介质用的是 bootmgr.efi，
        // 不是 bootmgfw.efi（两者大小与入口都不同），缺失时才退回 bootmgfw。
        string bootmgrEfi = File.Exists(@"C:\Windows\Boot\EFI\bootmgr.efi")
            ? @"C:\Windows\Boot\EFI\bootmgr.efi"
            : bootmgfw;
        string efisys = FindEfiBootImage()
            ?? throw new DeployException(
                "当前系统缺少 efisys_noprompt.bin / efisys.bin（UEFI 光盘引导镜像）。\n" +
                "请确认 C:\\Windows\\Boot\\DVD\\EFI 目录完整，或改用「写入 U 盘」。");
        string? bootFont = File.Exists(@"C:\Windows\Boot\Fonts\wgl4_boot.ttf")
            ? @"C:\Windows\Boot\Fonts\wgl4_boot.ttf"
            : null;

        cancellationToken.ThrowIfCancellationRequested();

        string? mountedIso = null;
        string workDirectory = Path.Combine(Path.GetTempPath(), "firewin-iso-" + Guid.NewGuid().ToString("N"));
        string stage = Path.Combine(workDirectory, "stage");
        Directory.CreateDirectory(stage);

        try
        {
            // ---- 可选系统镜像（ISO 时挂载并定位 install.wim / install.esd） ----
            string? imagePath = null;
            if (!string.IsNullOrWhiteSpace(optionalImagePath))
            {
                string candidate = optionalImagePath!.Trim().Trim('"');
                if (!File.Exists(candidate))
                {
                    throw new DeployException($"找不到系统镜像：{candidate}");
                }

                if (candidate.EndsWith(".iso", StringComparison.OrdinalIgnoreCase))
                {
                    progress.Report("挂载 ISO 并定位系统镜像（install.wim / install.esd）…");
                    percentProgress?.Report(2);
                    IsoMountResult mount = await DeployService.MountIsoAsync(candidate, cancellationToken).ConfigureAwait(false);
                    mountedIso = candidate;
                    imagePath = mount.InstallWimPath;
                }
                else
                {
                    imagePath = candidate;
                }
            }

            // ---- 引导配置 BCD：BIOS 与 UEFI 各取本机安装介质里对应的那一份 ----
            // 两份分开用：PCAT 的 BCD 给 \BOOT\BCD，EFI 的 BCD 给 \EFI\Microsoft\Boot\BCD。
            string biosBcdPath = Path.Combine(workDirectory, "bcd-bios");
            string efiBcdPath = Path.Combine(workDirectory, "bcd-efi");
            string? hostPcatBcd = FindHostBcd(@"C:\Windows\Boot\DVD\PCAT\BCD");
            string? hostEfiBcd = FindHostBcd(@"C:\Windows\Boot\DVD\EFI\BCD");

            if (hostPcatBcd is not null)
            {
                progress.Report("正在准备引导配置（BCD）…");
                File.Copy(hostPcatBcd, biosBcdPath, overwrite: true);
            }

            if (hostEfiBcd is not null)
            {
                File.Copy(hostEfiBcd, efiBcdPath, overwrite: true);
            }

            if (hostPcatBcd is null || hostEfiBcd is null)
            {
                // 本机介质不全时用 bcdedit 逐个生成（BIOS / UEFI 的加载器不同，不能共用一份）
                progress.Report("正在生成 ISO 引导配置（BCD）…");
                if (hostPcatBcd is null)
                {
                    await UsbBootService.CreateBcdStoreAtAsync(
                        biosBcdPath, efi: false, progress, cancellationToken).ConfigureAwait(false);
                }

                if (hostEfiBcd is null)
                {
                    await UsbBootService.CreateBcdStoreAtAsync(
                        efiBcdPath, efi: true, progress, cancellationToken).ConfigureAwait(false);
                }
            }

            percentProgress?.Report(6);

            // ---- 组装目录树（官方介质布局） ----
            progress.Report("正在准备 ISO 目录树…");
            StageFile(stage, @"bootmgr", bootmgr);
            StageFile(stage, @"bootmgr.efi", bootmgrEfi);
            StageFile(stage, @"boot\boot.sdi", bootSdi);
            StageFile(stage, @"boot\bcd", biosBcdPath);
            StageFile(stage, @"boot\etfsboot.com", etfsboot);
            StageFile(stage, @"efi\boot\bootx64.efi", bootmgfw);
            StageFile(stage, @"efi\microsoft\boot\bootmgfw.efi", bootmgfw);
            StageFile(stage, @"efi\microsoft\boot\bcd", efiBcdPath);
            StageFile(stage, @"sources\boot.wim", bootWim);

            if (bootFont is not null)
            {
                StageFile(stage, @"boot\fonts\wgl4_boot.ttf", bootFont);
                StageFile(stage, @"efi\microsoft\boot\fonts\wgl4_boot.ttf", bootFont);
            }

            if (!string.IsNullOrEmpty(imagePath))
            {
                string extension = Path.GetExtension(imagePath).ToUpperInvariant();
                StageFile(
                    stage,
                    UsbBootService.ImageFolderName + @"\INSTALL" + extension,
                    imagePath);
            }

            cancellationToken.ThrowIfCancellationRequested();
            percentProgress?.Report(10);

            string fullOutputPath = Path.GetFullPath(outputPath);
            Directory.CreateDirectory(Path.GetDirectoryName(fullOutputPath)!);
            if (File.Exists(fullOutputPath))
            {
                File.SetAttributes(fullOutputPath, FileAttributes.Normal);
            }

            progress.Report($"正在生成 UDF + ISO9660 + Joliet 镜像：{fullOutputPath} …");
            using (var output = new FileStream(fullOutputPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            {
                WriteImage(stage, efisys, output);
            }

            percentProgress?.Report(92);
            progress.Report("正在补写 BIOS 引导项…");
            PatchBiosBootEntry(fullOutputPath);

            progress.Report($"✓ ISO 生成完成：{fullOutputPath}");
            percentProgress?.Report(100);
            return fullOutputPath;
        }
        finally
        {
            if (mountedIso is not null)
            {
                DeployService.TryDismountIso(mountedIso);
            }

            try
            {
                Directory.Delete(workDirectory, true);
            }
            catch
            {
                // 临时目录清理失败不影响生成结果
            }
        }
    }

    #region 宿主引导文件

    private static string RequireHostFile(string path, string description)
    {
        if (!File.Exists(path))
        {
            throw new DeployException(
                $"当前系统缺少 {description}：{path}\n" +
                "生成 ISO 需要 Windows 10/11 自带的安装介质引导文件；请更换系统后重试，或改用「写入 U 盘」。");
        }

        return path;
    }

    /// <summary>优先取无按键提示的 efisys_noprompt.bin，其次 efisys.bin。</summary>
    private static string? FindEfiBootImage()
    {
        const string root = @"C:\Windows\Boot\DVD\EFI";
        if (!Directory.Exists(root))
        {
            return null;
        }

        string[] names = { "efisys_noprompt.bin", "efisys.bin" };
        foreach (string name in names)
        {
            string preferred = Path.Combine(root, "en-US", name);
            if (File.Exists(preferred))
            {
                return preferred;
            }
        }

        foreach (string name in names)
        {
            string? found = Directory.EnumerateFiles(root, name, SearchOption.AllDirectories).FirstOrDefault();
            if (found is not null)
            {
                return found;
            }
        }

        return null;
    }

    /// <summary>把宿主文件放进暂存目录；同一卷时建硬链接，避免复制 1 GB 的 boot.wim。</summary>
    private static void StageFile(string stage, string relativePath, string sourcePath)
    {
        if (!File.Exists(sourcePath))
        {
            throw new DeployException($"找不到文件：{sourcePath}");
        }

        string target = Path.Combine(stage, relativePath);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);

        if (CreateHardLink(target, sourcePath, IntPtr.Zero))
        {
            return;
        }

        File.Copy(sourcePath, target, overwrite: true);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLink(
        string lpFileName, string lpExistingFileName, IntPtr lpSecurityAttributes);

    #endregion

    #region IMAPI2FS 写镜像

    /// <summary>用 IMAPI2FS 生成 UDF + ISO9660 + Joliet 桥接镜像并导出到输出流。</summary>
    private static void WriteImage(string stage, string efisys, Stream output)
    {
        dynamic image;
        try
        {
            image = CreateComObject("IMAPI2FS.MsftFileSystemImage");
        }
        catch (COMException ex)
        {
            throw new DeployException("无法创建 IMAPI2FS 光盘映像组件：" + ex.Message + "\n请确认系统组件完整（Windows 10/11 自带）。");
        }

        image.FileSystemsToCreate = FileSystemsToCreate;
        image.VolumeName = "FireWinPE";
        image.FreeMediaBlocks = 0;              // 0 = 按内容自动扩展，不预占整张盘
        image.Root.AddTree(stage, false);

        // AssignBootImage 只接受 IStream（传路径会报类型转换失败），用 ADODB.Stream 载入文件。
        dynamic bootStream = CreateComObject("ADODB.Stream");
        bootStream.Type = 1;                    // adTypeBinary
        bootStream.Open();
        bootStream.LoadFromFile(efisys);

        dynamic bootOptions = CreateComObject("IMAPI2FS.BootOptions");
        bootOptions.AssignBootImage((object)bootStream);
        bootOptions.Emulation = 0;               // 不模拟（no emulation）
        bootOptions.PlatformId = 0xEF;           // EFI；BIOS 项由 PatchBiosBootEntry 补写
        image.BootImageOptions = (object)bootOptions;

        dynamic result = image.CreateResultImage();
        var stream = (IStream)result.ImageStream;
        GC.KeepAlive(bootStream);

        stream.Seek(0, 0, IntPtr.Zero);
        byte[] buffer = new byte[1 << 20];
        IntPtr readPtr = Marshal.AllocHGlobal(sizeof(int));
        try
        {
            while (true)
            {
                stream.Read(buffer, buffer.Length, readPtr);
                int read = Marshal.ReadInt32(readPtr);
                if (read <= 0)
                {
                    break;
                }

                output.Write(buffer, 0, read);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(readPtr);
        }

        output.Flush();
    }

    private static dynamic CreateComObject(string progId)
    {
        Type? type = Type.GetTypeFromProgID(progId);
        if (type is null)
        {
            throw new DeployException($"当前系统缺少 {progId} 组件，无法生成 ISO。");
        }

        return Activator.CreateInstance(type)
            ?? throw new DeployException($"无法创建 {progId} 组件实例，无法生成 ISO。");
    }

    #endregion

    #region El Torito 引导目录补写

    /// <summary>
    /// IMAPI2FS 只允许一个 El Torito 引导项（本次写的是 EFI 项），而 Legacy BIOS 固件只读
    /// 引导目录偏移 32 的默认项。这里把引导目录重写成官方介质的样子：
    ///   校验项（x86）→ 默认项 = etfsboot.com → 最终分区（0x91 / EFI，1 项）→ 原 EFI 项。
    /// </summary>
    private static void PatchBiosBootEntry(string isoPath)
    {
        using var fs = new FileStream(isoPath, FileMode.Open, FileAccess.ReadWrite, FileShare.None, 1 << 20);

        byte[] bootRecord = ReadSector(fs, 17);
        if (bootRecord[0] != 0 || Encoding.ASCII.GetString(bootRecord, 1, 5) != "CD001")
        {
            throw new DeployException("ISO 结构异常：LBA 17 不是 El Torito 引导记录。");
        }

        uint catalogLba = BitConverter.ToUInt32(bootRecord, 71);

        byte[] pvd = ReadSector(fs, 16);
        uint rootExtent = BitConverter.ToUInt32(pvd, 158);
        uint rootSize = BitConverter.ToUInt32(pvd, 166);

        DirectoryRecord bootDirectory = FindRecord(
            ReadExtent(fs, rootExtent, rootSize), "BOOT", directoryOnly: true)
            ?? throw new DeployException("ISO 结构异常：找不到 \\BOOT 目录。");
        DirectoryRecord etfsboot = FindRecord(
            ReadExtent(fs, bootDirectory.Extent, bootDirectory.Size), "ETFSBOOT.COM", directoryOnly: false)
            ?? throw new DeployException("ISO 结构异常：找不到 \\BOOT\\ETFSBOOT.COM。");

        byte[] catalog = ReadSector(fs, catalogLba);
        if (catalog[32] != 0x88)
        {
            throw new DeployException("ISO 引导目录结构异常：偏移 32 不是默认引导项，无法补写 BIOS 引导。");
        }

        byte[] patched = new byte[SectorSize];
        patched[0] = 0x01;                                  // 校验项：可启动
        patched[1] = 0x00;                                  // 平台：x86
        Encoding.ASCII.GetBytes("Windows PE").CopyTo(patched, 4);
        patched[30] = 0x55;
        patched[31] = 0xAA;

        ushort sum = 0;
        for (int i = 0; i < 32; i += 2)
        {
            sum += (ushort)(patched[i] | patched[i + 1] << 8);
        }

        PutLE16(patched, 28, (ushort)(0x10000 - sum));      // 校验和：16 位字累加为 0

        WriteBootEntry(
            patched, 32, etfsboot.Extent, (uint)((etfsboot.Size + BootSectorSize - 1) / BootSectorSize));

        patched[64] = 0x91;                                 // 最终分区头
        patched[65] = 0xEF;                                 // 平台：EFI
        PutLE16(patched, 66, 1);                            // 该分区下的引导项数量
        Array.Copy(catalog, 32, patched, 96, 32);           // 保留 IMAPI2FS 写入的 EFI 项

        fs.Position = (long)catalogLba * SectorSize;
        fs.Write(patched, 0, patched.Length);
        fs.Flush();
    }

    private static void WriteBootEntry(byte[] buffer, int offset, uint lba, uint sectorCount)
    {
        buffer[offset] = 0x88;                              // 可启动
        buffer[offset + 1] = 0x00;                          // 不模拟（no emulation）
        PutLE16(buffer, offset + 2, 0);                     // 加载段：0 = 传统 0x7C0
        buffer[offset + 4] = 0x00;                          // 系统类型
        PutLE16(buffer, offset + 6, (ushort)sectorCount);
        PutLE32(buffer, offset + 8, lba);
    }

    private static byte[] ReadSector(FileStream fs, uint lba)
    {
        byte[] buffer = new byte[SectorSize];
        fs.Position = (long)lba * SectorSize;
        fs.ReadExactly(buffer);
        return buffer;
    }

    private static byte[] ReadExtent(FileStream fs, uint lba, uint size)
    {
        byte[] buffer = new byte[size];
        fs.Position = (long)lba * SectorSize;
        fs.ReadExactly(buffer);
        return buffer;
    }

    /// <summary>在 ISO9660 目录数据里按名字找一条记录（文件名带 ";1" 版本号，需去掉后比较）。</summary>
    private static DirectoryRecord? FindRecord(byte[] directory, string name, bool directoryOnly)
    {
        int offset = 0;
        while (offset < directory.Length)
        {
            int length = directory[offset];
            if (length == 0)
            {
                offset = (offset / SectorSize + 1) * SectorSize;   // 扇区末尾补齐，跳到下一扇区
                continue;
            }

            if (offset + length > directory.Length)
            {
                break;
            }

            int nameLength = directory[offset + 32];
            bool isSelfOrParent = nameLength == 1 && directory[offset + 33] <= 1;
            bool isDirectory = (directory[offset + 25] & 0x02) != 0;

            if (!isSelfOrParent && (!directoryOnly || isDirectory))
            {
                string entryName = Encoding.ASCII.GetString(directory, offset + 33, nameLength);
                if (entryName.EndsWith(";1", StringComparison.Ordinal))
                {
                    entryName = entryName[..^2];
                }

                if (string.Equals(entryName, name, StringComparison.OrdinalIgnoreCase))
                {
                    return new DirectoryRecord(
                        BitConverter.ToUInt32(directory, offset + 2),
                        BitConverter.ToUInt32(directory, offset + 10));
                }
            }

            offset += length;
        }

        return null;
    }

    private readonly record struct DirectoryRecord(uint Extent, uint Size);

    private static void PutLE16(byte[] buffer, int offset, ushort value)
    {
        buffer[offset] = (byte)value;
        buffer[offset + 1] = (byte)(value >> 8);
    }

    private static void PutLE32(byte[] buffer, int offset, uint value)
    {
        buffer[offset] = (byte)value;
        buffer[offset + 1] = (byte)(value >> 8);
        buffer[offset + 2] = (byte)(value >> 16);
        buffer[offset + 3] = (byte)(value >> 24);
    }

    #endregion
}
