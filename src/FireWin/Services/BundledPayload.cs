using System;
using System.IO;
using System.Linq;

namespace FireWin.Services;

/// <summary>
/// 随程序分发的内置资源，只包含运行必需的成品 PE：
///   payload\pe\boot.wim
///   payload\pe\boot.sdi
///   payload\pe\Tools\FireWinPE.exe
///   payload\pe\Tools\wimlib-imagex.exe
///   payload\pe\Tools\libwim-15.dll
///   payload\pe\Tools\7za.exe
/// 有成品 PE 时无需安装 ADK：直接解压 payload 并写引导即可。
/// ADK 不随包分发——只有重新制作 PE 时才需要，按需通过 winget / 官方安装程序获取。
/// </summary>
public static class BundledPayload
{
    /// <summary>payload 根目录。</summary>
    public static string Root => Path.Combine(AppContext.BaseDirectory, "payload");

    /// <summary>成品 PE 目录。</summary>
    public static string PeDirectory => Path.Combine(Root, "pe");

    /// <summary>成品 PE 是否齐全；缺少时给出缺项说明。</summary>
    public static bool TryGetPePayload(out string missing)
    {
        string[] required =
        {
            Path.Combine(PeDirectory, "boot.wim"),
            Path.Combine(PeDirectory, "boot.sdi"),
            Path.Combine(PeDirectory, "Tools", "FireWinPE.exe"),
            Path.Combine(PeDirectory, "Tools", "wimlib-imagex.exe"),
            Path.Combine(PeDirectory, "Tools", "libwim-15.dll"),
            Path.Combine(PeDirectory, "Tools", "7za.exe"),
        };

        string[] absent = required.Where(path => !File.Exists(path)).ToArray();
        missing = absent.Length == 0
            ? string.Empty
            : string.Join("、", absent.Select(Path.GetFileName));
        return absent.Length == 0;
    }
}
