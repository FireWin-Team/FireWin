using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace FireWin.Services;

/// <summary>
/// 生成可启动的 PE ISO 镜像：内置 ISO9660 + Joliet + El Torito 写入器，不依赖 ADK / oscdimg。
///
/// 目录布局与微软官方安装介质保持一致：
///   \BOOTMGR  \BOOT\BCD  \BOOT\BOOT.SDI  \SOURCES\BOOT.WIM
///   \EFI\BOOT\BOOTX64.EFI  \EFI\MICROSOFT\BOOT\BCD
///
/// 必须同时写两套目录名：
///   主卷（ISO9660）用大写全名（微软的 Microsoft 不能截成 MICROSOF，否则 UEFI 报 0xc000000f）；
///   再写一份 Joliet（UCS-2）长名卷，兼容只读 Joliet 的固件。
/// 微软官方介质靠 UDF 承载长名，本写入器用「ISO9660 全名 + Joliet」达到同样效果。
///
/// El Torito 引导目录：
///   默认项（x86 / Legacy BIOS）= C:\Windows\Boot\DVD\PCAT\etfsboot.com
///   附加项（EFI / UEFI）       = C:\Windows\Boot\DVD\EFI\&lt;语言&gt;\efisys_noprompt.bin
/// 两者都是当前 Windows 自带的安装介质引导文件，因此生成的 ISO 在 UEFI 与 Legacy BIOS 下都能启动。
/// </summary>
public static class IsoImageService
{
    private const int SectorSize = 2048;

    /// <summary>引导目录（El Torito Boot Catalog）：16=PVD，17=Joliet 卷描述符，18=引导记录，19=结束符。</summary>
    private const uint BootCatalogLba = 20;

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
        Directory.CreateDirectory(workDirectory);

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
            // 之前两份都写 PCAT 的 BCD，UEFI 下 bootmgfw 读到的配置与固件不匹配。
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

            // ---- 组装 ISO 目录树 ----
            var root = new IsoDirectory { Name = string.Empty, Path = "\\" };
            IsoDirectory boot = root.AddDirectory("BOOT");
            IsoDirectory sources = root.AddDirectory("SOURCES");
            IsoDirectory efi = root.AddDirectory("EFI");
            IsoDirectory efiBoot = efi.AddDirectory("BOOT");
            IsoDirectory efiMicrosoftBoot = efi.AddDirectory("MICROSOFT").AddDirectory("BOOT");

            root.AddFile("BOOTMGR", bootmgr);
            // UEFI 光盘引导镜像（efisys）里的 cdboot 只负责加载卷根的 \bootmgr.efi，
            // 再由它读 \EFI\Microsoft\Boot\BCD —— 官方介质卷根也有这个文件，缺了会立刻 Not Found。
            root.AddFile("BOOTMGR.EFI", bootmgfw);
            boot.AddFile("BOOT.SDI", bootSdi);
            boot.AddFile("BCD", biosBcdPath);
            sources.AddFile("BOOT.WIM", bootWim);
            efiBoot.AddFile("BOOTX64.EFI", bootmgfw);
            efiMicrosoftBoot.AddFile("BOOTMGFW.EFI", bootmgfw);
            efiMicrosoftBoot.AddFile("BCD", efiBcdPath);

            if (bootFont is not null)
            {
                boot.AddDirectory("FONTS").AddFile("WGL4_BOOT.TTF", bootFont);
                efiMicrosoftBoot.AddDirectory("FONTS").AddFile("WGL4_BOOT.TTF", bootFont);
            }

            if (!string.IsNullOrEmpty(imagePath))
            {
                string extension = Path.GetExtension(imagePath).ToUpperInvariant();
                root.AddDirectory(UsbBootService.ImageFolderName).AddFile("INSTALL" + extension, imagePath);
            }

            // ---- 目录编号（广度优先，根目录为 1） ----
            // ISO9660 §9.4 要求路径表按「层次 → 父目录编号 → 名字」升序，
            // 目录编号本身也是按这个顺序分配的，因此同级的子目录必须先按名字排序再加入。
            var directories = new List<IsoDirectory> { root };
            for (int i = 0; i < directories.Count; i++)
            {
                IsoDirectory directory = directories[i];
                directory.Index = i + 1;
                foreach (IsoDirectory child in SortDirectories(directory))
                {
                    child.Parent = directory;
                    child.ParentIndex = directory.Index;
                    directories.Add(child);
                }
            }

            foreach (IsoDirectory directory in directories)
            {
                directory.Length = MeasureDirectory(directory, joliet: false);
                directory.JolietLength = MeasureDirectory(directory, joliet: true);
            }

            // 路径表必须等目录 extent 分配完成后才能生成（记录里要写目录 extent）
            uint pathTableSectors = SectorsFor(MeasurePathTable(directories, joliet: false));
            uint jolietPathTableSectors = SectorsFor(MeasurePathTable(directories, joliet: true));

            // ---- 分配扇区（LBA）：主卷路径表 → Joliet 路径表 → 主卷目录 → Joliet 目录 ----
            uint lba = BootCatalogLba + 1;
            uint pathTableLeLba = lba;
            lba += pathTableSectors;
            uint pathTableBeLba = lba;
            lba += pathTableSectors;
            uint jolietPathTableLeLba = lba;
            lba += jolietPathTableSectors;
            uint jolietPathTableBeLba = lba;
            lba += jolietPathTableSectors;

            foreach (IsoDirectory directory in directories)
            {
                directory.Extent = lba;
                lba += (uint)(directory.Length / SectorSize);
            }

            foreach (IsoDirectory directory in directories)
            {
                directory.JolietExtent = lba;
                lba += (uint)(directory.JolietLength / SectorSize);
            }

            byte[] pathTableLe = BuildPathTable(directories, littleEndian: true, joliet: false);
            byte[] pathTableBe = BuildPathTable(directories, littleEndian: false, joliet: false);
            byte[] jolietPathTableLe = BuildPathTable(directories, littleEndian: true, joliet: true);
            byte[] jolietPathTableBe = BuildPathTable(directories, littleEndian: false, joliet: true);

            byte[] etfsbootBytes = File.ReadAllBytes(etfsboot);
            byte[] efisysBytes = File.ReadAllBytes(efisys);

            uint biosBootLba = lba;
            lba += SectorsFor((uint)etfsbootBytes.Length);
            uint efiBootLba = lba;
            lba += SectorsFor((uint)efisysBytes.Length);

            var files = new List<IsoFile>();
            foreach (IsoDirectory directory in directories)
            {
                files.AddRange(directory.Files);
            }

            foreach (IsoFile file in files)
            {
                file.Extent = lba;
                lba += SectorsFor(file.Size);
            }

            uint totalSectors = lba;

            // ---- 组装卷描述符与引导目录 ----
            byte[] pvd = BuildPrimaryVolumeDescriptor(
                totalSectors, (uint)pathTableLe.Length, pathTableLeLba, pathTableBeLba, root);
            byte[] svd = BuildJolietVolumeDescriptor(
                totalSectors, (uint)jolietPathTableLe.Length, jolietPathTableLeLba, jolietPathTableBeLba, root);
            byte[] bootRecord = BuildBootRecord();
            byte[] terminator = BuildTerminator();
            byte[] catalog = BuildBootCatalog(
                biosBootLba,
                (uint)((etfsbootBytes.Length + 511) / 512),
                efiBootLba,
                (uint)((efisysBytes.Length + 511) / 512));

            string fullOutputPath = Path.GetFullPath(outputPath);
            Directory.CreateDirectory(Path.GetDirectoryName(fullOutputPath)!);
            if (File.Exists(fullOutputPath))
            {
                File.SetAttributes(fullOutputPath, FileAttributes.Normal);
            }

            progress.Report($"正在写入 ISO（{DeployService.FormatBytes((ulong)totalSectors * SectorSize)}）…");
            percentProgress?.Report(8);

            using (var stream = new FileStream(fullOutputPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            {
                byte[] zeroSector = new byte[SectorSize];
                for (int i = 0; i < 16; i++)
                {
                    stream.Write(zeroSector, 0, zeroSector.Length);
                }

                stream.Write(pvd, 0, pvd.Length);
                stream.Write(svd, 0, svd.Length);
                stream.Write(bootRecord, 0, bootRecord.Length);
                stream.Write(terminator, 0, terminator.Length);
                stream.Write(catalog, 0, catalog.Length);
                WritePadded(stream, pathTableLe);
                WritePadded(stream, pathTableBe);
                WritePadded(stream, jolietPathTableLe);
                WritePadded(stream, jolietPathTableBe);

                foreach (IsoDirectory directory in directories)
                {
                    byte[] data = BuildDirectory(directory, joliet: false);
                    stream.Write(data, 0, data.Length);
                }

                foreach (IsoDirectory directory in directories)
                {
                    byte[] data = BuildDirectory(directory, joliet: true);
                    stream.Write(data, 0, data.Length);
                }

                WritePadded(stream, etfsbootBytes);
                WritePadded(stream, efisysBytes);

                // 文件数据是 ISO 的绝大部分体积：按已写入字节推进 8%–99%
                long totalBytes = (long)totalSectors * SectorSize;
                long writtenBytes = 0;
                foreach (IsoFile file in files)
                {
                    progress.Report($"写入 {file.IsoPath}（{DeployService.FormatBytes(file.Size)}）…");
                    CopyFileIntoIso(stream, file, progress, cancellationToken);
                    writtenBytes += file.Size;
                    if (percentProgress is not null)
                    {
                        percentProgress.Report(8 + (int)(91L * Math.Min(writtenBytes, totalBytes) / Math.Max(totalBytes, 1)));
                    }
                }

                stream.Flush();

                long expected = (long)totalSectors * SectorSize;
                if (stream.Length != expected)
                {
                    throw new DeployException($"ISO 长度校验失败：期望 {expected} 字节，实际 {stream.Length} 字节。");
                }
            }

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

    #endregion

    #region ISO9660 结构

    private static int RecordLength(int nameLength) => 33 + nameLength + (nameLength % 2 == 0 ? 1 : 0);

    private static uint SectorsFor(uint size) => (uint)((size + (ulong)SectorSize - 1) / SectorSize);

    /// <summary>计算目录数据区字节数（按 2048 对齐，包含跨扇区补齐）。</summary>
    private static uint MeasureDirectory(IsoDirectory directory, bool joliet)
    {
        long offset = 0;

        void AddRecord(int nameLength)
        {
            int length = RecordLength(nameLength);
            if (offset % SectorSize + length > SectorSize)
            {
                offset = (offset / SectorSize + 1) * SectorSize;
            }

            offset += length;
        }

        AddRecord(1); // "."
        AddRecord(1); // ".."

        foreach (DirectoryEntry entry in EnumerateEntries(directory, joliet))
        {
            AddRecord(entry.Name.Length);
        }

        long sectors = Math.Max(1, (offset + SectorSize - 1) / SectorSize);
        return (uint)(sectors * SectorSize);
    }

    private static byte[] BuildDirectory(IsoDirectory directory, bool joliet)
    {
        byte[] data = new byte[joliet ? directory.JolietLength : directory.Length];
        int offset = 0;

        void Add(byte[] name, uint extent, uint size, bool isDirectory)
        {
            int length = RecordLength(name.Length);
            if (offset % SectorSize + length > SectorSize)
            {
                offset = (offset / SectorSize + 1) * SectorSize;
            }

            WriteDirectoryRecord(data, offset, name, extent, size, isDirectory);
            offset += length;
        }

        uint selfExtent = joliet ? directory.JolietExtent : directory.Extent;
        uint selfSize = joliet ? directory.JolietLength : directory.Length;
        uint parentExtent = joliet ? directory.Parent?.JolietExtent ?? selfExtent : directory.Parent?.Extent ?? selfExtent;
        uint parentSize = joliet ? directory.Parent?.JolietLength ?? selfSize : directory.Parent?.Length ?? selfSize;

        Add(new byte[] { 0 }, selfExtent, selfSize, true);
        Add(new byte[] { 1 }, parentExtent, parentSize, true);

        foreach (DirectoryEntry entry in EnumerateEntries(directory, joliet))
        {
            if (entry.Directory is not null)
            {
                Add(entry.Name,
                    joliet ? entry.Directory.JolietExtent : entry.Directory.Extent,
                    joliet ? entry.Directory.JolietLength : entry.Directory.Length,
                    true);
            }
            else
            {
                IsoFile file = entry.File!;
                Add(entry.Name, file.Extent, file.Size, false);
            }
        }

        return data;
    }

    /// <summary>同级子目录按主卷标识符升序（路径表顺序依赖它）。</summary>
    private static List<IsoDirectory> SortDirectories(IsoDirectory directory)
    {
        return directory.Directories
            .OrderBy(child => child.IsoName, StringComparer.Ordinal)
            .ToList();
    }

    /// <summary>
    /// 枚举目录下的记录（不含 "." 与 ".."），顺序按 ISO9660 §9.3：
    /// 目录与文件混在一起、按标识符逐字节升序。量算长度与写出记录必须共用这里，否则长度对不上。
    /// </summary>
    private static List<DirectoryEntry> EnumerateEntries(IsoDirectory directory, bool joliet)
    {
        var entries = new List<DirectoryEntry>(directory.Directories.Count + directory.Files.Count);

        foreach (IsoDirectory child in directory.Directories)
        {
            entries.Add(new DirectoryEntry(
                joliet ? Encoding.BigEndianUnicode.GetBytes(child.Name) : Encoding.ASCII.GetBytes(child.IsoName),
                child,
                null));
        }

        foreach (IsoFile file in directory.Files)
        {
            // 主卷文件标识必须带 ";1" 版本号；Joliet 明确禁止版本号
            entries.Add(new DirectoryEntry(
                joliet ? Encoding.BigEndianUnicode.GetBytes(file.Name) : Encoding.ASCII.GetBytes(file.IsoName + ";1"),
                null,
                file));
        }

        entries.Sort(static (left, right) => CompareIdentifiers(left.Name, right.Name));
        return entries;
    }

    /// <summary>标识符逐字节（无符号）比较，短前缀视为较小 —— ECMA-119 §9.3 的排序规则。</summary>
    private static int CompareIdentifiers(byte[] left, byte[] right)
    {
        int count = Math.Min(left.Length, right.Length);
        for (int i = 0; i < count; i++)
        {
            int difference = left[i] - right[i];
            if (difference != 0)
            {
                return difference;
            }
        }

        return left.Length - right.Length;
    }

    private static void WriteDirectoryRecord(byte[] buffer, int offset, byte[] name, uint extent, uint size, bool isDirectory)
    {
        int length = RecordLength(name.Length);
        buffer[offset] = (byte)length;
        buffer[offset + 1] = 0;
        PutBoth32(buffer, offset + 2, extent);
        PutBoth32(buffer, offset + 10, size);
        WriteRecordingTime(buffer, offset + 18);
        buffer[offset + 25] = (byte)(isDirectory ? 0x02 : 0x00);
        buffer[offset + 26] = 0;
        buffer[offset + 27] = 0;
        PutBoth16(buffer, offset + 28, 1);
        buffer[offset + 32] = (byte)name.Length;
        name.CopyTo(buffer, offset + 33);
    }

    /// <summary>路径表字节数（不含扇区补齐）。</summary>
    private static uint MeasurePathTable(List<IsoDirectory> directories, bool joliet)
    {
        uint size = 0;
        foreach (IsoDirectory directory in directories)
        {
            int nameLength = directory.Parent is null ? 1 : GetPathTableName(directory, joliet).Length;
            size += (uint)(8 + nameLength + (nameLength % 2 == 1 ? 1 : 0));
        }

        return size;
    }

    private static byte[] GetPathTableName(IsoDirectory directory, bool joliet)
    {
        if (directory.Parent is null)
        {
            return new byte[] { 0 };
        }

        return joliet
            ? Encoding.BigEndianUnicode.GetBytes(directory.Name)
            : Encoding.ASCII.GetBytes(directory.IsoName);
    }

    private static byte[] BuildPathTable(List<IsoDirectory> directories, bool littleEndian, bool joliet)
    {
        byte[] table = new byte[MeasurePathTable(directories, joliet)];
        int offset = 0;
        foreach (IsoDirectory directory in directories)
        {
            byte[] name = GetPathTableName(directory, joliet);
            ushort parentIndex = (ushort)(directory.Parent?.Index ?? 1);
            table[offset] = (byte)name.Length;
            table[offset + 1] = 0;
            uint extent = joliet ? directory.JolietExtent : directory.Extent;
            if (littleEndian)
            {
                PutLE32(table, offset + 2, extent);
                PutLE16(table, offset + 6, parentIndex);
            }
            else
            {
                PutBE32(table, offset + 2, extent);
                PutBE16(table, offset + 6, parentIndex);
            }

            name.CopyTo(table, offset + 8);
            offset += 8 + name.Length;
            if (name.Length % 2 == 1)
            {
                offset++;
            }
        }

        return table;
    }

    private static byte[] BuildPrimaryVolumeDescriptor(
        uint totalSectors, uint pathTableSize, uint pathTableLeLba, uint pathTableBeLba, IsoDirectory root)
    {
        byte[] pvd = new byte[SectorSize];
        pvd[0] = 1;
        Encoding.ASCII.GetBytes("CD001").CopyTo(pvd, 1);
        pvd[6] = 1;

        Fill(pvd, 8, 32, (byte)' ');
        Fill(pvd, 40, 32, (byte)' ');
        Encoding.ASCII.GetBytes("FireWinPE").CopyTo(pvd, 40);

        PutBoth32(pvd, 80, totalSectors);              // 卷空间大小（扇区数）
        PutBoth16(pvd, 120, 1);                        // 卷集大小
        PutBoth16(pvd, 124, 1);                        // 卷序号
        PutBoth16(pvd, 128, SectorSize);               // 逻辑块大小
        PutBoth32(pvd, 132, pathTableSize);            // 路径表字节数
        PutLE32(pvd, 140, pathTableLeLba);             // L 路径表位置
        PutBE32(pvd, 148, pathTableBeLba);             // M 路径表位置 [BE]

        WriteDirectoryRecord(pvd, 156, new byte[] { 0 }, root.Extent, root.Length, true);

        Fill(pvd, 190, 128, (byte)' ');
        Fill(pvd, 318, 128, (byte)' ');
        Fill(pvd, 446, 128, (byte)' ');
        Fill(pvd, 574, 128, (byte)' ');
        Encoding.ASCII.GetBytes("FireWin").CopyTo(pvd, 574); // 应用标识
        Fill(pvd, 702, 37, (byte)' ');
        Fill(pvd, 739, 37, (byte)' ');
        Fill(pvd, 776, 37, (byte)' ');
        WriteVolumeDate(pvd, 813);                     // 创建时间
        WriteVolumeDate(pvd, 830);                     // 修改时间
        Fill(pvd, 847, 17, (byte)'0');                 // 失效时间（未指定）
        Fill(pvd, 864, 17, (byte)'0');                 // 生效时间（未指定）
        pvd[881] = 1;                                  // 文件结构版本
        return pvd;
    }

    /// <summary>
    /// 补充 Joliet 卷描述符（UCS-2 长名卷）。
    /// 主卷受 8.3 限制，Microsoft 只能写成 MICROSOF；UEFI 的 bootmgfw 找的是
    /// \EFI\Microsoft\Boot\BCD，靠 Joliet 这一份长名才能对上。
    /// </summary>
    private static byte[] BuildJolietVolumeDescriptor(
        uint totalSectors, uint pathTableSize, uint pathTableLeLba, uint pathTableBeLba, IsoDirectory root)
    {
        byte[] svd = new byte[SectorSize];
        svd[0] = 2;                                    // Joliet 补充卷描述符
        Encoding.ASCII.GetBytes("CD001").CopyTo(svd, 1);
        svd[6] = 1;

        Fill(svd, 8, 32, (byte)' ');                   // 系统标识（ASCII）
        WriteJolietField(svd, 40, 32, "FireWinPE");    // 卷标识（UCS-2 大端）

        PutBoth32(svd, 80, totalSectors);
        PutBoth16(svd, 120, 1);
        PutBoth16(svd, 124, 1);
        PutBoth16(svd, 128, SectorSize);
        PutBoth32(svd, 132, pathTableSize);
        PutLE32(svd, 140, pathTableLeLba);
        PutBE32(svd, 148, pathTableBeLba);

        WriteDirectoryRecord(svd, 156, new byte[] { 0 }, root.JolietExtent, root.JolietLength, true);

        // 转义序列 "%/E"：Joliet Level 3（UCS-2），其余 29 字节保持 0
        svd[88] = (byte)'%';
        svd[89] = (byte)'/';
        svd[90] = (byte)'E';

        WriteJolietField(svd, 190, 128, string.Empty); // 卷集标识
        WriteJolietField(svd, 318, 128, string.Empty); // 发布者
        WriteJolietField(svd, 446, 128, string.Empty); // 数据准备者
        WriteJolietField(svd, 574, 128, "FireWin");    // 应用标识
        Fill(svd, 702, 37, (byte)' ');                 // 版权 / 摘要 / 书目（ASCII）
        Fill(svd, 739, 37, (byte)' ');
        Fill(svd, 776, 37, (byte)' ');
        WriteVolumeDate(svd, 813);
        WriteVolumeDate(svd, 830);
        Fill(svd, 847, 17, (byte)'0');
        Fill(svd, 864, 17, (byte)'0');
        svd[881] = 1;
        return svd;
    }

    /// <summary>按 Joliet 规则写定长文本字段：UCS-2 大端，不足部分用空格（0x20）补齐。</summary>
    private static void WriteJolietField(byte[] buffer, int offset, int fieldSize, string text)
    {
        Fill(buffer, offset, fieldSize, (byte)' ');
        byte[] bytes = Encoding.BigEndianUnicode.GetBytes(text);
        Array.Copy(bytes, 0, buffer, offset, Math.Min(bytes.Length, fieldSize));
    }

    private static byte[] BuildBootRecord()
    {
        byte[] record = new byte[SectorSize];
        record[0] = 0;                                 // 引导记录卷描述符
        Encoding.ASCII.GetBytes("CD001").CopyTo(record, 1);
        record[6] = 1;
        Encoding.ASCII.GetBytes("EL TORITO SPECIFICATION").CopyTo(record, 7);
        PutLE32(record, 71, BootCatalogLba);
        return record;
    }

    private static byte[] BuildTerminator()
    {
        byte[] terminator = new byte[SectorSize];
        terminator[0] = 255;
        Encoding.ASCII.GetBytes("CD001").CopyTo(terminator, 1);
        terminator[6] = 1;
        return terminator;
    }

    private static byte[] BuildBootCatalog(uint biosBootLba, uint biosSectorCount, uint efiBootLba, uint efiSectorCount)
    {
        byte[] catalog = new byte[SectorSize];

        // 校验项（Validation Entry）
        catalog[0] = 0x01;
        catalog[1] = 0x00;                             // 平台：x86
        catalog[30] = 0x55;
        catalog[31] = 0xAA;

        ushort sum = 0;
        for (int i = 0; i < 32; i += 2)
        {
            sum += (ushort)(catalog[i] | catalog[i + 1] << 8);
        }

        PutLE16(catalog, 28, (ushort)(0x10000 - sum));  // 校验和：16 位字累加为 0

        WriteBootEntry(catalog, 32, biosBootLba, biosSectorCount);   // 默认项（BIOS）
        catalog[64] = 0x91;                            // 最终分区头（Final Section Header）
        catalog[65] = 0xEF;                            // 平台：EFI / UEFI
        PutLE16(catalog, 66, 1);                       // 该分区下的引导项数量
        WriteBootEntry(catalog, 96, efiBootLba, efiSectorCount);     // EFI 引导项
        return catalog;
    }

    private static void WriteBootEntry(byte[] buffer, int offset, uint lba, uint sectorCount)
    {
        buffer[offset] = 0x88;                         // 可启动
        buffer[offset + 1] = 0x00;                     // 不模拟（no emulation）
        PutLE16(buffer, offset + 2, 0);                // 加载段：0 表示传统 0x7C0
        buffer[offset + 4] = 0;                        // 系统类型
        PutLE16(buffer, offset + 6, (ushort)sectorCount);
        PutLE32(buffer, offset + 8, lba);
    }

    #endregion

    #region 写入与工具函数

    private static void CopyFileIntoIso(
        Stream output, IsoFile file, IProgress<string> progress, CancellationToken cancellationToken)
    {
        using var input = new FileStream(
            file.SourcePath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        byte[] buffer = new byte[1 << 20];
        long copied = 0;
        int lastTenth = -1;
        int read;

        while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            output.Write(buffer, 0, read);
            copied += read;

            int tenth = (int)(copied * 10 / Math.Max(file.Size, 1u));
            if (tenth != lastTenth)
            {
                lastTenth = tenth;
                progress.Report($"  {file.IsoPath} {Math.Min(100, tenth * 10)}%");
            }
        }

        int padding = (int)((SectorSize - copied % SectorSize) % SectorSize);
        if (padding > 0)
        {
            output.Write(new byte[padding], 0, padding);
        }
    }

    private static void WritePadded(Stream stream, byte[] data)
    {
        stream.Write(data, 0, data.Length);
        int padding = (SectorSize - data.Length % SectorSize) % SectorSize;
        if (padding > 0)
        {
            stream.Write(new byte[padding], 0, padding);
        }
    }

    private static void WriteRecordingTime(byte[] buffer, int offset)
    {
        DateTime now = DateTime.Now;
        buffer[offset] = (byte)Math.Max(0, now.Year - 1900);
        buffer[offset + 1] = (byte)now.Month;
        buffer[offset + 2] = (byte)now.Day;
        buffer[offset + 3] = (byte)now.Hour;
        buffer[offset + 4] = (byte)now.Minute;
        buffer[offset + 5] = (byte)now.Second;
        buffer[offset + 6] = 0;                        // GMT 偏移（0 = UTC）
    }

    private static void WriteVolumeDate(byte[] buffer, int offset)
    {
        string text = DateTime.Now.ToString("yyyyMMddHHmmss") + "00";
        Encoding.ASCII.GetBytes(text).CopyTo(buffer, offset);
        buffer[offset + 16] = 0;
    }

    private static void Fill(byte[] buffer, int offset, int count, byte value)
    {
        Array.Fill(buffer, value, offset, count);
    }

    private static void PutLE16(byte[] buffer, int offset, ushort value)
    {
        buffer[offset] = (byte)value;
        buffer[offset + 1] = (byte)(value >> 8);
    }

    private static void PutBE16(byte[] buffer, int offset, ushort value)
    {
        buffer[offset] = (byte)(value >> 8);
        buffer[offset + 1] = (byte)value;
    }

    private static void PutLE32(byte[] buffer, int offset, uint value)
    {
        buffer[offset] = (byte)value;
        buffer[offset + 1] = (byte)(value >> 8);
        buffer[offset + 2] = (byte)(value >> 16);
        buffer[offset + 3] = (byte)(value >> 24);
    }

    private static void PutBE32(byte[] buffer, int offset, uint value)
    {
        buffer[offset] = (byte)(value >> 24);
        buffer[offset + 1] = (byte)(value >> 16);
        buffer[offset + 2] = (byte)(value >> 8);
        buffer[offset + 3] = (byte)value;
    }

    private static void PutBoth16(byte[] buffer, int offset, ushort value)
    {
        PutLE16(buffer, offset, value);
        PutBE16(buffer, offset + 2, value);
    }

    private static void PutBoth32(byte[] buffer, int offset, uint value)
    {
        PutLE32(buffer, offset, value);
        PutBE32(buffer, offset + 4, value);
    }

    #endregion

    #region 目录树

    private sealed class IsoDirectory
    {
        public string Name = string.Empty;

        /// <summary>主卷（ISO9660 Level 1）使用的 8.3 名称，例如 MICROSOF。</summary>
        public string IsoName = string.Empty;

        public string Path = "\\";
        public IsoDirectory? Parent;
        public int Index;
        public int ParentIndex;
        public uint Extent;
        public uint Length;
        public uint JolietExtent;
        public uint JolietLength;
        public List<IsoDirectory> Directories { get; } = new();
        public List<IsoFile> Files { get; } = new();

        public IsoDirectory AddDirectory(string name)
        {
            var directory = new IsoDirectory
            {
                Name = name,
                IsoName = ToIsoIdentifier(name),
                Path = Path + name + "\\",
            };
            Directories.Add(directory);
            return directory;
        }

        public void AddFile(string name, string sourcePath)
        {
            var info = new FileInfo(sourcePath);
            if (!info.Exists)
            {
                throw new DeployException($"找不到文件：{sourcePath}");
            }

            if (info.Length > uint.MaxValue)
            {
                throw new DeployException(
                    $"ISO 内单个文件不能超过 4 GB：{sourcePath}\n请改用「写入 U 盘」，或把镜像放到其他介质。");
            }

            Files.Add(new IsoFile
            {
                Name = name,
                IsoName = ToIsoIdentifier(name),
                IsoPath = Path + name,
                SourcePath = sourcePath,
                Size = (uint)info.Length,
            });
        }
    }

    private sealed class IsoFile
    {
        public string Name = string.Empty;

        /// <summary>主卷（ISO9660 Level 1）使用的 8.3 名称（不含 ";1"）。</summary>
        public string IsoName = string.Empty;

        public string IsoPath = string.Empty;
        public string SourcePath = string.Empty;
        public uint Size;
        public uint Extent;
    }

    /// <summary>目录下的一个记录：要么是子目录，要么是文件。</summary>
    private readonly struct DirectoryEntry
    {
        public DirectoryEntry(byte[] name, IsoDirectory? directory, IsoFile? file)
        {
            Name = name;
            Directory = directory;
            File = file;
        }

        /// <summary>写入镜像的标识符（主卷 ASCII 大写，Joliet UCS-2 大端）。</summary>
        public byte[] Name { get; }

        public IsoDirectory? Directory { get; }

        public IsoFile? File { get; }
    }

    /// <summary>
    /// 主卷（ISO9660）使用的目录/文件标识：大写 + 去掉非法字符。
    /// 过去按 Level 1 截成 8.3，把 Microsoft 截成了 MICROSOF，UEFI 的 bootmgfw
    /// 按 \EFI\Microsoft\Boot\BCD 查找时直接找不到（0xc000000f）；
    /// 这里保留完整名称（ISO9660 Level 2 允许 31 个字符），长名字再由 Joliet 卷承载。
    /// </summary>
    private static string ToIsoIdentifier(string name)
    {
        var builder = new StringBuilder(name.Length);
        foreach (char c in name.ToUpperInvariant())
        {
            builder.Append(c is >= 'A' and <= 'Z' or >= '0' and <= '9' or '_' or '-' or '.' ? c : '_');
        }

        if (builder.Length > 30)
        {
            builder.Length = 30;
        }

        return builder.ToString();
    }

    #endregion
}
