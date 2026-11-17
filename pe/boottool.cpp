// FireBoot —— FireWin 独立引导工具
// 语言：C++17 / Win32 API，静态 CRT（/MT），单 exe，无任何运行时依赖。
// 许可证：GPL-3.0-only
//
// 职责（与桌面端 BcdBootService.cs 的 bcdedit 序列保持一致）：
//   1. 查看 BCD 启动项（bcdedit /enum）；
//   2. 写入一次性 PE 引导：找到本机固定分区上的 boot.wim / boot.sdi，
//      创建 {ramdiskoptions} + osloader 项并设置 /bootsequence，重启即进 PE；
//      一次引导结束后系统自动回到默认启动项，不影响正常系统；
//   3. 删除本工具写入的 PE 引导项（GUID 持久化在 exe 目录 bootentry.txt）；
//   4. 修复系统引导：对目标分区执行 bcdboot <Windows 分区>\Windows /s <引导分区> /f ALL。
//
// 运行环境：完整 Windows 与裸 WinPE 均可（bcdedit / bcdboot 都在 System32）。
// 非管理员身份下窗口可正常打开，执行引导操作时才会请求提权（runas 重启自身）。

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // GetFirmwareType 需要 Win8 以上声明
#endif

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------------------
// 基础工具函数
// ---------------------------------------------------------------------------
namespace {

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring AnsiToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    int size = MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring Trim(const std::wstring& value)
{
    const size_t begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos)
    {
        return {};
    }

    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::wstring EnsureTrailingBackslash(const std::wstring& path)
{
    if (path.empty() || path.back() == L'\\')
    {
        return path;
    }

    return path + L'\\';
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& name)
{
    return EnsureTrailingBackslash(directory) + name;
}

bool FileExists(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring GetExeDirectory()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return {};
        }

        if (length < buffer.size() - 1)
        {
            break;
        }

        buffer.resize(buffer.size() * 2);
    }

    std::wstring path(buffer.data());
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// 取系统目录（完整 Windows 下为 C:\Windows\System32，PE 下为 X:\Windows\System32），
// bcdedit / bcdboot / shutdown 都在这里
std::wstring GetSystemDirectoryPath()
{
    wchar_t buffer[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(buffer, ARRAYSIZE(buffer));
    if (length == 0 || length >= ARRAYSIZE(buffer))
    {
        return {};
    }

    return std::wstring(buffer, length);
}

// ---------------------------------------------------------------------------
// 进程调用：捕获合并后的 stdout + stderr
// 说明：bcdedit 重定向输出为 UTF-16LE（通常带 BOM），其余工具多为 ANSI，
//       因此解码时先探测 BOM / 零字节密度，再回退本地 ANSI 代码页。
// ---------------------------------------------------------------------------
std::wstring DecodeProcessOutput(const std::string& raw)
{
    if (raw.size() >= 2)
    {
        const auto byte0 = static_cast<unsigned char>(raw[0]);
        const auto byte1 = static_cast<unsigned char>(raw[1]);

        // UTF-16LE BOM
        if (byte0 == 0xFF && byte1 == 0xFE)
        {
            return std::wstring(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / sizeof(wchar_t));
        }

        // 无 BOM 但偶数字节大量为 0 时也按 UTF-16 处理（bcdedit 某些版本不带 BOM）
        int zeros = 0;
        const size_t probe = std::min<size_t>(raw.size(), 128);
        for (size_t i = 1; i < probe; i += 2)
        {
            if (raw[i] == '\0')
            {
                ++zeros;
            }
        }

        if (zeros > 16)
        {
            return std::wstring(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / sizeof(wchar_t));
        }
    }

    return AnsiToWide(raw);
}

// 启动子进程并把 stdout + stderr 合并读回。返回是否成功启动并可取到退出码。
bool RunCapture(const std::wstring& exePath, const std::wstring& arguments,
                DWORD& exitCode, std::wstring& output, std::wstring& error)
{
    exitCode = static_cast<DWORD>(-1);
    output.clear();
    error.clear();

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0))
    {
        error = L"创建输出管道失败，错误码 " + std::to_wstring(GetLastError());
        return false;
    }

    // 只让子进程继承写端
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    std::wstring commandLine = L"\"" + exePath + L"\"";
    if (!arguments.empty())
    {
        commandLine += L" " + arguments;
    }

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd; // 合并 stderr，便于完整记录错误信息
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr,
                                        mutableCommandLine.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &processInfo);
    // 父进程用不到写端，无论成败都先关掉
    CloseHandle(writeEnd);

    if (!created)
    {
        CloseHandle(readEnd);
        error = L"启动进程失败：" + exePath + L"，错误码 " + std::to_wstring(GetLastError());
        return false;
    }

    CloseHandle(processInfo.hThread);

    std::string raw;
    char chunk[4096]{};
    DWORD read = 0;
    for (;;)
    {
        if (!ReadFile(readEnd, chunk, sizeof(chunk), &read, nullptr) || read == 0)
        {
            break;
        }

        raw.append(chunk, read);
    }
    CloseHandle(readEnd);

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hProcess);

    output = Trim(DecodeProcessOutput(raw));
    return true;
}

// ---------------------------------------------------------------------------
// 权限与固件
// ---------------------------------------------------------------------------
bool IsElevated()
{
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        TOKEN_ELEVATION info{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &info, sizeof(info), &size))
        {
            elevated = info.TokenIsElevated;
        }

        CloseHandle(token);
    }

    return elevated != FALSE;
}

// 以管理员身份重启自身；成功后调用方应退出当前实例
bool RelaunchElevated(HWND owner)
{
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe)) == 0)
    {
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

// 前向声明：日志函数定义在界面段，引导操作在它之前就要用
void AppendLog(HWND edit, const std::wstring& message);
void AppendLogToFile(const std::wstring& message);

// 固件类型：0 未知 / 1 BIOS / 2 UEFI；GetFirmwareType 不可用时回退解析 bcdedit 输出
int DetectFirmwareType(HWND logEdit)
{
    FIRMWARE_TYPE firmware = FirmwareTypeUnknown;
    if (GetFirmwareType(&firmware) && (firmware == FirmwareTypeBios || firmware == FirmwareTypeUefi))
    {
        return firmware == FirmwareTypeUefi ? 2 : 1;
    }

    const std::wstring bcdEdit = JoinPath(GetSystemDirectoryPath(), L"bcdedit.exe");
    DWORD exitCode = 0;
    std::wstring output;
    std::wstring error;
    if (RunCapture(bcdEdit, L"/enum {current}", exitCode, output, error) && exitCode == 0)
    {
        std::wstring lowered = output;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (lowered.find(L"winload.efi") != std::wstring::npos)
        {
            return 2;
        }

        if (lowered.find(L"winload.exe") != std::wstring::npos)
        {
            return 1;
        }
    }

    AppendLog(logEdit, L"警告：无法确定固件类型，将按 UEFI 处理（bcdboot 使用 /f ALL，两种模式都写）。");
    return 0;
}

// ---------------------------------------------------------------------------
// 磁盘与 PE 源扫描
// ---------------------------------------------------------------------------
std::vector<std::wstring> GetFixedDrives()
{
    std::vector<std::wstring> drives;

    wchar_t buffer[512]{};
    const DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(buffer) - 1, buffer);
    if (length == 0 || length >= ARRAYSIZE(buffer))
    {
        return drives;
    }

    for (const wchar_t* cursor = buffer; *cursor != L'\0'; cursor += lstrlenW(cursor) + 1)
    {
        if (GetDriveTypeW(cursor) == DRIVE_FIXED)
        {
            std::wstring drive(cursor);
            drive.pop_back(); // "C:\" → "C:"
            drives.push_back(drive);
        }
    }

    return drives;
}

// 取路径的分区根（"C:\FireWinPE" → "C:\"），失败返回空
std::wstring GetDriveRoot(const std::wstring& path)
{
    if (path.size() < 3 || path[1] != L':')
    {
        return {};
    }

    return path.substr(0, 3); // "C:\"
}

// 判断目录是否为可直接引导的 PE 源（boot.wim + boot.sdi）
bool IsPeSource(const std::wstring& directory)
{
    return FileExists(JoinPath(directory, L"boot.wim")) && FileExists(JoinPath(directory, L"boot.sdi"));
}

// 扫描常见位置的 PE 源目录：
//   exe 所在目录、exe 目录\payload\pe、每个固定分区根下的 \FireWinPE 与 \payload\pe
std::vector<std::wstring> FindPeSources()
{
    std::vector<std::wstring> found;

    const auto push = [&found](const std::wstring& candidate) {
        if (candidate.empty() || !IsPeSource(candidate))
        {
            return;
        }

        // 去尾反斜杠后去重
        std::wstring normalized = candidate;
        while (normalized.size() > 3 && normalized.back() == L'\\')
        {
            normalized.pop_back();
        }

        for (const std::wstring& existing : found)
        {
            if (_wcsicmp(existing.c_str(), normalized.c_str()) == 0)
            {
                return;
            }
        }

        found.push_back(normalized);
    };

    const std::wstring exeDirectory = GetExeDirectory();
    push(exeDirectory);
    push(JoinPath(exeDirectory, L"payload\\pe"));

    for (const std::wstring& drive : GetFixedDrives())
    {
        push(JoinPath(drive, L"FireWinPE"));
        push(JoinPath(drive, L"payload\\pe"));
    }

    return found;
}

// 从 bcdedit /create 输出中提取形如 {xxxxxxxx-...} 的 GUID
std::wstring ParseGuid(const std::wstring& text)
{
    const wchar_t* validChars = L"0123456789abcdefABCDEF-";
    for (size_t begin = text.find(L'{'); begin != std::wstring::npos; begin = text.find(L'{', begin + 1))
    {
        const size_t end = text.find(L'}', begin);
        if (end == std::wstring::npos || end - begin - 1 > 36)
        {
            continue;
        }

        const std::wstring inner = text.substr(begin + 1, end - begin - 1);
        if (inner.find_first_not_of(validChars) != std::wstring::npos)
        {
            continue;
        }

        return text.substr(begin, end - begin + 1);
    }

    return {};
}

// ---------------------------------------------------------------------------
// 日志：写界面文本框 + 落盘 boottool.log
// ---------------------------------------------------------------------------
void AppendLog(HWND edit, const std::wstring& message)
{
    if (edit == nullptr)
    {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t timestamp[32]{};
    swprintf_s(timestamp, L"[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);

    std::wstring line = std::wstring(timestamp) + message + L"\r\n";

    // 光标移到末尾再替换，保证追加效果并自动滚到最新
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void AppendLogToFile(const std::wstring& message)
{
    const std::wstring logPath = JoinPath(GetExeDirectory(), L"boottool.log");
    std::ofstream file(logPath, std::ios::app | std::ios::binary);
    if (!file.is_open())
    {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t timestamp[40]{};
    swprintf_s(timestamp, L"[%04u-%02u-%02u %02u:%02u:%02u] ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    file << WideToUtf8(timestamp) << WideToUtf8(message) << "\r\n";
}

// ---------------------------------------------------------------------------
// BCD 操作（bcdedit 子进程封装）
// ---------------------------------------------------------------------------
bool BcdRun(HWND logEdit, const std::wstring& arguments, std::wstring& output)
{
    const std::wstring bcdEdit = JoinPath(GetSystemDirectoryPath(), L"bcdedit.exe");
    DWORD exitCode = 0;
    std::wstring error;
    if (!RunCapture(bcdEdit, arguments, exitCode, output, error))
    {
        AppendLog(logEdit, L"错误：" + error);
        AppendLogToFile(L"错误：" + error);
        return false;
    }

    if (exitCode != 0)
    {
        AppendLog(logEdit, L"错误：bcdedit " + arguments + L" 失败（退出码 " + std::to_wstring(exitCode) + L"）");
        if (!output.empty())
        {
            AppendLog(logEdit, output);
        }

        AppendLogToFile(L"bcdedit 失败（" + arguments + L"）：" + output);
        return false;
    }

    return true;
}

// 忽略失败版本的 bcdedit 调用（清理逻辑用）
void BcdTry(HWND logEdit, const std::wstring& arguments)
{
    std::wstring output;
    BcdRun(logEdit, arguments, output);
}

bool BcdEntryExists(HWND logEdit, const std::wstring& entry)
{
    UNREFERENCED_PARAMETER(logEdit); // 存在性探测不输出日志
    const std::wstring bcdEdit = JoinPath(GetSystemDirectoryPath(), L"bcdedit.exe");
    DWORD exitCode = 0;
    std::wstring output;
    std::wstring error;
    if (!RunCapture(bcdEdit, L"/enum " + entry, exitCode, output, error))
    {
        return false;
    }

    return exitCode == 0;
}

bool BcdSet(HWND logEdit, const std::wstring& entry, const std::wstring& property, const std::wstring& value)
{
    std::wstring output;
    return BcdRun(logEdit, L"/set " + entry + L" " + property + L" \"" + value + L"\"", output);
}

// 创建 OSLOADER 项并返回 GUID（失败返回空）
std::wstring CreateOsLoaderEntry(HWND logEdit)
{
    std::wstring output;
    if (!BcdRun(logEdit, L"/create /d \"FireWin PE\" /application osloader", output))
    {
        return {};
    }

    const std::wstring guid = ParseGuid(output);
    if (guid.empty())
    {
        AppendLog(logEdit, L"错误：无法从 bcdedit 输出解析启动项 GUID：" + output);
    }

    return guid;
}

// ---------------------------------------------------------------------------
// 引导项 GUID 的持久化（exe 目录 bootentry.txt），供「删除 PE 引导项」使用
// ---------------------------------------------------------------------------
std::wstring LoadSavedGuid()
{
    const std::wstring path = JoinPath(GetExeDirectory(), L"bootentry.txt");
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }

    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return Trim(AnsiToWide(raw));
}

void SaveGuid(const std::wstring& guid)
{
    const std::wstring path = JoinPath(GetExeDirectory(), L"bootentry.txt");
    std::ofstream file(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (file.is_open())
    {
        file << WideToUtf8(guid);
    }
}

// ---------------------------------------------------------------------------
// 界面
// ---------------------------------------------------------------------------
constexpr wchar_t kWindowClass[] = L"FireBootMain";
constexpr wchar_t kWindowTitle[] = L"FireWin 引导工具";

constexpr int kClientWidth = 560;
constexpr int kClientHeight = 480;

constexpr int kCommandWritePe = 2001;
constexpr int kCommandRemovePe = 2002;
constexpr int kCommandShowEntries = 2003;
constexpr int kCommandRepair = 2004;
constexpr int kCommandRefresh = 2005;
constexpr int kComboPeSource = 2101;
constexpr int kComboWindows = 2102;
constexpr int kComboBoot = 2103;

struct ToolState
{
    HWND window = nullptr;
    HWND logEdit = nullptr;
    HWND comboPeSource = nullptr;
    HWND comboWindows = nullptr;
    HWND comboBoot = nullptr;
    HWND writeButton = nullptr;
    HFONT font = nullptr;
};

ToolState g_tool;

// 与 PE 启动器一致：按候选列表探测可用字体，避免中文显示成方块
int CALLBACK DetectFontFamily(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM parameter)
{
    auto* found = reinterpret_cast<bool*>(parameter);
    *found = true;
    return 0;
}

bool IsFontInstalled(const wchar_t* faceName)
{
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(query.lfFaceName, faceName, LF_FACESIZE);

    bool found = false;
    HDC screen = GetDC(nullptr);
    if (screen != nullptr)
    {
        EnumFontFamiliesExW(screen, &query, DetectFontFamily, reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, screen);
    }

    return found;
}

HFONT CreateToolFont()
{
    const wchar_t* candidates[] = { L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimSun", L"NSimSun" };

    LOGFONTW description{};
    description.lfHeight = -15;
    description.lfWeight = FW_NORMAL;
    description.lfCharSet = DEFAULT_CHARSET;

    for (const wchar_t* candidate : candidates)
    {
        if (!IsFontInstalled(candidate))
        {
            continue;
        }

        lstrcpynW(description.lfFaceName, candidate, LF_FACESIZE);
        HFONT font = CreateFontIndirectW(&description);
        if (font != nullptr)
        {
            return font;
        }
    }

    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

HWND CreateControl(HWND window, const wchar_t* className, const wchar_t* text, DWORD style,
                   int x, int y, int width, int height, int commandId)
{
    HWND control = CreateWindowExW(0, className, text, style,
                                   x, y, width, height,
                                   window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(commandId)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr && g_tool.font != nullptr)
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_tool.font), TRUE);
    }

    return control;
}

void RefreshPeSources()
{
    SendMessageW(g_tool.comboPeSource, CB_RESETCONTENT, 0, 0);

    const std::vector<std::wstring> sources = FindPeSources();
    for (const std::wstring& source : sources)
    {
        SendMessageW(g_tool.comboPeSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(source.c_str()));
    }

    if (!sources.empty())
    {
        SendMessageW(g_tool.comboPeSource, CB_SETCURSEL, 0, 0);
    }

    EnableWindow(g_tool.writeButton, !sources.empty());
}

void RefreshDriveCombos()
{
    const std::vector<std::wstring> drives = GetFixedDrives();

    SendMessageW(g_tool.comboWindows, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_tool.comboBoot, CB_RESETCONTENT, 0, 0);

    int windowsIndex = -1;
    int bootIndex = -1;
    for (size_t i = 0; i < drives.size(); ++i)
    {
        LPARAM self = reinterpret_cast<LPARAM>(drives[i].c_str());
        SendMessageW(g_tool.comboWindows, CB_ADDSTRING, 0, self);
        SendMessageW(g_tool.comboBoot, CB_ADDSTRING, 0, self);

        // Windows 分区默认选系统所在盘；引导分区默认选第一个固定磁盘
        wchar_t windowsDirectory[MAX_PATH]{};
        if (GetWindowsDirectoryW(windowsDirectory, ARRAYSIZE(windowsDirectory)) > 0 &&
            _wcsnicmp(windowsDirectory, drives[i].c_str(), 2) == 0)
        {
            windowsIndex = static_cast<int>(i);
        }

        if (bootIndex < 0)
        {
            bootIndex = static_cast<int>(i);
        }
    }

    if (windowsIndex >= 0)
    {
        SendMessageW(g_tool.comboWindows, CB_SETCURSEL, static_cast<WPARAM>(windowsIndex), 0);
    }
    else if (!drives.empty())
    {
        SendMessageW(g_tool.comboWindows, CB_SETCURSEL, 0, 0);
    }

    if (bootIndex >= 0)
    {
        SendMessageW(g_tool.comboBoot, CB_SETCURSEL, static_cast<WPARAM>(bootIndex), 0);
    }
}

std::wstring GetComboText(HWND combo)
{
    wchar_t buffer[MAX_PATH]{};
    const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index < 0)
    {
        return {};
    }

    if (SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(buffer)) <= 0)
    {
        return {};
    }

    return buffer;
}

std::wstring GetSelectedPeSource()
{
    wchar_t buffer[MAX_PATH * 2]{};
    const LRESULT index = SendMessageW(g_tool.comboPeSource, CB_GETCURSEL, 0, 0);
    if (index < 0)
    {
        return {};
    }

    if (SendMessageW(g_tool.comboPeSource, CB_GETLBTEXT, static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(buffer)) <= 0)
    {
        return {};
    }

    return buffer;
}

// 需要管理员权限的操作入口：非管理员时请求提权并重启自身
bool EnsureElevatedForOperation()
{
    if (IsElevated())
    {
        return true;
    }

    if (RelaunchElevated(g_tool.window))
    {
        PostQuitMessage(0);
        return false;
    }

    MessageBoxW(g_tool.window,
                L"引导操作需要管理员权限，且提权请求被拒绝。\r\n请右键以管理员身份运行本工具。",
                kWindowTitle, MB_OK | MB_ICONWARNING);
    return false;
}

// 重启：PE 里优先 wpeutil，完整 Windows 用 shutdown
void RebootComputer()
{
    const std::wstring systemDirectory = GetSystemDirectoryPath();

    const std::wstring wpeutil = JoinPath(systemDirectory, L"wpeutil.exe");
    if (FileExists(wpeutil))
    {
        DWORD exitCode = 0;
        std::wstring output;
        std::wstring error;
        RunCapture(wpeutil, L"reboot", exitCode, output, error);
        return;
    }

    const std::wstring shutdown = JoinPath(systemDirectory, L"shutdown.exe");
    DWORD exitCode = 0;
    std::wstring output;
    std::wstring error;
    RunCapture(shutdown, L"/r /t 0", exitCode, output, error);
}

// ---------------------------------------------------------------------------
// 功能实现
// ---------------------------------------------------------------------------

// 写入一次性 PE 引导：{ramdiskoptions} + osloader + /bootsequence
void WritePeBoot()
{
    const std::wstring peSource = GetSelectedPeSource();
    if (peSource.empty())
    {
        MessageBoxW(g_tool.window, L"请先选择 PE 位置（需包含 boot.wim 与 boot.sdi）。",
                    kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring driveRoot = GetDriveRoot(peSource);
    if (driveRoot.empty())
    {
        MessageBoxW(g_tool.window, (L"无法确定 PE 所在分区：" + peSource).c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
        return;
    }

    // PE 目录相对分区根的路径，例如 C:\FireWinPE → \FireWinPE
    std::wstring relative = L"\\" + peSource.substr(driveRoot.size());
    while (!relative.empty() && relative.back() == L'\\')
    {
        relative.pop_back();
    }
    if (relative == L"\\")
    {
        relative.clear();
    }
    const std::wstring relativeRoot = relative.empty() ? L"\\" : relative + L"\\";

    const int firmware = DetectFirmwareType(g_tool.logEdit);
    const bool uefi = firmware != 1; // 未知时按 UEFI 的 winload.efi 处理

    AppendLog(g_tool.logEdit, L"==== 写入 PE 引导 ====");
    AppendLog(g_tool.logEdit, L"PE 位置：" + peSource + L"（分区 " + driveRoot + L"）");
    AppendLog(g_tool.logEdit, uefi ? L"固件类型：UEFI" : L"固件类型：BIOS/Legacy");
    AppendLogToFile(L"写入 PE 引导：" + peSource);

    constexpr wchar_t kRamdiskOptions[] = L"{ramdiskoptions}";
    bool ramdiskCreated = false;
    std::wstring entry;

    // 任一环节失败都回滚已创建的项，避免留下坏启动项
    auto rollback = [&]() {
        if (!entry.empty())
        {
            BcdTry(g_tool.logEdit, L"/delete " + entry + L" /f");
        }

        if (ramdiskCreated)
        {
            BcdTry(g_tool.logEdit, L"/delete " + std::wstring(kRamdiskOptions) + L" /f");
        }
    };

    // 1. ramdisk 选项：boot.sdi 所在设备与路径
    if (!BcdEntryExists(g_tool.logEdit, kRamdiskOptions))
    {
        std::wstring output;
        if (!BcdRun(g_tool.logEdit,
                    std::wstring(L"/create ") + kRamdiskOptions + L" /d \"FireWin RAMDisk Options\"",
                    output))
        {
            return;
        }

        ramdiskCreated = true;
    }
    else
    {
        AppendLog(g_tool.logEdit, L"复用已有 {ramdiskoptions}。");
    }

    if (!BcdSet(g_tool.logEdit, kRamdiskOptions, L"ramdisksdidevice", L"partition=" + driveRoot) ||
        !BcdSet(g_tool.logEdit, kRamdiskOptions, L"ramdisksdipath", relativeRoot + L"boot.sdi"))
    {
        rollback();
        return;
    }

    // 2. 创建 OSLOADER 项
    entry = CreateOsLoaderEntry(g_tool.logEdit);
    if (entry.empty())
    {
        rollback();
        return;
    }

    // 3. 指向 boot.wim（ramdisk），并设置加载器与 WinPE 标记
    const std::wstring wimOnDisk = L"ramdisk=[" + driveRoot + L"]" + relativeRoot + L"boot.wim," + kRamdiskOptions;
    const std::wstring loaderPath = uefi ? L"\\Windows\\System32\\boot\\winload.efi"
                                         : L"\\Windows\\System32\\winload.exe";

    if (!BcdSet(g_tool.logEdit, entry, L"device", wimOnDisk) ||
        !BcdSet(g_tool.logEdit, entry, L"osdevice", wimOnDisk) ||
        !BcdSet(g_tool.logEdit, entry, L"path", loaderPath) ||
        !BcdSet(g_tool.logEdit, entry, L"systemroot", L"\\Windows") ||
        !BcdSet(g_tool.logEdit, entry, L"winpe", L"yes"))
    {
        rollback();
        return;
    }

    if (!uefi)
    {
        if (!BcdSet(g_tool.logEdit, entry, L"detecthal", L"yes"))
        {
            rollback();
            return;
        }
    }

    // 4. 仅下一次启动生效，不改变默认启动项
    std::wstring output;
    if (!BcdRun(g_tool.logEdit, L"/bootsequence " + entry, output))
    {
        rollback();
        return;
    }

    SaveGuid(entry);
    AppendLog(g_tool.logEdit, L"写入成功，启动项：" + entry);
    AppendLog(g_tool.logEdit, L"本次设置仅对下一次启动生效；进入 PE 部署完成后会自动清理该启动项。");
    AppendLogToFile(L"写入成功：" + entry);

    const int answer = MessageBoxW(g_tool.window,
                                   L"已写入一次性 PE 引导。\r\n\r\n立即重启进入 FireWin PE 吗？",
                                   kWindowTitle, MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2);
    if (answer == IDYES)
    {
        RebootComputer();
    }
}

// 删除本工具写入的 PE 引导项 + {ramdiskoptions} + 一次性引导记录
void RemovePeBoot()
{
    AppendLog(g_tool.logEdit, L"==== 删除 PE 引导项 ====");

    const std::wstring guid = LoadSavedGuid();
    if (guid.empty())
    {
        AppendLog(g_tool.logEdit, L"未找到本工具保存的启动项记录（bootentry.txt），跳过删除。");
        return;
    }

    AppendLog(g_tool.logEdit, L"删除启动项：" + guid);
    BcdTry(g_tool.logEdit, L"/delete " + guid + L" /f");
    BcdTry(g_tool.logEdit, L"/delete {ramdiskoptions} /f");
    // 清掉尚未消耗的一次性引导记录，避免下次启动找不到项
    BcdTry(g_tool.logEdit, L"/deletevalue {bootmgr} bootsequence");

    SaveGuid({});
    AppendLog(g_tool.logEdit, L"已删除并清空记录。");
    AppendLogToFile(L"删除 PE 引导项：" + guid);
}

// 查看当前 BCD 启动项列表
void ShowBootEntries()
{
    AppendLog(g_tool.logEdit, L"==== 当前启动项 ====");

    const std::wstring bcdEdit = JoinPath(GetSystemDirectoryPath(), L"bcdedit.exe");
    DWORD exitCode = 0;
    std::wstring output;
    std::wstring error;
    if (RunCapture(bcdEdit, L"/enum", exitCode, output, error) && exitCode == 0 && !output.empty())
    {
        AppendLog(g_tool.logEdit, output);
    }
    else
    {
        AppendLog(g_tool.logEdit, L"bcdedit /enum 失败：" + (output.empty() ? error : output));
    }
}

// 修复系统引导：bcdboot <Windows 分区>\Windows /s <引导分区> /f ALL
void RepairBoot()
{
    const std::wstring windowsDrive = GetComboText(g_tool.comboWindows);
    const std::wstring bootDrive = GetComboText(g_tool.comboBoot);
    if (windowsDrive.empty() || bootDrive.empty())
    {
        MessageBoxW(g_tool.window, L"请选择 Windows 分区与引导分区。", kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring windowsRoot = JoinPath(windowsDrive, L"Windows");
    if (!DirectoryExists(windowsRoot))
    {
        const std::wstring message = L"在 " + windowsRoot + L" 下未找到 Windows 目录，请确认所选分区。";
        MessageBoxW(g_tool.window, message.c_str(), kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    const int answer = MessageBoxW(g_tool.window,
                                   (L"将使用 " + windowsRoot + L" 的引导文件重建 " + bootDrive +
                                    L" 的引导项。\r\n\r\n继续吗？").c_str(),
                                   kWindowTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES)
    {
        return;
    }

    AppendLog(g_tool.logEdit, L"==== 修复系统引导 ====");
    AppendLog(g_tool.logEdit, L"Windows 分区：" + windowsDrive + L"，引导分区：" + bootDrive);
    AppendLogToFile(L"修复引导：Windows=" + windowsDrive + L"，引导=" + bootDrive);

    const std::wstring bcdBoot = JoinPath(GetSystemDirectoryPath(), L"bcdboot.exe");
    const std::wstring arguments = L"\"" + windowsRoot + L"\" /s " + bootDrive + L" /f ALL";
    DWORD exitCode = 0;
    std::wstring output;
    std::wstring error;
    RunCapture(bcdBoot, arguments, exitCode, output, error);

    if (!output.empty())
    {
        AppendLog(g_tool.logEdit, output);
    }

    if (exitCode == 0)
    {
        AppendLog(g_tool.logEdit, L"修复完成。");
        AppendLogToFile(L"修复完成。");
    }
    else
    {
        AppendLog(g_tool.logEdit, L"修复失败（退出码 " + std::to_wstring(exitCode) + L"）。");
        AppendLogToFile(L"修复失败（退出码 " + std::to_wstring(exitCode) + L"）。");
    }
}

void HandleCommand(int commandId)
{
    switch (commandId)
    {
    case kCommandWritePe:
        if (EnsureElevatedForOperation())
        {
            WritePeBoot();
        }
        break;
    case kCommandRemovePe:
        if (EnsureElevatedForOperation())
        {
            RemovePeBoot();
        }
        break;
    case kCommandShowEntries:
        ShowBootEntries();
        break;
    case kCommandRepair:
        if (EnsureElevatedForOperation())
        {
            RepairBoot();
        }
        break;
    case kCommandRefresh:
        RefreshPeSources();
        RefreshDriveCombos();
        break;
    default:
        break;
    }
}

void CreateControls(HWND window)
{
    g_tool.font = CreateToolFont();

    CreateControl(window, L"STATIC",
                  L"写入 PE 引导：重启一次进入 FireWin PE（不改动默认启动项）。\r\n"
                  L"修复系统引导：用所选 Windows 分区的引导文件重建所选引导分区的 BCD。",
                  WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 12, kClientWidth - 32, 44, 0);

    CreateControl(window, L"STATIC", L"PE 位置：", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 70, 70, 22, 0);
    g_tool.comboPeSource = CreateControl(window, L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                                         88, 67, 352, 220, kComboPeSource);
    CreateControl(window, L"BUTTON", L"刷新",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                  448, 66, 96, 27, kCommandRefresh);

    g_tool.writeButton = CreateControl(window, L"BUTTON", L"写入 PE 引导",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       16, 106, 256, 38, kCommandWritePe);
    CreateControl(window, L"BUTTON", L"删除 PE 引导项",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                  288, 106, 256, 38, kCommandRemovePe);

    CreateControl(window, L"STATIC", L"Windows 分区：", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 164, 92, 22, 0);
    g_tool.comboWindows = CreateControl(window, L"COMBOBOX", L"",
                                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                                        112, 161, 124, 220, kComboWindows);
    CreateControl(window, L"STATIC", L"引导分区：", WS_CHILD | WS_VISIBLE | SS_LEFT, 246, 164, 70, 22, 0);
    g_tool.comboBoot = CreateControl(window, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                                     318, 161, 124, 220, kComboBoot);
    CreateControl(window, L"BUTTON", L"修复系统引导",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                  450, 159, 94, 28, kCommandRepair);

    CreateControl(window, L"BUTTON", L"查看当前启动项",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                  16, 204, kClientWidth - 32, 30, kCommandShowEntries);

    // 日志框：只读多行，占满剩余空间
    g_tool.logEdit = CreateControl(window, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT |
                                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                   16, 246, kClientWidth - 32, kClientHeight - 246 - 16, 0);
    SendMessageW(g_tool.logEdit, EM_SETLIMITTEXT, 0x7FFFFFF0, 0);
}

LRESULT CALLBACK ToolProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        CreateControls(window);
        RefreshPeSources();
        RefreshDriveCombos();
        AppendLog(g_tool.logEdit, IsElevated()
                                      ? L"当前为管理员权限，引导功能可用。"
                                      : L"当前未以管理员身份运行；执行引导操作时会请求提权。");
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == 0)
        {
            HandleCommand(static_cast<int>(LOWORD(wParam)));
        }
        return 0;
    case WM_DESTROY:
        if (g_tool.font != nullptr)
        {
            DeleteObject(g_tool.font);
            g_tool.font = nullptr;
        }

        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    UNREFERENCED_PARAMETER(instance);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ToolProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0)
    {
        return 1;
    }

    RECT rect{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle,
                                  WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr)
    {
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
