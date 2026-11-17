using System.IO;
using System.Runtime.InteropServices;
using ManagedWimLib;

namespace FireWin.Services;

/// <summary>
/// wimlib 全局初始化服务。
/// 职责：
///   1. 校验进程架构（libwim-15.dll 仅提供 x64）；
///   2. 定位原生库（普通输出目录 / 单文件自解压目录均支持）；
///   3. 调用 Wim.GlobalInit，并保证进程内只初始化一次；
///   4. 退出时调用 Wim.GlobalCleanup 释放 wimlib 持有的资源。
/// </summary>
public sealed class WimInitService : IDisposable
{
    private const string NativeLibraryFileName = "libwim-15.dll";

    /// <summary>wimlib 全局状态是进程级的，必须串行初始化。</summary>
    private static readonly object InitLock = new();

    private static bool _isInitialized;
    private bool _disposed;

    /// <summary>当前实际加载的原生库完整路径；未初始化时为 null。</summary>
    public string? LoadedLibraryPath { get; private set; }

    /// <summary>wimlib 是否已完成全局初始化。</summary>
    public static bool IsInitialized => _isInitialized;

    /// <summary>初始化 wimlib。重复调用安全（幂等）。</summary>
    /// <exception cref="WimInitException">架构不匹配、找不到原生库或 wimlib 初始化失败。</exception>
    public void EnsureInitialized()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(WimInitService));
        }

        lock (InitLock)
        {
            if (_isInitialized)
            {
                return;
            }

            EnsureSupportedArchitecture();

            string? libraryPath = ResolveNativeLibraryPath();
            try
            {
                if (libraryPath is not null)
                {
                    // 显式传入路径，避免依赖 PATH / 当前工作目录，也避免被同名 DLL 劫持
                    Wim.GlobalInit(libraryPath, InitFlags.None);
                }
                else
                {
                    // 单文件自解压等场景下无法拿到确切路径时，交给默认探测逻辑
                    Wim.GlobalInit(InitFlags.None);
                }
            }
            catch (DllNotFoundException ex)
            {
                throw new WimInitException(
                    $"未能加载 {NativeLibraryFileName}。请确认该文件与主程序位于同一目录，或位于 runtimes\\win-x64\\native\\ 下。",
                    ex);
            }
            catch (BadImageFormatException ex)
            {
                throw new WimInitException(
                    $"{NativeLibraryFileName} 与当前进程架构不匹配（本程序必须为 x64 进程）。",
                    ex);
            }
            catch (Exception ex) when (ex is not WimInitException)
            {
                throw new WimInitException($"wimlib 全局初始化失败：{ex.Message}", ex);
            }

            _isInitialized = true;
            LoadedLibraryPath = libraryPath;
        }
    }

    /// <summary>
    /// 按优先级查找 libwim-15.dll：
    ///   1. 环境变量 OPENWININSTALLER_LIBWIM（调试自编译版本时使用）；
    ///   2. 程序目录（dotnet build / 手工铺开的发布目录）；
    ///   3. runtimes\win-x64\native\（NuGet 原始布局）；
    ///   4. NATIVE_DLL_SEARCH_DIRECTORIES（单文件发布时原生库被自解压到的临时目录）。
    /// 找不到时返回 null，由 wimlib 自身默认逻辑兜底。
    /// </summary>
    public static string? ResolveNativeLibraryPath()
    {
        string? overridePath = Environment.GetEnvironmentVariable("OPENWININSTALLER_LIBWIM");
        if (!string.IsNullOrWhiteSpace(overridePath) && File.Exists(overridePath))
        {
            return Path.GetFullPath(overridePath);
        }

        string baseDir = AppContext.BaseDirectory;
        string[] candidates =
        {
            Path.Combine(baseDir, NativeLibraryFileName),
            Path.Combine(baseDir, "runtimes", "win-x64", "native", NativeLibraryFileName),
            Path.Combine(baseDir, "native", NativeLibraryFileName),
        };

        foreach (string candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                return Path.GetFullPath(candidate);
            }
        }

        // 单文件 + IncludeNativeLibrariesForSelfExtract 时，运行时会把原生库解到临时目录，
        // 并把该目录写入 NATIVE_DLL_SEARCH_DIRECTORIES。
        if (AppContext.GetData("NATIVE_DLL_SEARCH_DIRECTORIES") is string searchDirs)
        {
            foreach (string dir in searchDirs.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                string candidate = Path.Combine(dir.Trim(), NativeLibraryFileName);
                if (File.Exists(candidate))
                {
                    return Path.GetFullPath(candidate);
                }
            }
        }

        return null;
    }

    /// <summary>libwim-15.dll 只有 x64 版本，32 位进程加载会直接失败，这里提前给出可读错误。</summary>
    private static void EnsureSupportedArchitecture()
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new WimInitException("本工具仅支持 Windows。");
        }

        if (RuntimeInformation.ProcessArchitecture != Architecture.X64)
        {
            throw new WimInitException(
                $"当前进程架构为 {RuntimeInformation.ProcessArchitecture}，wimlib 原生库仅支持 x64。请以 x64 方式运行。");
        }
    }

    /// <summary>释放 wimlib 全局资源。只应由进程退出前的最后一个使用者调用。</summary>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        lock (InitLock)
        {
            if (_isInitialized)
            {
                try
                {
                    Wim.GlobalCleanup();
                }
                catch
                {
                    // 退出阶段清理失败不影响结果
                }

                _isInitialized = false;
                LoadedLibraryPath = null;
            }

            _disposed = true;
        }
    }
}

/// <summary>wimlib 初始化相关异常。</summary>
public sealed class WimInitException : Exception
{
    public WimInitException(string message) : base(message)
    {
    }

    public WimInitException(string message, Exception innerException) : base(message, innerException)
    {
    }
}
