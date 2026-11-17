// FireWinPE —— PE 端部署程序
// 语言：C++17 / Win32 API，静态 CRT（/MT），单 exe，无任何运行时依赖。
// 许可证：GPL-3.0-only
//
// 职责：
//   1. 读取桌面端写入的 deploy.json；
//   2. 调用 wimlib-imagex.exe apply 把镜像应用到目标分区；
//   3. 调用 bcdboot.exe 写入引导；
//   4. 删除桌面端创建的一次性 BCD 启动项；
//   5. 写日志、给出结果提示（可选自动重启）。

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <intrin.h> // __cpuid：检测 VMware / Hyper-V 等虚拟机

#include "res/resource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
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

// 取出系统目录（PE 下为 X:\Windows\System32），bcdboot / bcdedit / shutdown 都在这里
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
// 日志
// ---------------------------------------------------------------------------
class Logger
{
public:
    bool Open(const std::wstring& path)
    {
        m_path = path;
        m_file.open(WideToUtf8(path), std::ios::out | std::ios::app | std::ios::binary);
        return m_file.is_open();
    }

    const std::wstring& Path() const { return m_path; }

    void Info(const std::wstring& message) { Write(L"INFO ", message); }
    void Warn(const std::wstring& message) { Write(L"WARN ", message); }
    void Error(const std::wstring& message) { Write(L"ERROR", message); }

private:
    std::wstring m_path;
    std::ofstream m_file;

    static std::wstring Timestamp()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);

        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buffer;
    }

    void Write(const wchar_t* level, const std::wstring& message)
    {
        const std::wstring line = L"[" + Timestamp() + L"] [" + level + L"] " + message + L"\r\n";
        const std::string utf8 = WideToUtf8(line);

        if (m_file.is_open())
        {
            m_file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            m_file.flush();
        }

        // WinPE 中通常由 startnet.cmd 的 conhost 承载输出；没有控制台时忽略即可
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        if (console != nullptr && console != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(console, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
};

// ---------------------------------------------------------------------------
// 极简 JSON 解析：只支持 deploy.json 这种「扁平对象」结构
// ---------------------------------------------------------------------------
class FlatJson
{
public:
    static bool Parse(const std::wstring& text, std::map<std::wstring, std::wstring>& output, std::wstring& error)
    {
        FlatJson parser(text);
        if (!parser.ParseObject(output))
        {
            error = parser.m_error.empty() ? L"JSON 解析失败" : parser.m_error;
            return false;
        }

        return true;
    }

private:
    explicit FlatJson(const std::wstring& text) : m_text(text) {}

    const std::wstring& m_text;
    size_t m_pos = 0;
    std::wstring m_error;

    bool Fail(const wchar_t* message)
    {
        wchar_t buffer[128]{};
        swprintf_s(buffer, L"%s（位置 %zu）", message, m_pos);
        m_error = buffer;
        return false;
    }

    void SkipWhitespace()
    {
        while (m_pos < m_text.size() && (m_text[m_pos] == L' ' || m_text[m_pos] == L'\t' ||
                                         m_text[m_pos] == L'\r' || m_text[m_pos] == L'\n'))
        {
            ++m_pos;
        }
    }

    bool ParseHex4(unsigned& value)
    {
        if (m_pos + 4 > m_text.size())
        {
            return Fail(L"\\u 转义长度不足");
        }

        value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const wchar_t c = m_text[m_pos++];
            value <<= 4;
            if (c >= L'0' && c <= L'9')
            {
                value |= static_cast<unsigned>(c - L'0');
            }
            else if (c >= L'a' && c <= L'f')
            {
                value |= static_cast<unsigned>(c - L'a' + 10);
            }
            else if (c >= L'A' && c <= L'F')
            {
                value |= static_cast<unsigned>(c - L'A' + 10);
            }
            else
            {
                return Fail(L"\\u 转义包含非十六进制字符");
            }
        }

        return true;
    }

    bool ParseString(std::wstring& output)
    {
        if (m_pos >= m_text.size() || m_text[m_pos] != L'"')
        {
            return Fail(L"期望字符串起始引号");
        }

        ++m_pos;
        output.clear();

        while (m_pos < m_text.size())
        {
            const wchar_t c = m_text[m_pos++];
            if (c == L'"')
            {
                return true;
            }

            if (c != L'\\')
            {
                output.push_back(c);
                continue;
            }

            if (m_pos >= m_text.size())
            {
                return Fail(L"字符串以反斜杠结尾");
            }

            const wchar_t escape = m_text[m_pos++];
            switch (escape)
            {
            case L'"': output.push_back(L'"'); break;
            case L'\\': output.push_back(L'\\'); break;
            case L'/': output.push_back(L'/'); break;
            case L'b': output.push_back(L'\b'); break;
            case L'f': output.push_back(L'\f'); break;
            case L'n': output.push_back(L'\n'); break;
            case L'r': output.push_back(L'\r'); break;
            case L't': output.push_back(L'\t'); break;
            case L'u':
            {
                unsigned code = 0;
                if (!ParseHex4(code))
                {
                    return false;
                }

                // 代理对（emoji 等）在路径里不常见，但顺手处理掉
                if (code >= 0xD800 && code <= 0xDBFF && m_pos + 1 < m_text.size() &&
                    m_text[m_pos] == L'\\' && m_text[m_pos + 1] == L'u')
                {
                    m_pos += 2;
                    unsigned low = 0;
                    if (!ParseHex4(low))
                    {
                        return false;
                    }

                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        const unsigned combined = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        output.push_back(static_cast<wchar_t>(0xD800 + ((combined - 0x10000) >> 10)));
                        output.push_back(static_cast<wchar_t>(0xDC00 + ((combined - 0x10000) & 0x3FF)));
                        break;
                    }
                }

                output.push_back(static_cast<wchar_t>(code));
                break;
            }
            default:
                return Fail(L"未知的转义字符");
            }
        }

        return Fail(L"字符串未闭合");
    }

    bool ParseScalar(std::wstring& output)
    {
        const size_t begin = m_pos;
        while (m_pos < m_text.size() && m_text[m_pos] != L',' && m_text[m_pos] != L'}' &&
               m_text[m_pos] != L' ' && m_text[m_pos] != L'\t' && m_text[m_pos] != L'\r' &&
               m_text[m_pos] != L'\n')
        {
            ++m_pos;
        }

        if (m_pos == begin)
        {
            return Fail(L"缺少值");
        }

        output = m_text.substr(begin, m_pos - begin);
        return true;
    }

    bool ParseObject(std::map<std::wstring, std::wstring>& output)
    {
        SkipWhitespace();
        if (m_pos >= m_text.size() || m_text[m_pos] != L'{')
        {
            return Fail(L"根节点必须是对象");
        }

        ++m_pos;
        SkipWhitespace();

        if (m_pos < m_text.size() && m_text[m_pos] == L'}')
        {
            ++m_pos;
            return true;
        }

        for (;;)
        {
            SkipWhitespace();

            std::wstring key;
            if (!ParseString(key))
            {
                return false;
            }

            SkipWhitespace();
            if (m_pos >= m_text.size() || m_text[m_pos] != L':')
            {
                return Fail(L"期望冒号");
            }

            ++m_pos;
            SkipWhitespace();

            std::wstring value;
            if (m_pos < m_text.size() && m_text[m_pos] == L'"')
            {
                if (!ParseString(value))
                {
                    return false;
                }
            }
            else if (m_pos < m_text.size() && m_text[m_pos] == L'{')
            {
                // 本程序不需要嵌套对象，直接跳过
                int depth = 0;
                do
                {
                    if (m_text[m_pos] == L'{')
                    {
                        ++depth;
                    }
                    else if (m_text[m_pos] == L'}')
                    {
                        --depth;
                    }

                    ++m_pos;
                } while (m_pos < m_text.size() && depth > 0);
            }
            else if (!ParseScalar(value))
            {
                return false;
            }

            output[key] = value;

            SkipWhitespace();
            if (m_pos >= m_text.size())
            {
                return Fail(L"对象未闭合");
            }

            if (m_text[m_pos] == L',')
            {
                ++m_pos;
                continue;
            }

            if (m_text[m_pos] == L'}')
            {
                ++m_pos;
                return true;
            }

            return Fail(L"期望逗号或右花括号");
        }
    }
};

// ---------------------------------------------------------------------------
// 部署参数
// ---------------------------------------------------------------------------
struct DeployConfig
{
    std::wstring wimPath;
    std::wstring wimVolumePath;
    int wimIndex = 1;
    std::wstring targetDrive;
    std::wstring targetVolume;
    std::wstring systemDrive;
    std::wstring systemVolume;
    std::wstring firmware = L"UEFI";
    std::wstring bootEntryGuid;
    std::wstring logPath;
    bool autoReboot = true;
};

std::wstring GetValue(const std::map<std::wstring, std::wstring>& values, const wchar_t* key)
{
    const auto it = values.find(key);
    return it == values.end() ? std::wstring() : it->second;
}

bool GetBool(const std::map<std::wstring, std::wstring>& values, const wchar_t* key, bool defaultValue)
{
    const std::wstring value = ToLower(Trim(GetValue(values, key)));
    if (value.empty())
    {
        return defaultValue;
    }

    return value == L"true" || value == L"1" || value == L"yes";
}

bool LoadConfig(const std::wstring& path, DeployConfig& config, std::wstring& error)
{
    std::ifstream file(WideToUtf8(path), std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        error = L"无法打开配置文件：" + path;
        return false;
    }

    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // 去掉 UTF-8 BOM
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF)
    {
        raw.erase(0, 3);
    }

    std::map<std::wstring, std::wstring> values;
    if (!FlatJson::Parse(Utf8ToWide(raw), values, error))
    {
        return false;
    }

    config.wimPath = GetValue(values, L"wimPath");
    config.wimVolumePath = GetValue(values, L"wimVolumePath");
    config.wimIndex = _wtoi(GetValue(values, L"wimIndex").c_str());
    if (config.wimIndex <= 0)
    {
        config.wimIndex = 1;
    }

    config.targetDrive = GetValue(values, L"targetDrive");
    config.targetVolume = GetValue(values, L"targetVolume");
    config.systemDrive = GetValue(values, L"systemDrive");
    config.systemVolume = GetValue(values, L"systemVolume");
    config.firmware = GetValue(values, L"firmware");
    config.bootEntryGuid = GetValue(values, L"bootEntryGuid");
    config.logPath = GetValue(values, L"logPath");
    config.autoReboot = GetBool(values, L"autoReboot", true);

    if (config.firmware.empty())
    {
        config.firmware = L"UEFI";
    }

    return true;
}

// 在常见位置寻找 deploy.json：显式参数 → exe 目录 → C:\FireWinPE → 其它分区
std::wstring LocateConfig(const std::wstring& explicitPath)
{
    if (!explicitPath.empty())
    {
        return FileExists(explicitPath) ? explicitPath : std::wstring();
    }

    const std::wstring exeDir = GetExeDirectory();
    const std::wstring candidates[] = {
        JoinPath(exeDir, L"deploy.json"),
        L"C:\\FireWinPE\\deploy.json",
    };

    for (const std::wstring& candidate : candidates)
    {
        if (FileExists(candidate))
        {
            return candidate;
        }
    }

    for (wchar_t letter = L'C'; letter <= L'Z'; ++letter)
    {
        const std::wstring candidate = std::wstring(1, letter) + L":\\FireWinPE\\deploy.json";
        if (FileExists(candidate))
        {
            return candidate;
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// 卷 → 盘符解析
// PE 里的盘符与桌面端不一致，deploy.json 里保存的卷 GUID 才是稳定标识
// ---------------------------------------------------------------------------
std::wstring NormalizeVolumeName(const std::wstring& input)
{
    if (input.empty())
    {
        return {};
    }

    std::wstring value = input;
    if (value.rfind(L"\\\\?\\Volume", 0) == 0)
    {
        return EnsureTrailingBackslash(value);
    }

    if (value.front() != L'{')
    {
        value = L"{" + value;
    }

    if (value.back() != L'}')
    {
        value += L'}';
    }

    return L"\\\\?\\Volume" + value + L"\\";
}

// 返回形如 "D:" 的盘符；解析不到返回空
std::wstring ResolveVolumeToDriveLetter(const std::wstring& volumeInput)
{
    const std::wstring volume = NormalizeVolumeName(volumeInput);
    if (volume.empty())
    {
        return {};
    }

    if (GetFileAttributesW(volume.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return {};
    }

    wchar_t buffer[1024]{};
    DWORD length = 0;
    if (!GetVolumePathNamesForVolumeNameW(volume.c_str(), buffer, ARRAYSIZE(buffer), &length))
    {
        return {};
    }

    // 返回的是多个以 \0 分隔的挂载点，取第一个
    const std::wstring mountPoint(buffer);
    if (mountPoint.size() >= 2 && mountPoint[1] == L':')
    {
        return mountPoint.substr(0, 2);
    }

    return {};
}

std::wstring NormalizeDrive(const std::wstring& drive)
{
    std::wstring value = Trim(drive);
    while (!value.empty() && value.back() == L'\\')
    {
        value.pop_back();
    }

    if (value.size() == 1 && ((value[0] >= L'a' && value[0] <= L'z') || (value[0] >= L'A' && value[0] <= L'Z')))
    {
        value += L":";
    }

    if (value.size() >= 2)
    {
        value = std::wstring(1, static_cast<wchar_t>(towupper(value[0]))) + value.substr(1, 1);
    }

    return value;
}

bool IsUsableDrive(const std::wstring& drive)
{
    if (drive.size() != 2 || drive[1] != L':')
    {
        return false;
    }

    const std::wstring root = drive + L"\\";
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    const UINT type = GetDriveTypeW(root.c_str());
    return type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN;
}

// ---------------------------------------------------------------------------
// 子进程执行（输出重定向到日志）
// ---------------------------------------------------------------------------
bool RunCommand(const std::wstring& exePath, const std::wstring& arguments, Logger& log, DWORD& exitCode)
{
    exitCode = static_cast<DWORD>(-1);

    if (!FileExists(exePath))
    {
        log.Error(L"找不到可执行文件：" + exePath);
        return false;
    }

    const std::wstring commandLine = L"\"" + exePath + L"\" " + arguments;
    log.Info(L"执行：" + commandLine);

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE logHandle = CreateFileW(log.Path().c_str(),
                                   FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &attributes,
                                   OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (logHandle == INVALID_HANDLE_VALUE)
    {
        log.Warn(L"无法打开日志文件用于输出重定向，子进程输出将不可见。");
    }

    HANDLE nulHandle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (logHandle != INVALID_HANDLE_VALUE)
    {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = logHandle;
        startup.hStdError = logHandle;
        // PE 中可能没有控制台，stdin 用 NUL 兜底，避免 STARTF_USESTDHANDLES 传入无效句柄
        startup.hStdInput = nulHandle != INVALID_HANDLE_VALUE ? nulHandle : nullptr;
    }

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr,
                                        mutableCommandLine.data(),
                                        nullptr,
                                        nullptr,
                                        logHandle != INVALID_HANDLE_VALUE ? TRUE : FALSE,
                                        CREATE_UNICODE_ENVIRONMENT,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &processInfo);

    if (!created)
    {
        const DWORD error = GetLastError();
        log.Error(L"创建进程失败，错误码 " + std::to_wstring(error));
        if (logHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(logHandle);
        }

        if (nulHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(nulHandle);
        }

        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    GetExitCodeProcess(processInfo.hProcess, &exitCode);

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    if (logHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(logHandle);
    }

    if (nulHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(nulHandle);
    }

    log.Info(L"退出码：" + std::to_wstring(exitCode));
    return exitCode == 0;
}

// 运行子进程并把输出接到管道，解析 wimlib-imagex 的 "…(35%) done" 实时进度。
// onPercent 只会上报递增的百分比；输出尾部追加到日志，失败时仍能查原因。
bool RunCommandWithProgress(const std::wstring& exePath, const std::wstring& arguments, Logger& log,
                            const std::function<void(int)>& onPercent, DWORD& exitCode)
{
    exitCode = static_cast<DWORD>(-1);

    if (!FileExists(exePath))
    {
        log.Error(L"找不到可执行文件：" + exePath);
        return false;
    }

    const std::wstring commandLine = L"\"" + exePath + L"\" " + arguments;
    log.Info(L"执行：" + commandLine);

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readPipe = INVALID_HANDLE_VALUE;
    HANDLE writePipe = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0))
    {
        log.Warn(L"创建管道失败，改为普通执行（没有实时进度）。");
        return RunCommand(exePath, arguments, log, exitCode);
    }

    // 读端只留给父进程，否则子进程继承后永远等不到 EOF
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nulHandle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nulHandle != INVALID_HANDLE_VALUE ? nulHandle : nullptr;

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr,
                                        mutableCommandLine.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_UNICODE_ENVIRONMENT,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &processInfo);

    CloseHandle(writePipe);   // 父进程不再持有写端，子进程结束后才能读到 EOF
    if (nulHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(nulHandle);
    }

    if (!created)
    {
        const DWORD error = GetLastError();
        CloseHandle(readPipe);
        log.Error(L"创建进程失败，错误码 " + std::to_wstring(error));
        return false;
    }

    int lastPercent = 0;
    std::string carry;
    std::string tail;
    char buffer[4096];

    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) || read == 0)
        {
            break;
        }

        const std::string piece(buffer, read);
        const std::string chunk = carry + piece;

        // 形如 "Extracting file data: 4 MiB of 21 MiB (22%) done"
        for (size_t pos = chunk.find('%'); pos != std::string::npos; pos = chunk.find('%', pos + 1))
        {
            size_t begin = pos;
            while (begin > 0 && chunk[begin - 1] >= '0' && chunk[begin - 1] <= '9')
            {
                --begin;
            }

            if (begin == pos || pos - begin > 3)
            {
                continue;
            }

            const int percent = std::atoi(chunk.substr(begin, pos - begin).c_str());
            if (percent > lastPercent && percent <= 100)
            {
                lastPercent = percent;
                if (onPercent)
                {
                    onPercent(percent);
                }
            }
        }

        // 数字可能被分到两个数据块里，留 8 个字符做衔接
        carry = chunk.size() > 8 ? chunk.substr(chunk.size() - 8) : chunk;

        tail += piece;
        if (tail.size() > 8192)
        {
            tail.erase(0, tail.size() - 8192);
        }
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    CloseHandle(readPipe);

    if (!tail.empty())
    {
        std::ofstream output(WideToUtf8(log.Path()), std::ios::out | std::ios::app | std::ios::binary);
        if (output.is_open())
        {
            output << tail << "\r\n";
        }
    }

    log.Info(L"退出码：" + std::to_wstring(exitCode));
    return exitCode == 0;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
struct DeploymentResult
{
    bool success = false;
    bool rebootStarted = false;
    std::wstring message;
    std::wstring logPath;
};

void ShowResult(const DeploymentResult& result)
{
    const std::wstring text = result.message + L"\r\n\r\n日志：" + result.logPath;
    MessageBoxW(nullptr,
                text.c_str(),
                L"FireWin PE",
                MB_OK | (result.success ? MB_ICONINFORMATION : MB_ICONERROR) | MB_SETFOREGROUND);
}

// 进度回调：参数为「当前步骤文字 + 百分比」。可为空，部署逻辑不依赖它。
using DeployProgress = std::function<void(const std::wstring&, int)>;

DeploymentResult Deploy(Logger& log, const DeployConfig& config, const std::wstring& toolsDir,
                        const DeployProgress& progress = DeployProgress())
{
    DeploymentResult result;
    result.logPath = log.Path();

    auto report = [&](int percent, const std::wstring& text) {
        if (progress)
        {
            progress(text, percent);
        }
    };

    // ---- 1. 解析镜像路径（优先卷 GUID 路径，避免 PE 盘符漂移） ----
    report(3, L"正在检查镜像与目标分区…");
    std::wstring wimPath = config.wimVolumePath.empty() ? config.wimPath : config.wimVolumePath;
    if (!FileExists(wimPath))
    {
        if (FileExists(config.wimPath))
        {
            log.Warn(L"卷 GUID 路径不可用，回退到原始路径：" + config.wimPath);
            wimPath = config.wimPath;
        }
        else
        {
            result.message = L"找不到镜像文件：" + wimPath;
            log.Error(result.message);
            return result;
        }
    }

    log.Info(L"镜像：" + wimPath + L"，索引：" + std::to_wstring(config.wimIndex));

    // ---- 2. 解析目标分区 ----
    std::wstring targetDrive = ResolveVolumeToDriveLetter(config.targetVolume);
    if (targetDrive.empty())
    {
        targetDrive = NormalizeDrive(config.targetDrive);
        log.Warn(L"未能通过卷 GUID 解析目标分区，使用配置盘符：" + (targetDrive.empty() ? L"(空)" : targetDrive));
    }

    if (!IsUsableDrive(targetDrive))
    {
        result.message = L"目标分区不可用：" + (targetDrive.empty() ? std::wstring(L"(未配置)") : targetDrive) +
                         L"\r\n请确认分区已在 PE 中获得盘符。";
        log.Error(result.message);
        return result;
    }

    const std::wstring targetRoot = targetDrive + L"\\";
    log.Info(L"目标分区：" + targetRoot);

    // ---- 3. wimlib-imagex apply ----
    report(10, L"正在应用系统镜像（这一步耗时最长）…");
    const std::wstring wimlib = JoinPath(toolsDir, L"wimlib-imagex.exe");
    const std::wstring applyArgs = L"apply \"" + wimPath + L"\" " + std::to_wstring(config.wimIndex) +
                                   L" \"" + targetRoot + L"\" --check";

    // wimlib 会不断输出 "…(35%) done"，解析出来驱动进度条；映射到 12%–88%
    DWORD exitCode = 0;
    const bool applied = RunCommandWithProgress(
        wimlib, applyArgs, log,
        [&](int percent)
        {
            const int mapped = 12 + percent * 76 / 100;
            report(mapped, L"正在写入系统文件… " + std::to_wstring(percent) + L"%");
        },
        exitCode);
    if (!applied)
    {
        result.message = L"镜像应用失败（wimlib-imagex 退出码 " + std::to_wstring(exitCode) + L"）。";
        log.Error(result.message);
        return result;
    }

    log.Info(L"镜像应用完成。");

    // ---- 4. bcdboot 写入引导 ----
    report(90, L"镜像应用完成，正在写入启动引导…");
    std::wstring systemDrive = ResolveVolumeToDriveLetter(config.systemVolume);
    if (systemDrive.empty())
    {
        systemDrive = NormalizeDrive(config.systemDrive);
    }

    if (!IsUsableDrive(systemDrive))
    {
        result.message = L"引导分区不可用：" + (systemDrive.empty() ? std::wstring(L"(未配置)") : systemDrive) +
                         L"\r\n请确认 ESP/系统分区已分配盘符。";
        log.Error(result.message);
        return result;
    }

    const bool uefi = ToLower(Trim(config.firmware)) != L"bios";
    const std::wstring bcdbootArgs = L"\"" + targetRoot + L"Windows\" /s " + systemDrive +
                                     L" /f " + (uefi ? L"UEFI" : L"BIOS");
    const std::wstring systemDir = GetSystemDirectoryPath();
    const std::wstring bcdboot = JoinPath(systemDir, L"bcdboot.exe");

    if (!RunCommand(bcdboot, bcdbootArgs, log, exitCode))
    {
        result.message = L"写入引导失败（bcdboot 退出码 " + std::to_wstring(exitCode) + L"）。";
        log.Error(result.message);
        return result;
    }

    log.Info(L"引导写入完成（" + std::wstring(uefi ? L"UEFI" : L"BIOS") + L"）。");
    report(92, L"引导写入完成，正在收尾…");

    // ---- 5. 清理桌面端写入的一次性启动项 ----
    if (!config.bootEntryGuid.empty())
    {
        const std::wstring bcdedit = JoinPath(systemDir, L"bcdedit.exe");
        const std::wstring deleteArgs = L"/delete " + config.bootEntryGuid + L" /f";
        if (!RunCommand(bcdedit, deleteArgs, log, exitCode))
        {
            log.Warn(L"清理临时启动项失败，可进入系统后手动执行：bcdedit /delete " + config.bootEntryGuid + L" /f");
        }
    }

    result.success = true;
    result.message = L"部署完成。Windows 已应用到 " + targetRoot + L"。";
    report(100, L"部署完成");

    // ---- 6. 可选自动重启 ----
    if (config.autoReboot)
    {
        const std::wstring shutdown = JoinPath(systemDir, L"shutdown.exe");
        if (RunCommand(shutdown, L"/r /t 15", log, exitCode))
        {
            result.rebootStarted = true;
            result.message += L"\r\n系统将在 15 秒后重启。";
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// 高分屏适配：全局 UI 缩放
// WinPE 的基本显示驱动几乎总是工作在 96 DPI，GetDeviceCaps 拿不到真实 DPI，
// 于是再按屏幕高度估算一个缩放比例（768p 为 100%），保证在 2K / 4K 屏幕上
// 任务栏、桌面图标、字体不会显得特别小。
// 所有布局常量都按 96 DPI 的原始像素写，用 UiScale() 换算后使用。
// ---------------------------------------------------------------------------
int g_uiScale = 100;   // 百分比：100 = 768p 下的原始尺寸

int UiScale(int value)
{
    return MulDiv(value, g_uiScale, 100);
}

void UpdateUiScale()
{
    int percent = 100;

    if (HDC screen = GetDC(nullptr))
    {
        const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        if (dpi > 0)
        {
            percent = std::max(percent, MulDiv(100, dpi, 96));
        }
    }

    // 以 768p 为基准按屏幕高度等比放大；上限 250%，避免 4K 下布局过于夸张
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (screenHeight > 0)
    {
        percent = std::max(percent, MulDiv(100, screenHeight, 768));
    }

    g_uiScale = std::min(250, std::max(100, percent));
}

void EnableDpiAwareness()
{
    // 自己按比例缩放界面，先声明 DPI 感知，避免系统再拉伸一次位图导致文字发虚。
    // 动态取函数：老系统 / 精简 PE 上没有就退回 SetProcessDPIAware。
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setContext = user32 != nullptr
                          ? reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
                          : nullptr;
    const HANDLE perMonitorAwareV2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
    if (setContext != nullptr && setContext(perMonitorAwareV2))
    {
        return;
    }

    SetProcessDPIAware();
}

// ---------------------------------------------------------------------------
// 图形启动器
// 无参数（或 /shell）时进入：先用常用工具检查磁盘、浏览文件，再点「开始安装」。
// 纯 Win32 API 代码建窗，不使用 .rc 资源文件，也不引入第三方库。
// ---------------------------------------------------------------------------
constexpr wchar_t kLauncherClass[] = L"FireWinPELauncher";
constexpr wchar_t kLauncherTitle[] = L"FireWin PE";
constexpr UINT kMessageDeployFinished = WM_APP + 1;
// 部署进度：界面线程收到后刷新状态行和进度条
constexpr UINT kMessageDeployProgress = WM_APP + 2;

// Win11 风格自绘窗口：自绘标题栏高度 / 配色（启动器窗口用，桌面模块另有同名配色）
// 尺寸按 96 DPI 写成基准值，运行时由 InitUiMetrics() 按 g_uiScale 换算
int kLauncherTitleBar = 44;
int kLauncherCorner = 16;
int kLauncherTitleButton = 46;
int kLauncherProgressHeight = 18;   // 底部进度条高度
constexpr COLORREF kLauncherChrome = RGB(28, 28, 28);
constexpr COLORREF kLauncherCard = RGB(45, 45, 45);
constexpr COLORREF kLauncherHover = RGB(63, 63, 63);
constexpr COLORREF kLauncherBorder = RGB(70, 70, 70);
constexpr COLORREF kLauncherText = RGB(255, 255, 255);
constexpr COLORREF kLauncherMuted = RGB(158, 158, 158);
constexpr COLORREF kLauncherAccent = RGB(76, 194, 255);
constexpr COLORREF kLauncherAccentButton = RGB(0, 120, 212);   // Win11 强调色按钮
constexpr COLORREF kLauncherCloseHover = RGB(196, 43, 28);     // Win11 关闭按钮悬停红

// 客户区宽度 / 高度随按钮数量（含 \Tools\Apps 扫描出的动态工具）动态计算：
// 工具多时自动增加列数并加宽窗口，再把高度压回可用屏幕范围内

enum LauncherCommand : int
{
    kCommandDeploy = 1001,
    kCommandInstall,
    kCommandExplorer,
    kCommandSevenZip,
    kCommandNotepadPlusPlus,
    kCommandNotepad,
    kCommandRegedit,
    kCommandCommandPrompt,
    kCommandDiskpart,
    kCommandBootTool,
    kCommandLoadDriver,
    kCommandInitNetwork,
    kCommandDisplayMode,
    kCommandReboot,
    // 动态工具按钮从 1200 起（对应 g_launcher.appTools 下标）
    kCommandShutdown,
    kCommandAppFirst = 1200,
};

// 扫描到的工具：exe 路径 + 显示名 + 开始菜单分组 + 一句话说明
struct AppTool
{
    std::wstring exe;
    std::wstring title;
    std::wstring category;
    std::wstring hint;
};

struct LauncherState
{
    HWND window = nullptr;
    HWND statusText = nullptr;
    HWND deployButton = nullptr;
    HWND installButton = nullptr;
    HFONT font = nullptr;
    std::wstring configPath;
    std::wstring toolsDir;
    bool deploying = false;
    std::thread deployThread;
    std::unique_ptr<DeployConfig> manualConfig;   // 「安装系统」手动选择的部署参数

    // 动态工具：扫描 X:\Tools\Apps 得到（含开始菜单分组与一句话说明）
    std::vector<AppTool> appTools;

    // 窗口布局：工具越多列数越多，保证在 768p 的 PE 屏幕上也放得下
    int clientWidth = 520;
    int clientHeight = 500;
    int columns = 2;

    // 整窗背景图（res\background.bmp，编译进 exe 资源）
    HBITMAP background = nullptr;

    // 自绘标题栏按钮状态：0 无 / 1 最小化 / 2 关闭；tracking 用于 WM_MOUSELEAVE
    int titleHover = 0;
    int titlePressed = 0;
    bool titleTracking = false;

    // 自绘标题栏左侧的程序图标（LR_SHARED，不需要 DestroyIcon）
    HICON icon = nullptr;

    // 部署进度：后台线程写入，界面线程在 WM_APP+2 里取出来更新状态行并重绘进度条
    std::mutex progressMutex;
    std::wstring progressText;
    int progressPercent = 0;
};

LauncherState g_launcher;

// 字体枚举回调：只要能回调到，就说明该字体存在
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

HFONT CreateLauncherFont()
{
    // WinPE 精简字体集不一定有雅黑：按顺序挑第一个存在的字体，避免中文显示成方块
    const wchar_t* candidates[] = { L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimSun", L"NSimSun" };

    LOGFONTW description{};
    description.lfHeight = -UiScale(16);
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

void ApplyLauncherFont(HWND control)
{
    if (g_launcher.font != nullptr)
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_launcher.font), TRUE);
    }
}

// 启动外部工具：不阻塞启动器，子进程句柄用完即关
bool LaunchTool(const std::wstring& exePath, const std::wstring& arguments, bool newConsole,
                const std::wstring& workingDirectory)
{
    if (!FileExists(exePath))
    {
        const std::wstring message = L"找不到工具：\r\n" + exePath;
        MessageBoxW(g_launcher.window, message.c_str(), kLauncherTitle, MB_OK | MB_ICONWARNING);
        return false;
    }

    std::wstring commandLine = L"\"" + exePath + L"\"";
    if (!arguments.empty())
    {
        commandLine += L" " + arguments;
    }

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(exePath.c_str(),
                                        mutableCommandLine.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        newConsole ? CREATE_NEW_CONSOLE : 0,
                                        nullptr,
                                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                        &startup,
                                        &processInfo);
    if (!created)
    {
        const DWORD error = GetLastError();
        const std::wstring message = L"启动失败：" + exePath + L"\r\n错误码：" + std::to_wstring(error);
        MessageBoxW(g_launcher.window, message.c_str(), kLauncherTitle, MB_OK | MB_ICONERROR);
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

// Explorer++ 的界面语言保存在 HKCU\Software\Explorer++\Settings 的 Language 值里。
// PE 里没有设置窗口可点，启动前直接写注册表，菜单/按钮就会走 Explorer++ZH_CN.dll。
void ApplyExplorerLanguage(const std::wstring& directory)
{
    if (!FileExists(JoinPath(directory, L"Explorer++ZH_CN.dll")))
    {
        return;
    }

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Explorer++\\Settings", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    const wchar_t language[] = L"zh-CN";
    RegSetValueExW(key, L"Language", 0, REG_SZ, reinterpret_cast<const BYTE*>(language), sizeof(language));
    RegCloseKey(key);
}

// 系统目录（X:\Windows\System32）下的工具：notepad / regedit / cmd / diskpart
void LaunchSystemTool(const wchar_t* fileName, bool newConsole, const std::wstring& workingDirectory)
{
    LaunchTool(JoinPath(GetSystemDirectoryPath(), fileName), std::wstring(), newConsole, workingDirectory);
}

// 优先 wpeutil，PE 里没有该文件时回退 ExitWindowsEx
void PerformPowerAction(bool reboot)
{
    const std::wstring wpeutil = JoinPath(GetSystemDirectoryPath(), L"wpeutil.exe");
    if (FileExists(wpeutil))
    {
        LaunchTool(wpeutil, reboot ? L"reboot" : L"shutdown", false, std::wstring());
        return;
    }

    const UINT flags = reboot ? static_cast<UINT>(EWX_REBOOT | EWX_FORCEIFHUNG)
                              : static_cast<UINT>(EWX_SHUTDOWN | EWX_POWEROFF | EWX_FORCEIFHUNG);
    if (!ExitWindowsEx(flags, 0))
    {
        const std::wstring message = std::wstring(L"无法执行") + (reboot ? L"重启" : L"关机") + L"。";
        MessageBoxW(g_launcher.window, message.c_str(), kLauncherTitle, MB_OK | MB_ICONERROR);
    }
}

// ---------------------------------------------------------------------------
// U 盘安装支持：扫描 \Images 镜像、选择版本与分区、构造配置并部署
// 使启动盘在没有 deploy.json 时也能独立完成系统安装
// ---------------------------------------------------------------------------
void DeployWorker();

// 运行子进程并捕获输出（重定向到临时文件后读回，兼容 PE 中无控制台的场景）
bool RunCapture(const std::wstring& exePath, const std::wstring& arguments,
                DWORD& exitCode, std::wstring& output)
{
    exitCode = static_cast<DWORD>(-1);
    output.clear();

    if (!FileExists(exePath))
    {
        return false;
    }

    const std::wstring tempFile = JoinPath(GetExeDirectory(), L"cmdout.tmp");
    DeleteFileW(tempFile.c_str());

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE fileHandle = CreateFileW(tempFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                    &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = fileHandle;
    startup.hStdError = fileHandle;
    startup.hStdInput = nullptr;

    const std::wstring commandLine = L"\"" + exePath + L"\" " + arguments;
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr,
                                        mutableCommandLine.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_UNICODE_ENVIRONMENT,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &processInfo);
    CloseHandle(fileHandle);
    if (!created)
    {
        DeleteFileW(tempFile.c_str());
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    HANDLE readHandle = CreateFileW(tempFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (readHandle != INVALID_HANDLE_VALUE)
    {
        std::string raw;
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(readHandle, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            raw.append(buffer, read);
        }

        CloseHandle(readHandle);
        output = Utf8ToWide(raw);
    }

    DeleteFileW(tempFile.c_str());
    return true;
}

// 固件类型：0 未知 / 1 BIOS / 2 UEFI（与 FireBoot 保持一致）
int DetectFirmwareKind()
{
    // Win8+ kernel32 提供 GetFirmwareType；PE 中一般可用
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 != nullptr)
    {
        using GetFirmwareTypeFn = BOOL(WINAPI*)(unsigned*);
        const auto getFirmwareType =
            reinterpret_cast<GetFirmwareTypeFn>(reinterpret_cast<void*>(GetProcAddress(kernel32, "GetFirmwareType")));
        unsigned firmwareType = 0;
        if (getFirmwareType != nullptr && getFirmwareType(&firmwareType) &&
            (firmwareType == 1 || firmwareType == 2))
        {
            return static_cast<int>(firmwareType);
        }
    }

    DWORD exitCode = 0;
    std::wstring output;
    const std::wstring bcdEdit = JoinPath(GetSystemDirectoryPath(), L"bcdedit.exe");
    if (RunCapture(bcdEdit, L"/enum {current}", exitCode, output) && exitCode == 0)
    {
        const std::wstring lowered = ToLower(output);
        if (lowered.find(L"winload.efi") != std::wstring::npos)
        {
            return 2;
        }

        if (lowered.find(L"winload.exe") != std::wstring::npos)
        {
            return 1;
        }
    }

    return 0;
}

// 分区信息（安装目标 / 引导分区候选）
struct VolumeEntry
{
    std::wstring drive;
    std::wstring label;
    std::wstring filesystem;
    unsigned long long totalBytes = 0;
    unsigned long long freeBytes = 0;
    bool isEsp = false;   // FAT32 且小于 2GB，按 ESP 处理
};

std::wstring FormatBytesText(unsigned long long bytes)
{
    wchar_t buffer[64]{};
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int unit = 0;
    double value = static_cast<double>(bytes);
    while (value >= 1024.0 && unit < 4)
    {
        value /= 1024.0;
        ++unit;
    }

    swprintf_s(buffer, L"%.1f %s", value, units[unit]);
    return buffer;
}

std::vector<VolumeEntry> GetFixedVolumes(bool excludeRamdiskDrive)
{
    std::vector<VolumeEntry> volumes;

    wchar_t buffer[512]{};
    const DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(buffer) - 1, buffer);
    if (length == 0 || length >= ARRAYSIZE(buffer))
    {
        return volumes;
    }

    for (const wchar_t* cursor = buffer; *cursor != L'\0'; cursor += lstrlenW(cursor) + 1)
    {
        const std::wstring root(cursor);   // "D:\"
        if (root.size() < 3 || GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
        {
            continue;
        }

        const std::wstring drive = root.substr(0, 2);
        if (excludeRamdiskDrive && _wcsicmp(drive.c_str(), L"X:") == 0)
        {
            continue;
        }

        ULARGE_INTEGER total{};
        ULARGE_INTEGER freeBytes{};
        GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, &freeBytes);

        VolumeEntry entry;
        entry.drive = drive;
        entry.totalBytes = total.QuadPart;
        entry.freeBytes = freeBytes.QuadPart;

        wchar_t volumeName[MAX_PATH + 1]{};
        wchar_t filesystem[MAX_PATH + 1]{};
        DWORD serial = 0;
        DWORD maxComponent = 0;
        DWORD flags = 0;
        if (GetVolumeInformationW(root.c_str(), volumeName, ARRAYSIZE(volumeName),
                                  &serial, &maxComponent, &flags,
                                  filesystem, ARRAYSIZE(filesystem)))
        {
            entry.label = volumeName;
            entry.filesystem = filesystem;
        }

        entry.isEsp = _wcsicmp(entry.filesystem.c_str(), L"FAT32") == 0 &&
                      entry.totalBytes < 2ULL * 1024 * 1024 * 1024;
        volumes.push_back(entry);
    }

    return volumes;
}

std::wstring FormatVolumeItem(const VolumeEntry& volume)
{
    std::wstring item = volume.drive + L"  " +
                        (volume.label.empty() ? std::wstring(L"(无卷标)") : volume.label) +
                        L"  " + (volume.filesystem.empty() ? std::wstring(L"未知") : volume.filesystem) +
                        L"  总 " + FormatBytesText(volume.totalBytes) +
                        L"  可用 " + FormatBytesText(volume.freeBytes);
    if (volume.isEsp)
    {
        item += L"  [引导分区/ESP]";
    }

    return item;
}

// 扫描各分区的镜像目录：优先 \FWIMG（制作工具自动创建），兼容旧版 \Images
std::vector<std::wstring> FindImagesOnVolumes()
{
    std::vector<std::wstring> images;

    wchar_t buffer[512]{};
    const DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(buffer) - 1, buffer);
    if (length == 0 || length >= ARRAYSIZE(buffer))
    {
        return images;
    }

    // FWIMG 优先列出，之后追加 \Images 中的镜像（隐藏的启动分区没有盘符，不会被枚举）
    for (const wchar_t* folderName : { L"FWIMG", L"Images" })
    {
        for (const wchar_t* cursor = buffer; *cursor != L'\0'; cursor += lstrlenW(cursor) + 1)
        {
            const std::wstring root(cursor);
            if (root.size() < 3)
            {
                continue;
            }

            const std::wstring directory = root.substr(0, 2) + L"\\" + folderName;
            const DWORD attributes = GetFileAttributesW(directory.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }

            for (const wchar_t* pattern : { L"*.wim", L"*.esd", L"*.swm" })
            {
                WIN32_FIND_DATAW find{};
                HANDLE handle = FindFirstFileW((directory + L"\\" + pattern).c_str(), &find);
                if (handle == INVALID_HANDLE_VALUE)
                {
                    continue;
                }

                do
                {
                    if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    {
                        const std::wstring path = directory + L"\\" + find.cFileName;
                        if (std::find(images.begin(), images.end(), path) == images.end())
                        {
                            images.push_back(path);
                        }
                    }
                } while (FindNextFileW(handle, &find));

                FindClose(handle);
            }
        }
    }

    return images;
}

// GetOpenFileNameW 浏览任意镜像文件
std::wstring BrowseForImage(HWND parent, const std::wstring& initialDirectory)
{
    wchar_t fileName[MAX_PATH * 2]{};

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = parent;
    dialog.lpstrFilter = L"系统镜像 (*.wim;*.esd;*.swm)\0*.wim;*.esd;*.swm\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = ARRAYSIZE(fileName);
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrTitle = L"选择 Windows 系统镜像";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&dialog) == FALSE)
    {
        return {};
    }

    return std::wstring(fileName);
}

// 用 wimlib info 解析镜像索引数量与名称；解析失败时按单索引处理
int GetWimImageCount(const std::wstring& wimPath, std::vector<std::wstring>& names)
{
    names.clear();
    int count = 1;

    const std::wstring wimlib = JoinPath(g_launcher.toolsDir, L"wimlib-imagex.exe");
    DWORD exitCode = 0;
    std::wstring output;
    if (!RunCapture(wimlib, L"info \"" + wimPath + L"\"", exitCode, output) || exitCode != 0)
    {
        return count;
    }

    std::vector<std::wstring> lines;
    {
        size_t begin = 0;
        while (begin <= output.size())
        {
            const size_t end = output.find(L'\n', begin);
            std::wstring line = output.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
            {
                line.pop_back();
            }

            lines.push_back(line);
            if (end == std::wstring::npos)
            {
                break;
            }

            begin = end + 1;
        }
    }

    bool inImageSection = false;
    for (const std::wstring& line : lines)
    {
        const std::wstring lowered = ToLower(line);
        if (lowered.find(L"image count:") != std::wstring::npos)
        {
            const std::wstring digits = Trim(line.substr(line.find(L':') + 1));
            const int parsed = _wtoi(digits.c_str());
            if (parsed > 0)
            {
                count = parsed;
            }

            continue;
        }

        if (lowered.find(L"available images") != std::wstring::npos ||
            lowered.find(L"index:") != std::wstring::npos)
        {
            inImageSection = lowered.find(L"available images") != std::wstring::npos;
            if (inImageSection)
            {
                continue;
            }
        }

        if (lowered.find(L"name:") == 0)
        {
            names.push_back(Trim(line.substr(line.find(L':') + 1)));
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// 选择弹窗（模态：禁用主窗口 + 独立消息循环）
// ---------------------------------------------------------------------------
constexpr wchar_t kPickerClass[] = L"FireWinPEPicker";
constexpr int kPickerOk = 2001;
constexpr int kPickerCancel = 2002;
constexpr int kPickerList1 = 2003;
constexpr int kPickerList2 = 2004;

struct PickerState
{
    HWND window = nullptr;
    HWND list1 = nullptr;
    HWND list2 = nullptr;
    HWND okButton = nullptr;
    std::vector<std::wstring> items1;
    std::vector<std::wstring> items2;
    int selection1 = -1;
    int selection2 = -1;
    bool requireBoth = false;
    bool confirmed = false;
};

void ApplyPickerFont(HWND control)
{
    if (g_launcher.font != nullptr)
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_launcher.font), TRUE);
    }
}

HWND CreatePickerControl(HWND window, const wchar_t* className, const wchar_t* text, DWORD style,
                         int x, int y, int width, int height, int command = 0)
{
    HWND control_ = CreateWindowExW(0, className, text, style,
                                    x, y, width, height,
                                    window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(command)),
                                    GetModuleHandleW(nullptr), nullptr);
    ApplyPickerFont(control_);
    return control_;
}

void UpdatePickerOkState(PickerState* state)
{
    if (state->okButton == nullptr)
    {
        return;
    }

    const bool ok = state->requireBoth
                        ? state->selection1 >= 0 && state->selection2 >= 0
                        : state->selection1 >= 0;
    EnableWindow(state->okButton, ok ? TRUE : FALSE);
}

void FillPickerList(HWND listBox, const std::vector<std::wstring>& items, int preselect)
{
    for (const std::wstring& item : items)
    {
        SendMessageW(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }

    if (preselect >= 0 && preselect < static_cast<int>(items.size()))
    {
        SendMessageW(listBox, LB_SETCURSEL, static_cast<WPARAM>(preselect), 0);
    }
}

LRESULT CALLBACK PickerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<PickerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message)
    {
    case WM_CREATE:
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return 0;
    }
    case WM_COMMAND:
        if (state != nullptr)
        {
            const int command = static_cast<int>(LOWORD(wParam));
            const int notification = static_cast<int>(HIWORD(wParam));
            if (command == kPickerOk)
            {
                state->confirmed = true;
                DestroyWindow(window);
                return 0;
            }

            if (command == kPickerCancel)
            {
                DestroyWindow(window);
                return 0;
            }

            if (command == kPickerList1 && notification == LBN_SELCHANGE)
            {
                state->selection1 = static_cast<int>(SendMessageW(state->list1, LB_GETCURSEL, 0, 0));
            }

            if (command == kPickerList2 && notification == LBN_SELCHANGE)
            {
                state->selection2 = static_cast<int>(SendMessageW(state->list2, LB_GETCURSEL, 0, 0));
            }

            UpdatePickerOkState(state);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

// 单列表选择（返回 0 起始索引；取消返回 -1）
int ShowSinglePicker(HWND parent, const wchar_t* title, const wchar_t* prompt,
                     const std::vector<std::wstring>& items)
{
    PickerState state;
    state.items1 = items;
    state.requireBoth = false;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const int clientWidth = 480;
    const int clientHeight = 90 + 220 + 60;
    RECT rect{0, 0, clientWidth, clientHeight};
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    const int x = (GetSystemMetrics(SM_CXSCREEN) - (rect.right - rect.left)) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - (rect.bottom - rect.top)) / 2;

    state.window = CreateWindowExW(0, kPickerClass, title, style, x, y,
                                   rect.right - rect.left, rect.bottom - rect.top,
                                   parent, nullptr, GetModuleHandleW(nullptr), &state);

    CreatePickerControl(state.window, L"STATIC", prompt, WS_CHILD | WS_VISIBLE,
                        16, 14, clientWidth - 32, 40);
    state.list1 = CreatePickerControl(state.window, L"LISTBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_HASSTRINGS,
                                      16, 60, clientWidth - 32, 220, kPickerList1);
    FillPickerList(state.list1, state.items1, 0);
    state.selection1 = 0;

    state.okButton = CreatePickerControl(state.window, L"BUTTON", L"确定",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         clientWidth - 206, clientHeight - 44, 90, 32, kPickerOk);
    CreatePickerControl(state.window, L"BUTTON", L"取消",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        clientWidth - 106, clientHeight - 44, 90, 32, kPickerCancel);
    UpdatePickerOkState(&state);

    EnableWindow(parent, FALSE);
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return state.confirmed ? state.selection1 : -1;
}

// 「安装系统」四步向导（镜像 → 版本 → 磁盘 → 摘要）在桌面 UI 代码之后实现
void RunInstallWizard();

// 「加载驱动」入口：扫描各分区 \Drivers 下的 INF → drvload 加载（循环加载直到取消）
// 用于 U 盘启动后补齐缺失的基础驱动（网卡 / 显卡 / 存储控制器等）
void CollectInfFiles(const std::wstring& directory, int depth, std::vector<std::wstring>& results)
{
    if (depth > 3)
    {
        return;
    }

    WIN32_FIND_DATAW find{};
    HANDLE handle = FindFirstFileW((directory + L"\\*").c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        const std::wstring name = find.cFileName;
        if (name == L"." || name == L"..")
        {
            continue;
        }

        const std::wstring fullPath = directory + L"\\" + name;
        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            CollectInfFiles(fullPath, depth + 1, results);
        }
        else
        {
            const std::wstring lowered = ToLower(name);
            if (lowered.size() > 4 && lowered.rfind(L".inf") == lowered.size() - 4)
            {
                results.push_back(fullPath);
            }
        }
    } while (FindNextFileW(handle, &find));

    FindClose(handle);
}

std::vector<std::wstring> FindInfFilesOnVolumes()
{
    std::vector<std::wstring> results;

    wchar_t buffer[512]{};
    const DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(buffer) - 1, buffer);
    if (length == 0 || length >= ARRAYSIZE(buffer))
    {
        return results;
    }

    for (const wchar_t* cursor = buffer; *cursor != L'\0'; cursor += lstrlenW(cursor) + 1)
    {
        const std::wstring root(cursor);
        if (root.size() < 3)
        {
            continue;
        }

        CollectInfFiles(root.substr(0, 2) + L"\\Drivers", 0, results);
    }

    return results;
}

std::wstring BrowseForInf(HWND parent, const std::wstring& initialDirectory)
{
    wchar_t fileName[MAX_PATH * 2]{};

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = parent;
    dialog.lpstrFilter = L"驱动 INF (*.inf)\0*.inf\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = ARRAYSIZE(fileName);
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrTitle = L"选择驱动 INF 文件";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&dialog) == FALSE)
    {
        return {};
    }

    return std::wstring(fileName);
}

void StartDriverLoader()
{
    const std::wstring drvload = JoinPath(GetSystemDirectoryPath(), L"drvload.exe");
    if (!FileExists(drvload))
    {
        MessageBoxW(g_launcher.window,
                    L"当前 PE 缺少 drvload.exe，无法加载驱动。",
                    kLauncherTitle, MB_OK | MB_ICONERROR);
        return;
    }

    for (;;)
    {
        std::vector<std::wstring> infs = FindInfFilesOnVolumes();
        std::wstring infPath;

        if (infs.empty())
        {
            infPath = BrowseForInf(g_launcher.window, std::wstring());
            if (infPath.empty())
            {
                return;
            }
        }
        else
        {
            std::vector<std::wstring> items = infs;
            items.push_back(L"浏览其它 INF 文件…");
            const int picked = ShowSinglePicker(g_launcher.window,
                                                L"加载驱动",
                                                L"在以下位置的 \\Drivers 目录找到驱动 INF（把驱动放到任意分区 \\Drivers\\ 即可被扫描到）：",
                                                items);
            if (picked < 0)
            {
                return;
            }

            if (picked == static_cast<int>(infs.size()))
            {
                wchar_t buffer[8]{};
                wcscpy_s(buffer, infs[0].substr(0, 2).c_str());
                wcscat_s(buffer, L"\\");
                infPath = BrowseForInf(g_launcher.window, buffer);
                if (infPath.empty())
                {
                    return;
                }
            }
            else
            {
                infPath = infs[static_cast<size_t>(picked)];
            }
        }

        DWORD exitCode = 0;
        std::wstring output;
        const bool launched = RunCapture(drvload, L"\"" + infPath + L"\"", exitCode, output);
        if (launched && exitCode == 0)
        {
            const std::wstring message = L"驱动加载成功：\r\n" + infPath +
                                         L"\r\n\r\n部分设备需要重启后才能完全生效。要继续加载其它驱动吗？";
            if (MessageBoxW(g_launcher.window, message.c_str(), kLauncherTitle,
                            MB_YESNO | MB_ICONINFORMATION) != IDYES)
            {
                return;
            }
        }
        else
        {
            const std::wstring message = L"驱动加载失败（退出码 " + std::to_wstring(exitCode) + L"）：" +
                                         infPath +
                                         L"\r\n\r\n常见原因：INF 不适用于 WinPE（x64）、缺少配套文件（把整个驱动目录一起放到 \\Drivers\\）、或驱动本身需要完整 Windows。";
            MessageBoxW(g_launcher.window, message.c_str(), kLauncherTitle, MB_OK | MB_ICONWARNING);
        }
    }
}

// ---------------------------------------------------------------------------
// 动态工具按钮：扫描 X:\Tools\Apps\<目录>，参考市面常见 PE（微PE / 优启通等）的工具集
// 已知目录给出友好名称与推荐 exe；未知目录取目录里第一个 exe，按钮文字用目录名。
// 构建时由 build-minipe.ps1 把 pe\apps 递归复制到 PE 的 \Tools\Apps。
// ---------------------------------------------------------------------------
struct KnownApp
{
    const wchar_t* folder;      // X:\Tools\Apps\<folder>
    const wchar_t* display;     // 按钮文字
    const wchar_t* exeHint;     // exe 文件名关键字（小写匹配；空则取目录里第一个 exe）
    const wchar_t* category;    // 开始菜单分组
    const wchar_t* hint;        // 一句话说明
};

// 目录名与下载脚本 scripts\download-pe-apps.ps1 保持一致；工具未下载时自动跳过。
constexpr KnownApp kKnownApps[] = {
    { L"DiskGenius",      L"DiskGenius 分区",       L"diskgenius",     L"分区与磁盘", L"分区、格式化与数据恢复" },
    { L"AOMEI",           L"傲梅分区助手",          L"partassist",     L"分区与磁盘", L"无损调整分区大小、合并与克隆分区" },
    { L"WinNTSetup",      L"WinNTSetup 安装器",     L"winntsetup",     L"安装与部署", L"图形化安装 Windows 镜像" },
    { L"BOOTICE",         L"BOOTICE 引导维护",      L"bootice",        L"安装与部署", L"编辑主引导记录与分区引导记录" },
    { L"Dism++",          L"Dism++ 系统精简",       L"dism++",         L"系统维护",   L"系统映像、驱动与启动项维护" },
    { L"CrystalDiskInfo", L"硬盘健康检测",          L"diskinfo",       L"硬件检测",   L"查看硬盘 SMART 健康状态" },
    { L"CPU-Z",           L"CPU-Z 硬件检测",        L"cpuz_x64",       L"硬件检测",   L"查看处理器、主板与内存信息" },
    { L"GPU-Z",           L"GPU-Z 显卡检测",        L"gpu-z",          L"硬件检测",   L"查看显卡型号、温度与显存" },
    { L"HWiNFO",          L"HWiNFO 硬件信息",       L"hwinfo",         L"硬件检测",   L"全面的硬件信息与传感器读数" },
    { L"ProcExp",         L"任务管理器",            L"procexp64",      L"系统维护",   L"进程、句柄与性能查看" },
    { L"SystemInformer",  L"进程高级管理",          L"systeminformer.exe", L"系统维护", L"进程、服务与内核对象分析" },
    { L"Autoruns",        L"启动项管理",            L"autoruns64.exe", L"系统维护",   L"查看并清理开机自启项" },
    { L"NTPWEdit",        L"Windows 密码清除",      L"ntpwedit64",     L"系统维护",   L"清空本地账户密码" },
    { L"TestDisk",        L"分区表恢复",            L"testdisk_win",   L"分区与磁盘", L"修复分区表与丢失的分区" },
    { L"TestDisk",        L"PhotoRec 文件恢复",     L"photorec",       L"分区与磁盘", L"恢复误删的文件与照片（随 TestDisk 一起）" },
    { L"Everything",      L"Everything 搜索",       L"everything",     L"文件与网络", L"秒级文件名搜索" },
    { L"Q-Dir",           L"Q-Dir 文件管理",        L"q-dir_x64",      L"文件与网络", L"四窗格文件管理器" },
    { L"TCPView",         L"网络连接查看",          L"tcpview64",      L"文件与网络", L"查看 TCP / UDP 连接" },
    { L"Chrome",          L"Chrome 浏览器",         L"chrome",         L"文件与网络", L"联网下载驱动、镜像与工具" },
    { L"RustDesk",        L"RustDesk 远程协助",     L"rustdesk",       L"文件与网络", L"开源远程桌面，可自建服务器" },
    { L"ToDesk",          L"ToDesk 远程协助",       L"todesk.exe",     L"文件与网络", L"轻量远程控制" },
};

// 启动器已有同名静态按钮的工具目录，不再重复生成动态按钮
bool HasStaticButtonFor(const std::wstring& folder)
{
    static const wchar_t* const kStaticFolders[] = { L"7-Zip", L"Notepad++", L"Explorer++", L"SDelete" };
    for (const wchar_t* name : kStaticFolders)
    {
        if (_wcsicmp(folder.c_str(), name) == 0)
        {
            return true;
        }
    }

    return false;
}

// PE 里没有 WOW64（System32 缺 wow64.dll / wow64cpu.dll / wow64win.dll，
// SysWOW64 也缺 kernel32.dll），32 位程序一启动必然失败，所以只挑 64 位 exe。
bool IsX64Executable(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    bool isX64 = false;
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (ReadFile(file, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
    {
        DWORD signature = 0;
        IMAGE_FILE_HEADER header{};
        if (ReadFile(file, &signature, sizeof(signature), &read, nullptr) && read == sizeof(signature) &&
            signature == IMAGE_NT_SIGNATURE &&
            ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header))
        {
            isX64 = header.Machine == IMAGE_FILE_MACHINE_AMD64;
        }
    }

    CloseHandle(file);
    return isX64;
}

// 去掉末尾的 .exe，便于和 exeHint 精确比较
std::wstring StripExeSuffix(std::wstring name)
{
    const wchar_t suffix[] = L".exe";
    const size_t suffixLength = ARRAYSIZE(suffix) - 1;
    if (name.size() > suffixLength &&
        _wcsicmp(name.c_str() + name.size() - suffixLength, suffix) == 0)
    {
        name.resize(name.size() - suffixLength);
    }

    return name;
}

// 在目录中挑出要启动的 exe：优先与 exeHint 同名，其次包含关键字，再退到与目录同名，最后取第一个。
// 只接受 64 位 exe；目录里没有 64 位程序时返回空，调用方自然跳过该工具（而不是给个点了必报错的按钮）。
std::wstring PickAppExe(const std::wstring& directory, const wchar_t* exeHint)
{
    std::wstring firstExe;
    std::wstring containedExe;
    std::wstring namedExe;
    const std::wstring hint = exeHint != nullptr ? StripExeSuffix(ToLower(exeHint)) : std::wstring();
    const std::wstring folderName = directory.substr(directory.find_last_of(L"\\") + 1);

    WIN32_FIND_DATAW find{};
    HANDLE handle = FindFirstFileW((directory + L"\\*.exe").c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return std::wstring();
    }

    do
    {
        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }

        const std::wstring fileName = find.cFileName;
        const std::wstring lowered = ToLower(fileName);
        const std::wstring fullPath = directory + L"\\" + fileName;
        if (!IsX64Executable(fullPath))
        {
            continue;
        }

        if (firstExe.empty())
        {
            firstExe = fullPath;
        }

        if (!hint.empty())
        {
            // 主程序通常与关键字同名（chrome.exe / procexp64.exe），同名优先，避免误选
            // chrome_proxy.exe、autoruns64a.exe 这类附属程序
            if (StripExeSuffix(lowered) == hint)
            {
                FindClose(handle);
                return fullPath;
            }

            if (containedExe.empty() && lowered.find(hint) != std::wstring::npos)
            {
                containedExe = fullPath;
            }
        }

        if (namedExe.empty() && _wcsicmp(fileName.c_str(), folderName.c_str()) == 0)
        {
            namedExe = fullPath;
        }
    } while (FindNextFileW(handle, &find));

    FindClose(handle);

    if (!containedExe.empty())
    {
        return containedExe;
    }

    if (!namedExe.empty())
    {
        return namedExe;
    }

    return firstExe;
}

void ScanAppTools()
{
    g_launcher.appTools.clear();
    const std::wstring appsRoot = JoinPath(g_launcher.toolsDir, L"Apps");

    // 已知工具按固定顺序排在前面
    for (const KnownApp& known : kKnownApps)
    {
        const std::wstring directory = JoinPath(appsRoot, known.folder);
        const std::wstring exe = PickAppExe(directory, known.exeHint);
        if (!exe.empty())
        {
            g_launcher.appTools.push_back({ exe, known.display, known.category, known.hint });
        }
    }

    // 其它目录：取第一个 exe，按钮文字用目录名
    WIN32_FIND_DATAW find{};
    HANDLE handle = FindFirstFileW((appsRoot + L"\\*").c_str(), &find);
    if (handle != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                find.cFileName == std::wstring(L".") || find.cFileName == std::wstring(L".."))
            {
                continue;
            }

            bool alreadyKnown = false;
            for (const KnownApp& known : kKnownApps)
            {
                if (_wcsicmp(find.cFileName, known.folder) == 0)
                {
                    alreadyKnown = true;
                    break;
                }
            }

            if (alreadyKnown || HasStaticButtonFor(find.cFileName))
            {
                continue;
            }

            const std::wstring directory = JoinPath(appsRoot, find.cFileName);
            const std::wstring exe = PickAppExe(directory, nullptr);
            if (!exe.empty())
            {
                g_launcher.appTools.push_back({ exe, find.cFileName, L"其它工具", L"" });
            }
        } while (FindNextFileW(handle, &find));

        FindClose(handle);
    }
}

// 按钮按列排布：工具越多列数越多，避免在 768p 屏幕上一屏放不下
// 返回窗口客户区高度，同时写回 g_launcher.columns / clientWidth
int ComputeLauncherClientHeight(bool hasConfig, size_t appCount)
{
    const size_t buttonCount = (hasConfig ? 1 : 0) + 14 + appCount;   // install + 静态工具 + 重启/关机
    const int rowPitch = UiScale(52);
    // 标题/说明 + 状态行 + 进度条 + 底部留白，再加自绘标题栏
    const int chrome = kLauncherTitleBar + UiScale(88) + UiScale(8) + UiScale(24) + UiScale(8) +
                       kLauncherProgressHeight + UiScale(16);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int available = (workArea.bottom - workArea.top) - UiScale(56);      // 减去窗口边距

    for (int columns = 2; columns <= 4; ++columns)
    {
        const int rows = static_cast<int>((buttonCount + columns - 1) / columns);
        const int height = chrome + rows * rowPitch;
        const int width = UiScale(columns == 2 ? 520 : (columns == 3 ? 760 : 960));
        if (height <= available)
        {
            g_launcher.columns = columns;
            g_launcher.clientWidth = width;
            return height;
        }
    }

    // 极端情况（工具多到 4 列也放不下）：4 列 + 撑满可用高度，超出部分靠窗口滚动不可达，
    // 但正常工具集不会走到这里
    g_launcher.columns = 4;
    g_launcher.clientWidth = UiScale(960);
    return available;
}

// 后台线程：与 /autodeploy 走完全相同的 LoadConfig → Deploy 流程
void DeployWorker()
{
    std::wstring error;
    DeployConfig config;
    bool loaded = false;

    if (g_launcher.manualConfig)
    {
        config = *g_launcher.manualConfig;
        loaded = true;
    }
    else
    {
        loaded = LoadConfig(g_launcher.configPath, config, error);
        if (!loaded)
        {
            const std::wstring message = L"deploy.json 读取失败：" + error;
            MessageBoxW(g_launcher.window, message.c_str(), kLauncherTitle, MB_OK | MB_ICONERROR);
        }
    }

    if (loaded)
    {
        std::wstring logPath = config.logPath.empty() ? JoinPath(g_launcher.toolsDir, L"deploy.log")
                                                      : config.logPath;

        Logger log;
        if (!log.Open(logPath))
        {
            logPath = JoinPath(g_launcher.toolsDir, L"deploy.log");
            log.Open(logPath);
        }

        log.Info(L"==== FireWin PE 启动 ====");
        if (g_launcher.manualConfig)
        {
            log.Info(L"来源：启动器「安装系统」手动选择");
        }
        else
        {
            log.Info(L"配置文件：" + g_launcher.configPath);
        }

        log.Info(L"工具目录：" + g_launcher.toolsDir);

        const DeploymentResult result = Deploy(
            log, config, g_launcher.toolsDir,
            [](const std::wstring& text, int percent)
            {
                {
                    std::lock_guard<std::mutex> lock(g_launcher.progressMutex);
                    g_launcher.progressText = text;
                    g_launcher.progressPercent = percent;
                }

                if (g_launcher.window != nullptr)
                {
                    PostMessageW(g_launcher.window, kMessageDeployProgress, static_cast<WPARAM>(percent), 0);
                }
            });
        log.Info(result.success ? L"==== 部署成功 ====" : L"==== 部署失败 ====");
        ShowResult(result);
    }

    // 结果交回 UI 线程，由 UI 线程恢复按钮状态
    PostMessageW(g_launcher.window, kMessageDeployFinished, static_cast<WPARAM>(loaded ? 1 : 0), 0);
}

void StartDeployment()
{
    if (g_launcher.deploying || g_launcher.configPath.empty())
    {
        return;
    }

    const int answer = MessageBoxW(g_launcher.window,
                                   L"即将开始部署 Windows。\r\n\r\n目标分区上的现有内容会被覆盖，是否继续？",
                                   kLauncherTitle,
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES)
    {
        return;
    }

    g_launcher.deploying = true;
    if (g_launcher.deployButton != nullptr)
    {
        EnableWindow(g_launcher.deployButton, FALSE);
    }

    if (g_launcher.statusText != nullptr)
    {
        SetWindowTextW(g_launcher.statusText, L"正在准备部署…");
    }

    {
        std::lock_guard<std::mutex> lock(g_launcher.progressMutex);
        g_launcher.progressText = L"正在准备部署…";
        g_launcher.progressPercent = 0;
    }

    InvalidateRect(g_launcher.window, nullptr, TRUE);

    g_launcher.deployThread = std::thread(DeployWorker);
}

// 「设置分辨率」小工具（实现在显示模式相关函数之后，这里先声明）
void ShowDisplayModeDialog(HWND owner);

void HandleLauncherCommand(int command)
{
    const std::wstring appsDir = JoinPath(g_launcher.toolsDir, L"Apps");

    switch (static_cast<LauncherCommand>(command))
    {
    case kCommandDeploy:
        StartDeployment();
        break;
    case kCommandInstall:
        RunInstallWizard();
        break;
    case kCommandExplorer:
    {
        // 资源管理器：优先内置 Explorer++，没有时退回系统 explorer.exe
        const std::wstring directory = JoinPath(appsDir, L"Explorer++");
        const std::wstring explorerExe = JoinPath(directory, L"Explorer++.exe");
        const std::wstring systemExplorer = JoinPath(GetSystemDirectoryPath(), L"explorer.exe");
        if (FileExists(explorerExe))
        {
            ApplyExplorerLanguage(directory);
            LaunchTool(explorerExe, std::wstring(), false, directory);
        }
        else if (FileExists(systemExplorer))
        {
            LaunchTool(systemExplorer, std::wstring(), false, std::wstring());
        }
        else
        {
            MessageBoxW(g_launcher.window,
                        L"未找到资源管理器。\r\n请确认 PE 的 Tools\\Apps\\Explorer++ 目录完整。",
                        kLauncherTitle, MB_OK | MB_ICONWARNING);
        }
        break;
    }
    case kCommandSevenZip:
    {
        const std::wstring directory = JoinPath(appsDir, L"7-Zip");
        LaunchTool(JoinPath(directory, L"7zFM.exe"), std::wstring(), false, directory);
        break;
    }
    case kCommandNotepadPlusPlus:
    {
        const std::wstring directory = JoinPath(appsDir, L"Notepad++");
        LaunchTool(JoinPath(directory, L"notepad++.exe"), std::wstring(), false, directory);
        break;
    }
    case kCommandNotepad:
        LaunchSystemTool(L"notepad.exe", false, std::wstring());
        break;
    case kCommandRegedit:
        LaunchSystemTool(L"regedit.exe", false, std::wstring());
        break;
    case kCommandCommandPrompt:
        LaunchSystemTool(L"cmd.exe", true, L"X:\\");
        break;
    case kCommandDiskpart:
        LaunchSystemTool(L"diskpart.exe", true, std::wstring());
        break;
    case kCommandBootTool:
        LaunchTool(JoinPath(g_launcher.toolsDir, L"FireBoot.exe"), std::wstring(), false, std::wstring());
        break;
    case kCommandLoadDriver:
        StartDriverLoader();
        break;
    case kCommandInitNetwork:
        if (FileExists(JoinPath(GetSystemDirectoryPath(), L"wpeutil.exe")))
        {
            LaunchTool(JoinPath(GetSystemDirectoryPath(), L"wpeutil.exe"),
                       L"InitializeNetwork", true, std::wstring());
        }
        else
        {
            MessageBoxW(g_launcher.window,
                        L"当前 PE 缺少 wpeutil.exe，无法初始化网络。",
                        kLauncherTitle, MB_OK | MB_ICONERROR);
        }
        break;
    case kCommandDisplayMode:
        ShowDisplayModeDialog(g_launcher.window);
        break;
    case kCommandReboot:
        PerformPowerAction(true);
        break;
    case kCommandShutdown:
        PerformPowerAction(false);
        break;
    default:
        if (command >= kCommandAppFirst &&
            command < kCommandAppFirst + static_cast<int>(g_launcher.appTools.size()))
        {
            // 工具都放在各自目录里（Dism++ / Q-Dir / SystemInformer 依赖同目录资源），
            // 因此工作目录设为 exe 所在目录；同时给控制台类工具（TestDisk / PhotoRec）建新控制台
            const std::wstring exePath = g_launcher.appTools[static_cast<size_t>(command - kCommandAppFirst)].exe;
            const size_t slash = exePath.find_last_of(L"\\/");
            const std::wstring workingDirectory = slash == std::wstring::npos ? std::wstring() : exePath.substr(0, slash);
            LaunchTool(exePath, std::wstring(), true, workingDirectory);
        }
        break;
    }
}

void CreateLauncherControls(HWND window)
{
    g_launcher.font = CreateLauncherFont();

    const bool hasConfig = !g_launcher.configPath.empty();
    const std::wstring hint = hasConfig
                                  ? L"可以先用下面的工具检查磁盘、浏览文件，准备好后再点『开始安装』。"
                                  : L"未找到部署配置 deploy.json。点『安装系统』可扫描 U 盘 \\FWIMG（兼容 \\Images）与本机分区直接安装 Windows，或先使用工具。";

    HWND hintText = CreateWindowExW(0, L"STATIC", hint.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    16, kLauncherTitleBar + 16, g_launcher.clientWidth - 32, 56,
                                    window, nullptr, GetModuleHandleW(nullptr), nullptr);
    ApplyLauncherFont(hintText);

    struct ToolButton
    {
        LauncherCommand command;
        const wchar_t* text;
    };

    std::vector<ToolButton> buttons;
    if (hasConfig)
    {
        buttons.push_back({kCommandDeploy, L"开始安装"});
    }
    buttons.push_back({kCommandInstall, L"安装系统…"});
    buttons.push_back({kCommandExplorer, L"文件管理器"});
    buttons.push_back({kCommandSevenZip, L"7-Zip"});
    buttons.push_back({kCommandNotepadPlusPlus, L"Notepad++"});
    buttons.push_back({kCommandNotepad, L"记事本"});
    buttons.push_back({kCommandRegedit, L"注册表编辑器"});
    buttons.push_back({kCommandCommandPrompt, L"命令提示符"});
    buttons.push_back({kCommandDiskpart, L"磁盘管理"});
    buttons.push_back({kCommandBootTool, L"引导工具"});
    buttons.push_back({kCommandLoadDriver, L"加载驱动…"});
    buttons.push_back({kCommandDisplayMode, L"设置分辨率…"});
    buttons.push_back({kCommandInitNetwork, L"网络初始化"});
    // \Tools\Apps 里扫描到的第三方工具（DiskGenius / WinNTSetup / Dism++ 等，存在才显示）
    for (size_t appIndex = 0; appIndex < g_launcher.appTools.size(); ++appIndex)
    {
        buttons.push_back({static_cast<LauncherCommand>(kCommandAppFirst + static_cast<int>(appIndex)),
                           g_launcher.appTools[appIndex].title.c_str()});
    }
    buttons.push_back({kCommandReboot, L"重启"});
    buttons.push_back({kCommandShutdown, L"关机"});

    const int margin = UiScale(16);
    const int gap = UiScale(12);
    const int columns = g_launcher.columns;
    const int buttonWidth = (g_launcher.clientWidth - margin * 2 - gap * (columns - 1)) / columns;
    const int buttonHeight = UiScale(40);
    const int rowPitch = buttonHeight + gap;
    const int firstRowTop = kLauncherTitleBar + UiScale(88);

    for (size_t index = 0; index < buttons.size(); ++index)
    {
        const int column = static_cast<int>(index % static_cast<size_t>(columns));
        const int row = static_cast<int>(index / static_cast<size_t>(columns));
        // BS_OWNERDRAW：按钮由 LauncherProc 的 WM_DRAWITEM 画成 Win11 扁平圆角样式
        HWND button = CreateWindowExW(0, L"BUTTON", buttons[index].text,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      margin + column * (buttonWidth + gap),
                                      firstRowTop + row * rowPitch,
                                      buttonWidth, buttonHeight,
                                      window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(buttons[index].command)),
                                      GetModuleHandleW(nullptr), nullptr);
        ApplyLauncherFont(button);

        if (buttons[index].command == kCommandDeploy)
        {
            g_launcher.deployButton = button;
        }

        if (buttons[index].command == kCommandInstall)
        {
            g_launcher.installButton = button;
        }
    }

    // 状态行位置随按钮行数变化（有部署按钮时多一行）
    const int rowCount = static_cast<int>((buttons.size() + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
    const int statusTop = firstRowTop + rowCount * rowPitch + UiScale(8);
    g_launcher.statusText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            margin, statusTop, g_launcher.clientWidth - margin * 2, UiScale(24),
                                            window, nullptr, GetModuleHandleW(nullptr), nullptr);
    ApplyLauncherFont(g_launcher.statusText);
}

// 整窗背景：背景图等比缩放铺满客户区（居中裁剪），再以 92% 不透明度贴合，
// 这样既能看清火焰底图，按钮和状态文字也不会被亮部吃掉对比度。
void PaintLauncherBackground(HWND window, HDC dc)
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    BITMAP bitmap{};
    if (g_launcher.background == nullptr ||
        GetObjectW(g_launcher.background, sizeof(bitmap), &bitmap) == 0 ||
        bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0)
    {
        return;
    }

    HDC source = CreateCompatibleDC(dc);
    if (source == nullptr)
    {
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(source, g_launcher.background);

    const double scale = std::max(static_cast<double>(width) / bitmap.bmWidth,
                                  static_cast<double>(height) / bitmap.bmHeight);
    const int sourceWidth = static_cast<int>(width / scale);
    const int sourceHeight = static_cast<int>(height / scale);
    const int sourceX = (bitmap.bmWidth - sourceWidth) / 2;
    const int sourceY = (bitmap.bmHeight - sourceHeight) / 2;

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 235;
    AlphaBlend(dc, 0, 0, width, height, source, sourceX, sourceY, sourceWidth, sourceHeight, blend);

    SelectObject(source, oldBitmap);
    DeleteDC(source);
}

// ---------------------------------------------------------------------------
// Win11 风格自绘窗口：标题栏 + 扁平圆角按钮
// ---------------------------------------------------------------------------
RECT LauncherTitleButtonRect(HWND window, int which)
{
    RECT client{};
    GetClientRect(window, &client);

    RECT rect{};
    rect.top = 0;
    rect.bottom = kLauncherTitleBar;
    if (which == 2)
    {
        rect.right = client.right;
        rect.left = rect.right - kLauncherTitleButton;
    }
    else
    {
        rect.right = client.right - kLauncherTitleButton;
        rect.left = rect.right - kLauncherTitleButton;
    }

    return rect;
}

// 返回命中的标题栏按钮：0 无 / 1 最小化 / 2 关闭
int LauncherHitTitleButton(HWND window, int x, int y)
{
    for (int which = 1; which <= 2; ++which)
    {
        const RECT rect = LauncherTitleButtonRect(window, which);
        if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
        {
            return which;
        }
    }

    return 0;
}

void InvalidateLauncherTitleBar(HWND window)
{
    RECT bar{};
    GetClientRect(window, &bar);
    bar.bottom = kLauncherTitleBar;
    InvalidateRect(window, &bar, FALSE);
}

void PaintLauncherTitleBar(HWND window, HDC dc)
{
    RECT client{};
    GetClientRect(window, &client);
    if (client.right <= 0)
    {
        return;
    }

    RECT bar{0, 0, client.right, kLauncherTitleBar};
    HBRUSH chrome = CreateSolidBrush(kLauncherChrome);
    FillRect(dc, &bar, chrome);
    DeleteObject(chrome);

    // 右上角最小化 / 关闭：悬停填充，关闭按钮用 Win11 的红
    for (int which = 1; which <= 2; ++which)
    {
        const RECT rect = LauncherTitleButtonRect(window, which);
        if (g_launcher.titleHover == which)
        {
            COLORREF hot = which == 2 ? kLauncherCloseHover
                                      : (g_launcher.titlePressed == which ? RGB(84, 84, 84) : kLauncherHover);
            HBRUSH brush = CreateSolidBrush(hot);
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
        }

        const int centerX = (rect.left + rect.right) / 2;
        const int centerY = (rect.top + rect.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, kLauncherText);
        HGDIOBJ previousPen = SelectObject(dc, pen);
        if (which == 1)
        {
            MoveToEx(dc, centerX - 5, centerY, nullptr);
            LineTo(dc, centerX + 6, centerY);
        }
        else
        {
            MoveToEx(dc, centerX - 5, centerY - 5, nullptr);
            LineTo(dc, centerX + 6, centerY + 6);
            MoveToEx(dc, centerX + 5, centerY - 5, nullptr);
            LineTo(dc, centerX - 6, centerY + 6);
        }
        SelectObject(dc, previousPen);
        DeleteObject(pen);
    }

    int textLeft = UiScale(16);
    if (g_launcher.icon != nullptr)
    {
        const int iconSize = UiScale(20);
        DrawIconEx(dc, UiScale(14), (kLauncherTitleBar - iconSize) / 2, g_launcher.icon,
                   iconSize, iconSize, 0, nullptr, DI_NORMAL);
        textLeft = UiScale(42);
    }

    RECT title{textLeft, 0, client.right - kLauncherTitleButton * 2 - 8, kLauncherTitleBar};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kLauncherText);

    HGDIOBJ fontObject = g_launcher.font;
    if (fontObject == nullptr)
    {
        fontObject = GetStockObject(DEFAULT_GUI_FONT);
    }
    HGDIOBJ previousFont = SelectObject(dc, fontObject);
    DrawTextW(dc, L"FireWin PE 部署工具", -1, &title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, previousFont);

    RECT line{0, kLauncherTitleBar - 1, client.right, kLauncherTitleBar};
    HBRUSH border = CreateSolidBrush(kLauncherBorder);
    FillRect(dc, &line, border);
    DeleteObject(border);
}

// 启动器按钮自绘：圆角卡片 / 主按钮用 Win11 强调色
void PaintLauncherButton(const DRAWITEMSTRUCT& item)
{
    HDC dc = item.hDC;
    RECT rect = item.rcItem;
    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return;
    }

    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = item.CtlID == static_cast<UINT>(kCommandDeploy);

    COLORREF fill = primary ? kLauncherAccentButton : kLauncherCard;
    if (disabled)
    {
        fill = RGB(48, 48, 48);
    }
    else if (pressed)
    {
        fill = primary ? RGB(0, 90, 158) : kLauncherHover;
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? fill : kLauncherBorder);
    HGDIOBJ previousBrush = SelectObject(dc, brush);
    HGDIOBJ previousPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right - 1, rect.bottom - 1, 10, 10);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, primary ? RGB(255, 255, 255) : (disabled ? kLauncherMuted : kLauncherText));

    HGDIOBJ fontObject = g_launcher.font;
    if (fontObject == nullptr)
    {
        fontObject = GetStockObject(DEFAULT_GUI_FONT);
    }
    HGDIOBJ previousFont = SelectObject(dc, fontObject);
    DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, previousFont);
}

// 底部进度条的矩形：宽度跟随客户区，贴在窗口底部留白处
RECT LauncherProgressRect(HWND window)
{
    RECT client{};
    GetClientRect(window, &client);

    RECT rect{};
    rect.left = UiScale(16);
    rect.right = client.right - UiScale(16);
    rect.bottom = client.bottom - UiScale(16);
    rect.top = rect.bottom - kLauncherProgressHeight;
    return rect;
}

// Windows 11 风格进度条：圆角轨道 + 强调色填充 + 内嵌高光。
// 启动器是纯自绘窗口（不依赖 comctl32 的 PROGRESS_CLASS），所以这里直接画。
void PaintLauncherProgress(HWND window, HDC dc)
{
    int percent = 0;
    {
        std::lock_guard<std::mutex> lock(g_launcher.progressMutex);
        percent = g_launcher.progressPercent;
    }

    if (!g_launcher.deploying && percent <= 0)
    {
        return;
    }

    RECT bar = LauncherProgressRect(window);
    if (bar.right - bar.left < 40)
    {
        return;
    }

    HBRUSH track = CreateSolidBrush(RGB(40, 40, 40));
    HPEN border = CreatePen(PS_SOLID, 1, kLauncherBorder);
    HGDIOBJ previousBrush = SelectObject(dc, track);
    HGDIOBJ previousPen = SelectObject(dc, border);
    RoundRect(dc, bar.left, bar.top, bar.right, bar.bottom,
              kLauncherProgressHeight, kLauncherProgressHeight);

    if (percent > 0)
    {
        RECT fill = bar;
        InflateRect(&fill, -2, -2);
        const int trackWidth = fill.right - fill.left;
        fill.right = fill.left + trackWidth * std::clamp(percent, 0, 100) / 100;
        if (fill.right > fill.left)
        {
            HBRUSH accent = CreateSolidBrush(kLauncherAccent);
            HPEN accentPen = CreatePen(PS_SOLID, 1, kLauncherAccent);
            SelectObject(dc, accent);
            SelectObject(dc, accentPen);

            const int radius = std::min<int>(kLauncherProgressHeight - 4, fill.right - fill.left);
            RoundRect(dc, fill.left, fill.top, fill.right, fill.bottom, radius, radius);

            RECT highlight{fill.left + radius / 2, fill.top + 2, fill.right - radius / 2, fill.top + 6};
            if (highlight.right > highlight.left)
            {
                HBRUSH light = CreateSolidBrush(RGB(126, 216, 255));
                FillRect(dc, &highlight, light);
                DeleteObject(light);
            }

            SelectObject(dc, previousBrush);
            SelectObject(dc, previousPen);
            DeleteObject(accentPen);
            DeleteObject(accent);
        }
    }

    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(track);
    DeleteObject(border);
}

LRESULT CALLBACK LauncherProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (message)
    {
    case WM_CREATE:
        g_launcher.background = static_cast<HBITMAP>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDB_BACKGROUND),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        {
            // 标题栏 / 任务栏图标（res\app.ico 编入程序资源）
            HICON icon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                0, 0, LR_DEFAULTSIZE | LR_SHARED));
            if (icon != nullptr)
            {
                SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
                SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            }

            // 自绘标题栏左侧用 20px 小图标（LR_SHARED，随模块释放）
            g_launcher.icon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                20, 20, LR_SHARED));
        }
        CreateLauncherControls(window);
        return 0;
    case WM_ERASEBKGND:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        PaintLauncherBackground(window, dc);
        PaintLauncherTitleBar(window, dc);
        PaintLauncherProgress(window, dc);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    {
        // 说明 / 状态文字直接压在背景图上：透明背景 + 浅色文字
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(232, 232, 232));
        return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
    }
    case WM_COMMAND:
        HandleLauncherCommand(static_cast<int>(LOWORD(wParam)));
        return 0;
    case WM_DRAWITEM:
    {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item != nullptr && item->CtlType == ODT_BUTTON)
        {
            PaintLauncherButton(*item);
            return TRUE;
        }
        break;
    }
    case WM_NCHITTEST:
    {
        // 无边框自绘窗口：标题栏区域当作可拖动区，右上角按钮留给 WM_LBUTTON*
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        if (point.y >= 0 && point.y < kLauncherTitleBar)
        {
            return LauncherHitTitleButton(window, point.x, point.y) != 0 ? HTCLIENT : HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_MOUSEMOVE:
    {
        const int hover = LauncherHitTitleButton(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hover != g_launcher.titleHover)
        {
            g_launcher.titleHover = hover;
            InvalidateLauncherTitleBar(window);
        }

        if (!g_launcher.titleTracking)
        {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            TrackMouseEvent(&track);
            g_launcher.titleTracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_launcher.titleTracking = false;
        if (g_launcher.titleHover != 0 || g_launcher.titlePressed != 0)
        {
            g_launcher.titleHover = 0;
            g_launcher.titlePressed = 0;
            InvalidateLauncherTitleBar(window);
        }
        return 0;
    case WM_LBUTTONDOWN:
    {
        const int hit = LauncherHitTitleButton(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hit != 0)
        {
            g_launcher.titlePressed = hit;
            SetCapture(window);
            InvalidateLauncherTitleBar(window);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
    {
        if (g_launcher.titlePressed != 0)
        {
            const int pressed = g_launcher.titlePressed;
            g_launcher.titlePressed = 0;
            ReleaseCapture();

            const int hit = LauncherHitTitleButton(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            InvalidateLauncherTitleBar(window);
            if (hit == pressed)
            {
                if (pressed == 1)
                {
                    ShowWindow(window, SW_MINIMIZE);
                }
                else
                {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                }
            }
            return 0;
        }
        break;
    }
    case kMessageDeployProgress:
        if (g_launcher.statusText != nullptr)
        {
            std::wstring text;
            {
                std::lock_guard<std::mutex> lock(g_launcher.progressMutex);
                text = g_launcher.progressText;
            }

            if (!text.empty())
            {
                SetWindowTextW(g_launcher.statusText, text.c_str());
            }
        }

        // 背景（含进度条）整块重绘；按钮是子窗口，WS_CLIPCHILDREN 会跳过
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case kMessageDeployFinished:
        g_launcher.deploying = false;
        if (g_launcher.deployThread.joinable())
        {
            g_launcher.deployThread.join();
        }

        g_launcher.manualConfig = nullptr;   // 手动部署参数用完即弃
        if (g_launcher.deployButton != nullptr)
        {
            EnableWindow(g_launcher.deployButton, TRUE);
        }

        if (g_launcher.installButton != nullptr)
        {
            EnableWindow(g_launcher.installButton, TRUE);
        }

        if (g_launcher.statusText != nullptr)
        {
            SetWindowTextW(g_launcher.statusText,
                           wParam != 0 ? L"部署完成。" : L"部署失败，详情见日志。");
        }

        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_CLOSE:
        // 部署过程中禁止关窗，避免后台线程在窗口销毁后继续使用界面句柄
        if (g_launcher.deploying)
        {
            MessageBoxW(window, L"正在部署，请等待完成后再关闭窗口。", kLauncherTitle,
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (g_launcher.font != nullptr)
        {
            DeleteObject(g_launcher.font);
            g_launcher.font = nullptr;
        }

        if (g_launcher.background != nullptr)
        {
            DeleteObject(g_launcher.background);
            g_launcher.background = nullptr;
        }

        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

int RunLauncher()
{
    g_launcher.configPath = LocateConfig(std::wstring());
    g_launcher.toolsDir = GetExeDirectory();
    ScanAppTools();
    g_launcher.clientHeight = ComputeLauncherClientHeight(!g_launcher.configPath.empty(), g_launcher.appTools.size());

    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = static_cast<UINT>(sizeof(windowClass));
    windowClass.lpfnWndProc = LauncherProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW) + 1);
    windowClass.lpszClassName = kLauncherClass;

    if (RegisterClassExW(&windowClass) == 0)
    {
        MessageBoxW(nullptr, L"启动器窗口类注册失败。", kLauncherTitle, MB_OK | MB_ICONERROR);
        return 4;
    }

    // Win11 风格：无系统边框 / 标题栏，标题栏与按钮全部自绘，整窗做圆角
    const DWORD style = WS_POPUP | WS_CLIPCHILDREN;
    const DWORD exStyle = WS_EX_APPWINDOW;

    const int width = g_launcher.clientWidth;
    const int height = g_launcher.clientHeight;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_launcher.window = CreateWindowExW(exStyle, kLauncherClass, kLauncherTitle, style,
                                        x, y, width, height,
                                        nullptr, nullptr, instance, nullptr);
    if (g_launcher.window == nullptr)
    {
        MessageBoxW(nullptr, L"启动器窗口创建失败。", kLauncherTitle, MB_OK | MB_ICONERROR);
        return 5;
    }

    // 圆角裁切：WinPE 没有 DWM，只能靠窗口区域自己切
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, kLauncherCorner, kLauncherCorner);
    if (region != nullptr)
    {
        SetWindowRgn(g_launcher.window, region, TRUE);   // 成功后区域归系统所有，不再删除
    }

    ShowWindow(g_launcher.window, SW_SHOW);
    UpdateWindow(g_launcher.window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // 兜底：窗口若在部署过程中被强制销毁，仍需回收后台线程
    if (g_launcher.deployThread.joinable())
    {
        g_launcher.deployThread.join();
    }

    return 0;
}

// ---------------------------------------------------------------------------
// 桌面模式：类 Windows 桌面（壁纸 + 图标 + 任务栏 + 开始菜单）
// 纯 Win32 绘制、零外部依赖。任务栏 / 开始菜单是独立置顶弹出窗，
// 这样第三方工具窗口最大化也不会盖住任务栏。
// ---------------------------------------------------------------------------
constexpr wchar_t kDesktopClass[] = L"FireWinPEDesktop";
constexpr wchar_t kTaskbarClass[] = L"FireWinPETaskbar";
constexpr wchar_t kStartMenuClass[] = L"FireWinPEStartMenu";

// 桌面 / 任务栏 / 开始菜单尺寸：96 DPI 基准值，运行时由 InitUiMetrics() 换算
int kTaskbarHeight = 46;
int kCellWidth = 98;
int kCellHeight = 94;
int kIconSize = 48;
int kDesktopMargin = 16;
int kStartButtonWidth = 88;
int kClockWidth = 108;
int kStartMenuWidth = 420;
int kStartMenuHeader = 54;
int kMenuSeparator = 9;
int kStartMenuPadding = 8;
int kStartMenuColumnGap = 12;
int kStartMenuColumnWidth = 196;
constexpr int kTaskbarTimerId = 1;
constexpr int kDesktopHitNone = -1;

// Win11 深色配色：中性灰底 + 天蓝强调色
constexpr COLORREF kUiBg = RGB(32, 32, 32);       // #202020
constexpr COLORREF kUiCard = RGB(43, 43, 43);     // #2B2B2B
constexpr COLORREF kUiHover = RGB(58, 58, 58);    // #3A3A3A
constexpr COLORREF kUiBorder = RGB(66, 66, 66);   // #424242
constexpr COLORREF kUiText = RGB(255, 255, 255);  // #FFFFFF
constexpr COLORREF kUiMuted = RGB(160, 160, 160); // #A0A0A0
constexpr COLORREF kUiAccent = RGB(76, 194, 255); // #4CC2FF
constexpr COLORREF kUiWarn = RGB(255, 169, 64);   // 警告橙
constexpr COLORREF kUiTile = RGB(60, 60, 60);     // 图标底板

enum DesktopCommand : int
{
    kDesktopRefresh = 2001,
    kDesktopOpenLauncher,
    kDesktopCloseWindow,
};

// 所有快捷方式统一绘制的矢量图标（GDI 图元，等效于「画 SVG」），不再从 exe 提取图标，
// 避免原生 exe 图标（尺寸 / 配色 / 风格各不相同）破坏整套图标的统一感。
enum class DesktopGlyph
{
    None,
    App,
    Install,
    Driver,
    Network,
    Disk,
    Refresh,
    Launcher,
    Power,
    Reboot,
    Folder,
    Document,
    Console,
    Archive,
    Chip,
    Search,
    Key,
    Disc,
    Display,
};

struct DesktopEntry
{
    std::wstring title;
    std::wstring exePath;    // 非空：直接启动该 exe
    int actionId = 0;        // exePath 为空：交给 HandleLauncherCommand / 桌面内置动作
    DesktopGlyph glyph = DesktopGlyph::None; // 统一绘制的矢量图标
    HICON icon = nullptr;    // 从 exe 里提取的程序图标，非空时优先于矢量图标
    bool isHeader = false;   // 开始菜单里的分组标题（不可点击、不参与键盘选择）
};

struct DesktopState
{
    HWND window = nullptr;
    HWND taskbar = nullptr;
    HWND startMenu = nullptr;
    HBITMAP background = nullptr;
    HICON appIcon = nullptr; // 程序图标（LR_SHARED，不销毁）
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HFONT smallFont = nullptr;
    HFONT clockFont = nullptr;
    std::vector<DesktopEntry> icons;
    std::vector<DesktopEntry> startItems;
    std::vector<HWND> taskWindows;
    int rows = 6;            // 桌面图标按 Windows 习惯竖向排列，先填满一列再换下一列
    int selected = kDesktopHitNone;
    int hover = kDesktopHitNone;
    int menuHover = kDesktopHitNone;
    int menuRowHeight = 34;
    int menuHeight = 0;
    int menuSplit = 0;       // 开始菜单两列的分界下标（左侧 [0, split)，右侧 [split, n)）
    int taskHover = kDesktopHitNone;
    bool menuOpen = false;
    bool startButtonHover = false;
};

DesktopState g_desktop;

void ShowStartMenu(bool show);

// ---------------------------------------------------------------------------
// 绘制小工具
// ---------------------------------------------------------------------------
void UiFillRect(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void UiRoundRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void UiText(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT flags)
{
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, oldFont);
}

// 逻辑坐标字体：向导使用 960x720 画布映射，传入的字号不能再按 g_uiScale 换算
HFONT CreateDesktopFont(int height, int weight, bool applyScale = true)
{
    static const wchar_t* const kFaces[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimSun", L"NSimSun"};

    LOGFONTW description{};
    description.lfHeight = applyScale ? UiScale(height) : height;
    description.lfWeight = weight;
    description.lfCharSet = DEFAULT_CHARSET;
    description.lfQuality = CLEARTYPE_QUALITY;

    for (const wchar_t* face : kFaces)
    {
        if (!IsFontInstalled(face))
        {
            continue;
        }

        lstrcpynW(description.lfFaceName, face, LF_FACESIZE);
        HFONT font = CreateFontIndirectW(&description);
        if (font != nullptr)
        {
            return font;
        }
    }

    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// ---------------------------------------------------------------------------
// 「安装系统」四步向导：镜像 → 版本 → 磁盘 → 摘要 / 进度
// 完全自绘（深色 Win11 风格），不使用标准控件，保证 PE 下视觉一致。
// ---------------------------------------------------------------------------
// 窗口类注册函数定义在桌面模块（本文件后半部分），向导模块先用到，这里前置声明。
void RegisterDesktopClass(const wchar_t* name, WNDPROC procedure, UINT style);

constexpr wchar_t kInstallWizardClass[] = L"FireWinPEInstallWizard";
// 向导内部坐标全部基于 960x720 的「逻辑画布」，绘制时整体做一次映射，
// 因此这里的常量保持原始值，只有窗口尺寸按 g_uiScale 放大。
constexpr int kWizardWidth = 960;
constexpr int kWizardHeight = 720;
constexpr int kWizardSidebarWidth = 260;
constexpr int kWizardContentLeft = 316;
constexpr int kWizardContentRight = 908;
constexpr UINT kMessageWizardProgress = WM_APP + 201;
constexpr UINT kMessageWizardDone = WM_APP + 202;

// 把上面所有 96 DPI 基准值换算成当前屏幕使用的实际像素。
// 分辨率可能在加载页后台被改动，所以进入桌面 / 启动器前会再次调用。
void InitUiMetrics()
{
    kLauncherTitleBar = UiScale(44);
    kLauncherCorner = UiScale(16);
    kLauncherTitleButton = UiScale(46);
    kLauncherProgressHeight = UiScale(18);

    kTaskbarHeight = UiScale(46);
    kCellWidth = UiScale(98);
    kCellHeight = UiScale(94);
    kIconSize = UiScale(48);
    kDesktopMargin = UiScale(16);
    kStartButtonWidth = UiScale(88);
    kClockWidth = UiScale(108);
    kStartMenuWidth = UiScale(420);
    kStartMenuHeader = UiScale(54);
    kMenuSeparator = UiScale(9);
    kStartMenuPadding = UiScale(8);
    kStartMenuColumnGap = UiScale(12);
    kStartMenuColumnWidth = (kStartMenuWidth - kStartMenuPadding * 2 - kStartMenuColumnGap) / 2;
}

enum WizardHit
{
    kWizardHitNone = 0,
    kWizardHitCancel,
    kWizardHitBack,
    kWizardHitNext,
    kWizardHitListFirst = 1000
};

struct InstallWizardState
{
    HWND window = nullptr;
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;

    int step = 0;               // 0 镜像 / 1 版本 / 2 磁盘 / 3 摘要
    int hover = kWizardHitNone;
    bool deploying = false;
    bool finished = false;
    bool resultSuccess = false;

    std::vector<std::wstring> images;
    int imageSelected = -1;
    std::wstring wimPath;
    std::wstring lastDirectory;

    int versionCount = 0;
    std::vector<std::wstring> versions;
    int versionSelected = 0;

    std::vector<VolumeEntry> volumes;
    int firmwareKind = 0;       // 1 BIOS / 2 UEFI / 0 未知
    int targetSelected = -1;
    int bootSelected = -1;

    std::mutex mutex;           // 保护进度文字
    std::wstring progressText = L"准备开始…";
    int progressPercent = 0;
    std::wstring resultMessage;

    std::thread worker;
};

RECT WizardNextRect()
{
    return RECT{796, 650, 908, 686};
}

RECT WizardBackRect()
{
    return RECT{672, 650, 784, 686};
}

RECT WizardCancelRect()
{
    return RECT{316, 650, 428, 686};
}

RECT WizardListRowRect(int index)
{
    // 镜像 / 版本列表：56 高 + 8 间距
    const int top = 128 + index * 64;
    return RECT{kWizardContentLeft, top, kWizardContentRight, top + 56};
}

RECT WizardTargetRowRect(int index)
{
    const int top = 168 + index * 48;
    return RECT{316, top, 600, top + 42};
}

RECT WizardBootRowRect(int index)
{
    const int top = 168 + index * 48;
    return RECT{624, top, 908, top + 42};
}

std::wstring FileNamePart(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DirectoryPart(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// 分区一行副标题：卷标 + 容量 + 可用空间
std::wstring VolumeSubtitle(const VolumeEntry& volume)
{
    std::wstring text;
    if (!volume.label.empty())
    {
        text = volume.label + L"  ·  ";
    }

    text += FormatBytesText(volume.totalBytes) + L"，可用 " + FormatBytesText(volume.freeBytes);
    if (volume.isEsp)
    {
        text += L"  ·  ESP";
    }

    return text;
}

bool WizardCanAdvance(const InstallWizardState& state)
{
    switch (state.step)
    {
    case 0:
        return state.imageSelected >= 0 && state.imageSelected < static_cast<int>(state.images.size());
    case 1:
        return state.versionSelected >= 0 && state.versionSelected < state.versionCount;
    case 2:
        return state.targetSelected >= 0 && state.bootSelected >= 0 && state.targetSelected != state.bootSelected &&
               state.targetSelected < static_cast<int>(state.volumes.size()) &&
               state.bootSelected < static_cast<int>(state.volumes.size());
    default:
        return true;
    }
}

void WizardSetStep(InstallWizardState& state, int step)
{
    if (step < 0 || step > 3)
    {
        return;
    }

    // 首次进入「版本」时解析镜像索引名称（wimlib info 可能要一两秒）
    if (step == 1 && state.versionCount <= 0 && !state.wimPath.empty())
    {
        state.versionCount = GetWimImageCount(state.wimPath, state.versions);
        while (static_cast<int>(state.versions.size()) < state.versionCount)
        {
            state.versions.push_back(L"镜像 " + std::to_wstring(state.versions.size() + 1));
        }

        if (state.versionSelected >= state.versionCount)
        {
            state.versionSelected = 0;
        }
    }

    state.step = step;
}

void WizardRunDeploy(InstallWizardState* state)
{
    Logger log;
    log.Open(JoinPath(GetExeDirectory(), L"FireWinPE-install.log"));

    DeployConfig config;
    config.wimPath = state->wimPath;
    config.wimIndex = state->versionSelected + 1;

    const VolumeEntry& target = state->volumes[static_cast<size_t>(state->targetSelected)];
    const VolumeEntry& boot = state->volumes[static_cast<size_t>(state->bootSelected)];
    config.targetDrive = target.drive;
    config.systemDrive = boot.drive;
    config.firmware = state->firmwareKind == 2 ? L"UEFI" : (state->firmwareKind == 1 ? L"BIOS" : L"UEFI");
    config.autoReboot = false;      // 由用户在向导里决定是否重启

    log.Info(L"向导开始部署：" + config.wimPath + L" 索引 " + std::to_wstring(config.wimIndex) +
             L" → " + config.targetDrive + L"，引导 " + config.systemDrive);

    const DeploymentResult result = Deploy(
        log, config, g_launcher.toolsDir, [state](const std::wstring& text, int percent) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->progressText = text;
                state->progressPercent = percent;
            }

            if (HWND window = state->window)
            {
                PostMessageW(window, kMessageWizardProgress, 0, 0);
            }
        });

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->resultSuccess = result.success;
        state->resultMessage = result.message;
        if (!result.success && !result.logPath.empty())
        {
            state->resultMessage += L"\r\n日志：" + result.logPath;
        }
    }

    state->finished = true;
    state->deploying = false;
    g_launcher.deploying = false;

    if (HWND window = state->window)
    {
        PostMessageW(window, kMessageWizardDone, 0, 0);
    }
}

void WizardStartDeploy(InstallWizardState& state)
{
    state.deploying = true;
    state.finished = false;
    state.resultSuccess = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.progressText = L"准备开始…";
        state.progressPercent = 0;
    }

    g_launcher.deploying = true;
    state.worker = std::thread(WizardRunDeploy, &state);
}

void PaintWizardButton(HDC dc, const RECT& rect, const std::wstring& text, HFONT font, bool primary,
                       bool enabled, bool hovered)
{
    COLORREF fill = primary ? (enabled ? kUiAccent : RGB(72, 72, 72))
                            : (hovered && enabled ? kUiHover : RGB(48, 48, 48));
    const COLORREF border = primary ? fill : kUiBorder;
    const COLORREF color = primary ? RGB(16, 16, 16) : (enabled ? kUiText : RGB(120, 120, 120));

    UiRoundRect(dc, rect, 6, fill, border);
    RECT textRect = rect;
    UiText(dc, text, textRect, font, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void PaintWizardRow(HDC dc, HFONT titleFont, HFONT smallFont, const RECT& row, const std::wstring& title,
                    const std::wstring& subtitle, bool selected, bool hovered)
{
    const COLORREF fill = selected ? RGB(30, 58, 74) : (hovered ? kUiHover : RGB(44, 44, 44));
    const COLORREF border = selected ? kUiAccent : kUiBorder;
    UiRoundRect(dc, row, 8, fill, border);

    const int height = row.bottom - row.top;
    RECT titleRect{row.left + 14, row.top + 5, row.right - 34, row.top + height / 2 + 4};
    UiText(dc, title, titleRect, titleFont, kUiText,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT subRect{row.left + 14, row.top + height / 2 + 1, row.right - 14, row.bottom - 5};
    UiText(dc, subtitle, subRect, smallFont, selected ? RGB(190, 226, 246) : kUiMuted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (selected)
    {
        RECT dot{row.right - 24, row.top + height / 2 - 4, row.right - 16, row.top + height / 2 + 4};
        UiRoundRect(dc, dot, 4, kUiAccent, kUiAccent);
    }
}

void PaintWizardHeader(HDC dc, const InstallWizardState& state, const std::wstring& title,
                       const std::wstring& subtitle, COLORREF subtitleColor)
{
    RECT titleRect{kWizardContentLeft, 44, kWizardContentRight, 84};
    UiText(dc, title, titleRect, state.titleFont, kUiText,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT subRect{kWizardContentLeft, 82, kWizardContentRight, 110};
    UiText(dc, subtitle, subRect, state.smallFont, subtitleColor,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void PaintWizardSidebar(HDC dc, const InstallWizardState& state)
{
    RECT sidebar{0, 0, kWizardSidebarWidth, kWizardHeight};
    UiFillRect(dc, sidebar, RGB(24, 24, 24));

    RECT divider{kWizardSidebarWidth - 1, 0, kWizardSidebarWidth, kWizardHeight};
    UiFillRect(dc, divider, kUiBorder);

    if (g_desktop.appIcon != nullptr)
    {
        DrawIconEx(dc, 28, 30, g_desktop.appIcon, 32, 32, 0, nullptr, DI_NORMAL);
    }

    RECT brand{72, 24, 244, 54};
    UiText(dc, L"FireWin PE", brand, state.bodyFont, kUiText,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT brandSub{72, 52, 244, 76};
    UiText(dc, L"Windows 安装向导", brandSub, state.smallFont, kUiMuted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const wchar_t* const titles[] = {L"选择镜像", L"选择版本", L"选择磁盘", L"确认并安装"};
    const wchar_t* const captions[] = {L"选取 install.wim / install.esd", L"选择要安装的系统版本",
                                       L"指定目标分区与引导分区", L"核对摘要并开始部署"};

    for (int i = 0; i < 4; ++i)
    {
        const int top = 150 + i * 72;
        const bool active = state.step == i && !state.finished;
        const bool passed = state.step > i || state.finished;

        if (i < 3)
        {
            RECT line{43, top + 32, 45, top + 72};
            UiFillRect(dc, line, kUiBorder);
        }

        RECT circle{33, top + 8, 55, top + 30};
        if (active)
        {
            UiRoundRect(dc, circle, 11, kUiAccent, kUiAccent);
        }
        else if (passed)
        {
            UiRoundRect(dc, circle, 11, RGB(48, 96, 118), RGB(48, 96, 118));
        }
        else
        {
            UiRoundRect(dc, circle, 11, RGB(30, 30, 30), kUiBorder);
        }

        RECT number = circle;
        UiText(dc, std::to_wstring(i + 1), number, state.smallFont,
               active ? RGB(16, 16, 16) : (passed ? kUiText : kUiMuted),
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT titleRect{66, top + 2, 248, top + 28};
        UiText(dc, titles[i], titleRect, state.bodyFont,
               (active || passed) ? kUiText : kUiMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT captionRect{66, top + 26, 248, top + 50};
        UiText(dc, captions[i], captionRect, state.smallFont, kUiMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void PaintWizardStepImages(HDC dc, const InstallWizardState& state)
{
    const bool noImages = state.images.empty();
    const std::wstring imagesHint = noImages ? std::wstring(L"没有自动找到镜像，请点击下方「浏览其它镜像…」")
                                             : std::wstring(L"自动扫描各分区 \\FWIMG 与 \\Images 目录");
    PaintWizardHeader(dc, state, L"选择系统镜像", imagesHint, noImages ? kUiWarn : kUiMuted);

    for (size_t i = 0; i < state.images.size() && i < 7; ++i)
    {
        const RECT row = WizardListRowRect(static_cast<int>(i));
        PaintWizardRow(dc, state.bodyFont, state.smallFont, row, FileNamePart(state.images[i]),
                       DirectoryPart(state.images[i]), state.imageSelected == static_cast<int>(i),
                       state.hover == kWizardHitListFirst + static_cast<int>(i));
    }

    // 「浏览其它镜像…」放在列表末尾
    const int browseIndex = static_cast<int>(state.images.size());
    RECT browseRow = WizardListRowRect(browseIndex);
    browseRow.bottom = browseRow.top + 46;
    const bool hovered = state.hover == kWizardHitListFirst + browseIndex;
    UiRoundRect(dc, browseRow, 8, hovered ? kUiHover : RGB(44, 44, 44), kUiBorder);
    RECT browseText = browseRow;
    UiText(dc, L"浏览其它镜像…", browseText, state.bodyFont, kUiAccent,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void PaintWizardStepVersions(HDC dc, const InstallWizardState& state)
{
    PaintWizardHeader(dc, state, L"选择系统版本", FileNamePart(state.wimPath) + L"  ·  共 " +
                                                           std::to_wstring(state.versionCount) + L" 个版本",
                      kUiMuted);

    for (int i = 0; i < state.versionCount && i < 7; ++i)
    {
        const RECT row = WizardListRowRect(i);
        const std::wstring name = i < static_cast<int>(state.versions.size()) ? state.versions[i]
                                                                             : L"镜像 " + std::to_wstring(i + 1);
        PaintWizardRow(dc, state.bodyFont, state.smallFont, row, name,
                       L"索引 " + std::to_wstring(i + 1), state.versionSelected == i,
                       state.hover == kWizardHitListFirst + i);
    }
}

void PaintWizardStepDisks(HDC dc, const InstallWizardState& state)
{
    const wchar_t* firmware = state.firmwareKind == 1 ? L"BIOS"
                              : state.firmwareKind == 2 ? L"UEFI"
                                                        : L"未知";
    PaintWizardHeader(dc, state, L"选择磁盘分区",
                      L"注意：目标分区上的所有数据都会被清除，请先备份重要资料", kUiWarn);

    RECT targetLabel{316, 126, 600, 150};
    UiText(dc, L"安装到（将被格式化）", targetLabel, state.smallFont, kUiMuted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT bootLabel{624, 126, 908, 150};
    UiText(dc, std::wstring(L"引导分区（当前固件：") + firmware + L"）", bootLabel, state.smallFont, kUiMuted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    for (size_t i = 0; i < state.volumes.size() && i < 8; ++i)
    {
        const VolumeEntry& volume = state.volumes[i];
        const std::wstring title = volume.drive + L"  " + (volume.label.empty() ? L"本地磁盘" : volume.label) +
                                   (volume.isEsp ? L"（ESP）" : L"");

        PaintWizardRow(dc, state.bodyFont, state.smallFont, WizardTargetRowRect(static_cast<int>(i)), title,
                       VolumeSubtitle(volume), state.targetSelected == static_cast<int>(i),
                       state.hover == kWizardHitListFirst + static_cast<int>(i));

        PaintWizardRow(dc, state.bodyFont, state.smallFont, WizardBootRowRect(static_cast<int>(i)), title,
                       VolumeSubtitle(volume), state.bootSelected == static_cast<int>(i),
                       state.hover == kWizardHitListFirst + 1000 + static_cast<int>(i));
    }

    if (state.targetSelected >= 0 && state.targetSelected == state.bootSelected)
    {
        RECT hint{316, 586, 908, 612};
        UiText(dc, L"目标分区与引导分区不能相同", hint, state.smallFont, kUiWarn,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    else if (state.bootSelected < 0)
    {
        RECT hint{316, 586, 908, 612};
        UiText(dc, L"UEFI 请选择 FAT32 的 ESP 分区；BIOS 一般选择与目标相同的系统分区", hint, state.smallFont,
               kUiMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void PaintWizardStepSummary(HDC dc, InstallWizardState& state)
{
    std::wstring progress;
    int percent = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        progress = state.progressText;
        percent = state.progressPercent;
    }

    if (state.finished)
    {
        PaintWizardHeader(dc, state, state.resultSuccess ? L"安装完成" : L"安装失败",
                          state.resultSuccess ? L"系统已写入目标分区，可以从硬盘启动了" : L"请查看下方信息并重试",
                          state.resultSuccess ? kUiAccent : kUiWarn);

        RECT messageRect{kWizardContentLeft, 140, kWizardContentRight, 420};
        UiText(dc, state.resultMessage, messageRect, state.bodyFont,
               state.resultSuccess ? kUiText : RGB(255, 180, 180),
               DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        return;
    }

    PaintWizardHeader(dc, state, L"确认并安装", L"核对下面的信息，确认无误后开始部署", kUiMuted);

    const std::wstring target = state.targetSelected >= 0
                                    ? state.volumes[static_cast<size_t>(state.targetSelected)].drive
                                    : L"未选择";
    const std::wstring boot = state.bootSelected >= 0
                                  ? state.volumes[static_cast<size_t>(state.bootSelected)].drive
                                  : L"未选择";
    const std::wstring version =
        state.versionSelected < static_cast<int>(state.versions.size()) ? state.versions[state.versionSelected]
                                                                       : L"未选择";
    const wchar_t* firmware = state.firmwareKind == 1 ? L"BIOS"
                              : state.firmwareKind == 2 ? L"UEFI"
                                                        : L"自动";

    RECT card{kWizardContentLeft, 128, kWizardContentRight, 300};
    UiRoundRect(dc, card, 8, RGB(44, 44, 44), kUiBorder);

    const std::pair<const wchar_t*, std::wstring> rows[] = {
        {L"镜像", FileNamePart(state.wimPath)},
        {L"版本", version},
        {L"安装到", target},
        {L"引导分区", boot},
        {L"固件", firmware},
    };

    int y = 144;
    for (const auto& row : rows)
    {
        RECT labelRect{kWizardContentLeft + 18, y, kWizardContentLeft + 138, y + 28};
        UiText(dc, row.first, labelRect, state.smallFont, kUiMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT valueRect{kWizardContentLeft + 138, y, kWizardContentRight - 18, y + 28};
        UiText(dc, row.second, valueRect, state.bodyFont, kUiText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += 30;
    }

    // 进度区：Windows 11 风格进度条（圆角轨道 + 强调色填充 + 内嵌高光）
    RECT bar{kWizardContentLeft, 352, kWizardContentRight, 372};
    UiRoundRect(dc, bar, 10, RGB(40, 40, 40), RGB(70, 70, 70));
    if (percent > 0)
    {
        RECT fill = bar;
        InflateRect(&fill, -2, -2);
        const int trackWidth = fill.right - fill.left;
        fill.right = fill.left + trackWidth * std::clamp(percent, 0, 100) / 100;
        if (fill.right > fill.left)
        {
            const int radius = std::min<int>(8, (fill.right - fill.left) / 2);
            UiRoundRect(dc, fill, radius, kUiAccent, kUiAccent);

            RECT highlight{fill.left + radius, fill.top + 2, fill.right - radius, fill.top + 6};
            if (highlight.right > highlight.left)
            {
                UiFillRect(dc, highlight, RGB(126, 216, 255));
            }
        }
    }

    RECT progressRect{kWizardContentLeft, 380, kWizardContentRight - 60, 406};
    UiText(dc, state.deploying ? progress : L"点击「开始部署」后开始应用镜像并写入引导，过程中请勿关机",
           progressRect, state.smallFont, kUiMuted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT percentRect{kWizardContentRight - 56, 380, kWizardContentRight, 406};
    UiText(dc, std::to_wstring(percent) + L"%", percentRect, state.smallFont, kUiMuted,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void PaintWizardFooter(HDC dc, const InstallWizardState& state)
{
    const bool canAdvance = WizardCanAdvance(state);

    if (state.finished)
    {
        PaintWizardButton(dc, WizardCancelRect(), L"关闭", state.bodyFont, false, true,
                          state.hover == kWizardHitCancel);
        PaintWizardButton(dc, WizardNextRect(), L"重启", state.bodyFont, true, true,
                          state.hover == kWizardHitNext);
        return;
    }

    PaintWizardButton(dc, WizardCancelRect(), L"取消", state.bodyFont, false, !state.deploying,
                      state.hover == kWizardHitCancel);

    if (state.step > 0)
    {
        PaintWizardButton(dc, WizardBackRect(), L"上一步", state.bodyFont, false, !state.deploying,
                          state.hover == kWizardHitBack);
    }

    const std::wstring label = state.step < 3 ? L"下一步" : L"开始部署";
    PaintWizardButton(dc, WizardNextRect(), label, state.bodyFont, true, canAdvance && !state.deploying,
                      state.hover == kWizardHitNext);
}

void PaintWizard(InstallWizardState& state, HDC target)
{
    RECT client{};
    GetClientRect(state.window, &client);

    HDC dc = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, buffer);

    UiFillRect(dc, client, kUiBg);

    // 向导内部按 960x720 逻辑画布绘制，这里一次性映射到实际窗口尺寸：
    // 高分屏上整块界面（含字体、圆角、间距）等比放大，不必逐个改坐标。
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, kWizardWidth, kWizardHeight, nullptr);
    SetViewportExtEx(dc, client.right, client.bottom, nullptr);

    RECT panel{288, 24, kWizardContentRight + 28, 620};
    UiRoundRect(dc, panel, 12, kUiCard, kUiBorder);

    PaintWizardSidebar(dc, state);

    switch (state.step)
    {
    case 0:
        PaintWizardStepImages(dc, state);
        break;
    case 1:
        PaintWizardStepVersions(dc, state);
        break;
    case 2:
        PaintWizardStepDisks(dc, state);
        break;
    default:
        PaintWizardStepSummary(dc, state);
        break;
    }

    PaintWizardFooter(dc, state);

    // 位图拷贝用设备坐标，先把映射恢复成 1:1
    SetMapMode(dc, MM_TEXT);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(buffer);
    DeleteDC(dc);
}

int WizardHitTest(const InstallWizardState& state, int x, int y)
{
    // 鼠标坐标是窗口实际像素，先换算回 960x720 逻辑画布
    RECT client{};
    GetClientRect(state.window, &client);
    if (client.right > 0 && client.bottom > 0)
    {
        x = MulDiv(x, kWizardWidth, client.right);
        y = MulDiv(y, kWizardHeight, client.bottom);
    }

    auto inside = [&](const RECT& rect) {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    };

    if (inside(WizardCancelRect()))
    {
        return kWizardHitCancel;
    }

    if (inside(WizardNextRect()))
    {
        return kWizardHitNext;
    }

    if (state.deploying || state.finished)
    {
        return kWizardHitNone;
    }

    if (state.step > 0 && inside(WizardBackRect()))
    {
        return kWizardHitBack;
    }

    if (state.step == 0)
    {
        for (size_t i = 0; i <= state.images.size() && i <= 7; ++i)
        {
            RECT row = WizardListRowRect(static_cast<int>(i));
            if (i == state.images.size())
            {
                row.bottom = row.top + 46;
            }

            if (inside(row))
            {
                return kWizardHitListFirst + static_cast<int>(i);
            }
        }
    }
    else if (state.step == 1)
    {
        for (int i = 0; i < state.versionCount && i < 7; ++i)
        {
            if (inside(WizardListRowRect(i)))
            {
                return kWizardHitListFirst + i;
            }
        }
    }
    else if (state.step == 2)
    {
        for (size_t i = 0; i < state.volumes.size() && i < 8; ++i)
        {
            if (inside(WizardTargetRowRect(static_cast<int>(i))))
            {
                return kWizardHitListFirst + static_cast<int>(i);
            }

            if (inside(WizardBootRowRect(static_cast<int>(i))))
            {
                return kWizardHitListFirst + 1000 + static_cast<int>(i);
            }
        }
    }

    return kWizardHitNone;
}

void WizardHandleClick(InstallWizardState& state, int x, int y)
{
    const int hit = WizardHitTest(state, x, y);
    if (hit == kWizardHitNone)
    {
        return;
    }

    if (hit == kWizardHitCancel)
    {
        if (!state.deploying)
        {
            DestroyWindow(state.window);
        }
        return;
    }

    if (state.finished)
    {
        if (hit == kWizardHitNext)
        {
            HandleLauncherCommand(kCommandReboot);
        }
        return;
    }

    if (state.deploying)
    {
        return;
    }

    if (hit == kWizardHitBack)
    {
        WizardSetStep(state, state.step - 1);
        InvalidateRect(state.window, nullptr, FALSE);
        return;
    }

    if (hit == kWizardHitNext)
    {
        if (!WizardCanAdvance(state))
        {
            return;
        }

        if (state.step < 3)
        {
            if (state.step == 0)
            {
                state.wimPath = state.images[static_cast<size_t>(state.imageSelected)];
                state.versionCount = 0;
                state.versions.clear();
                state.versionSelected = 0;
            }

            WizardSetStep(state, state.step + 1);
            InvalidateRect(state.window, nullptr, FALSE);
            return;
        }

        WizardStartDeploy(state);
        InvalidateRect(state.window, nullptr, FALSE);
        return;
    }

    const int code = hit - kWizardHitListFirst;
    const int column = code / 1000;     // 0 主列 / 1 引导分区列
    const int index = code % 1000;

    if (state.step == 0)
    {
        if (index == static_cast<int>(state.images.size()))
        {
            const std::wstring picked = BrowseForImage(state.window, state.lastDirectory);
            if (!picked.empty())
            {
                state.lastDirectory = DirectoryPart(picked);
                state.images.push_back(picked);
                state.imageSelected = static_cast<int>(state.images.size()) - 1;
            }
        }
        else if (index < static_cast<int>(state.images.size()))
        {
            state.imageSelected = index;
        }
    }
    else if (state.step == 1)
    {
        if (index < state.versionCount)
        {
            state.versionSelected = index;
        }
    }
    else if (state.step == 2)
    {
        if (column == 0 && index < static_cast<int>(state.volumes.size()))
        {
            state.targetSelected = index;
        }
        else if (column == 1 && index < static_cast<int>(state.volumes.size()))
        {
            state.bootSelected = index;
        }
    }

    InvalidateRect(state.window, nullptr, FALSE);
}

LRESULT CALLBACK InstallWizardProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<InstallWizardState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message)
    {
    case WM_CREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        if (state == nullptr)
        {
            break;
        }

        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintWizard(*state, dc);
        EndPaint(window, &paint);
        return 0;
    }

    case kMessageWizardProgress:
    case kMessageWizardDone:
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE:
    {
        if (state == nullptr)
        {
            break;
        }

        const int hit = WizardHitTest(*state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hit != state->hover)
        {
            state->hover = hit;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (state != nullptr)
        {
            WizardHandleClick(*state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && state != nullptr && !state->deploying)
        {
            DestroyWindow(window);
        }
        return 0;

    case WM_CLOSE:
        if (state != nullptr && state->deploying)
        {
            return 0;
        }
        DestroyWindow(window);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void RunInstallWizard()
{
    if (g_launcher.deploying)
    {
        MessageBoxW(g_desktop.window, L"已有部署任务正在进行，请等待完成。", L"FireWin PE",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    InstallWizardState state;
    state.images = FindImagesOnVolumes();
    state.firmwareKind = DetectFirmwareKind();
    state.volumes = GetFixedVolumes(true);
    if (!state.images.empty())
    {
        state.imageSelected = 0;
        state.lastDirectory = DirectoryPart(state.images.front());
    }

    // 默认选中：目标 = 第一个非 ESP 分区；引导 = UEFI 优先 ESP，BIOS 用目标分区
    int espIndex = -1;
    for (size_t i = 0; i < state.volumes.size(); ++i)
    {
        if (state.volumes[i].isEsp)
        {
            if (espIndex < 0)
            {
                espIndex = static_cast<int>(i);
            }
            continue;
        }

        if (state.targetSelected < 0)
        {
            state.targetSelected = static_cast<int>(i);
        }
    }

    if (state.firmwareKind == 1)
    {
        state.bootSelected = state.targetSelected;
    }
    else
    {
        state.bootSelected = espIndex >= 0 ? espIndex : state.targetSelected;
    }

    RegisterDesktopClass(kInstallWizardClass, InstallWizardProc, 0);

    // 向导在 960x720 逻辑画布上绘制，字体保持原始字号，由画布映射统一放大
    state.titleFont = CreateDesktopFont(-22, FW_SEMIBOLD, false);
    state.bodyFont = CreateDesktopFont(-14, FW_NORMAL, false);
    state.smallFont = CreateDesktopFont(-12, FW_NORMAL, false);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int windowWidth = UiScale(kWizardWidth);
    const int windowHeight = UiScale(kWizardHeight);
    const int x = workArea.left + ((workArea.right - workArea.left) - windowWidth) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - windowHeight) / 2;

    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kInstallWizardClass, L"安装 Windows", WS_POPUP, x, y,
                                  windowWidth, windowHeight, g_desktop.window, nullptr,
                                  GetModuleHandleW(nullptr), &state);
    if (window == nullptr)
    {
        DeleteObject(state.titleFont);
        DeleteObject(state.bodyFont);
        DeleteObject(state.smallFont);
        return;
    }

    state.window = window;
    SetWindowRgn(window, CreateRoundRectRgn(0, 0, windowWidth + 1, windowHeight + 1,
                                            UiScale(12), UiScale(12)), TRUE);
    EnableWindow(g_desktop.window, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (IsWindow(window))
    {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                break;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        else
        {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        }
    }

    if (state.worker.joinable())
    {
        state.worker.join();
    }

    state.window = nullptr;
    EnableWindow(g_desktop.window, TRUE);
    SetForegroundWindow(g_desktop.window);

    DeleteObject(state.titleFont);
    DeleteObject(state.bodyFont);
    DeleteObject(state.smallFont);
}


constexpr COLORREF kGlyphWhite = RGB(244, 246, 252);
constexpr COLORREF kGlyphSlate = RGB(150, 160, 184);
constexpr COLORREF kGlyphDark = RGB(26, 28, 38);
constexpr COLORREF kGlyphOrange = RGB(255, 138, 74);
constexpr COLORREF kGlyphAmber = RGB(246, 184, 86);
constexpr COLORREF kGlyphTeal = RGB(64, 198, 184);
constexpr COLORREF kGlyphBlue = RGB(88, 152, 248);
constexpr COLORREF kGlyphGreen = RGB(96, 206, 132);
constexpr COLORREF kGlyphRed = RGB(240, 96, 84);

void PaintGlyphTile(HDC dc, const RECT& tile, int radius)
{
    UiRoundRect(dc, tile, radius, kUiTile, kUiBorder);

    // 顶部高光：让底板有一点玻璃质感，所有图标完全一致
    RECT highlight = tile;
    InflateRect(&highlight, -1, -1);
    highlight.bottom = tile.top + (tile.bottom - tile.top) / 2;

    HBRUSH brush = CreateSolidBrush(RGB(52, 52, 56));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, highlight.left, highlight.top, highlight.right, highlight.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

// 用 GDI 图元画多色矢量图标（等效于「画 SVG」），按传入方框等比缩放。
void PaintVectorGlyph(HDC dc, const RECT& box, DesktopGlyph glyph)
{
    const int w = box.right - box.left;
    const int h = box.bottom - box.top;
    if (w < 8 || h < 8)
    {
        return;
    }

    if (glyph == DesktopGlyph::None)
    {
        glyph = DesktopGlyph::App;
    }

    auto X = [&](double t) { return box.left + static_cast<int>(t * w + 0.5); };
    auto Y = [&](double t) { return box.top + static_cast<int>(t * h + 0.5); };
    const int stroke = std::max(1, (w + 7) / 14);

    auto fillPoly = [&](COLORREF color, const std::vector<std::pair<double, double>>& points) {
        std::vector<POINT> pts;
        pts.reserve(points.size());
        for (const auto& point : points)
        {
            pts.push_back({X(point.first), Y(point.second)});
        }

        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        Polygon(dc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    };
    auto fillRect = [&](COLORREF color, double l, double t, double r, double b) {
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        Rectangle(dc, X(l), Y(t), X(r), Y(b));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    };
    auto fillEllipse = [&](COLORREF color, double l, double t, double r, double b) {
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        Ellipse(dc, X(l), Y(t), X(r), Y(b));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    };
    auto fillRound = [&](COLORREF color, double l, double t, double r, double b, double radius) {
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, X(l), Y(t), X(r), Y(b),
                  std::max(2, static_cast<int>(radius * w)), std::max(2, static_cast<int>(radius * h)));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    };
    auto dot = [&](COLORREF color, double cx, double cy, double r) {
        fillEllipse(color, cx - r, cy - r, cx + r, cy + r);
    };
    auto strokeLine = [&](COLORREF color, int width, double x1, double y1, double x2, double y2) {
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        MoveToEx(dc, X(x1), Y(y1), nullptr);
        LineTo(dc, X(x2), Y(y2));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    };
    auto strokeCircle = [&](COLORREF color, int width, double cx, double cy, double r) {
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, X(cx - r), Y(cy - r), X(cx + r), Y(cy + r));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    };
    auto strokeRect = [&](COLORREF color, int width, double l, double t, double r, double b) {
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, X(l), Y(t), X(r), Y(b));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    };
    auto strokeArc = [&](COLORREF color, int width, double l, double t, double r, double b,
                         double x1, double y1, double x2, double y2) {
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Arc(dc, X(l), Y(t), X(r), Y(b), X(x1), Y(y1), X(x2), Y(y2));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    };

    switch (glyph)
    {
    case DesktopGlyph::Install:
        // 橙色下载箭头 + 白色托盘
        fillPoly(kGlyphOrange, {{0.46, 0.04}, {0.56, 0.04}, {0.56, 0.46}, {0.80, 0.46},
                                {0.51, 0.76}, {0.22, 0.46}, {0.46, 0.46}});
        fillPoly(kGlyphWhite, {{0.16, 0.66}, {0.26, 0.66}, {0.26, 0.84}, {0.76, 0.84},
                               {0.76, 0.66}, {0.86, 0.66}, {0.86, 0.94}, {0.16, 0.94}});
        break;

    case DesktopGlyph::Driver:
    {
        // 琥珀齿轮 + 深色轴孔 + 白色轴心
        const double kTeeth[8][2] = {{1.0, 0.0}, {0.707, 0.707}, {0.0, 1.0}, {-0.707, 0.707},
                                     {-1.0, 0.0}, {-0.707, -0.707}, {0.0, -1.0}, {0.707, -0.707}};
        for (const auto& tooth : kTeeth)
        {
            strokeLine(kGlyphAmber, std::max(2, w / 7),
                       0.5 + tooth[0] * 0.30, 0.5 + tooth[1] * 0.30,
                       0.5 + tooth[0] * 0.47, 0.5 + tooth[1] * 0.47);
        }
        fillEllipse(kGlyphAmber, 0.17, 0.17, 0.83, 0.83);
        dot(kGlyphDark, 0.50, 0.50, 0.15);
        dot(kGlyphWhite, 0.50, 0.50, 0.055);
        break;
    }

    case DesktopGlyph::Network:
        // 白色连线 + 青/蓝/绿三个节点
        strokeLine(kGlyphWhite, stroke, 0.50, 0.26, 0.22, 0.70);
        strokeLine(kGlyphWhite, stroke, 0.50, 0.26, 0.78, 0.70);
        strokeLine(kGlyphWhite, stroke, 0.22, 0.70, 0.78, 0.70);
        dot(kGlyphTeal, 0.50, 0.24, 0.14);
        dot(kGlyphBlue, 0.21, 0.72, 0.13);
        dot(kGlyphGreen, 0.79, 0.72, 0.13);
        break;

    case DesktopGlyph::Disk:
        // 蓝色硬盘 + 白色槽位 + 绿色指示灯
        fillRound(RGB(84, 116, 168), 0.04, 0.20, 0.96, 0.80, 0.10);
        strokeLine(kGlyphWhite, stroke, 0.18, 0.38, 0.60, 0.38);
        strokeLine(RGB(52, 74, 112), stroke, 0.18, 0.56, 0.60, 0.56);
        dot(kGlyphGreen, 0.79, 0.62, 0.075);
        break;

    case DesktopGlyph::Refresh:
    case DesktopGlyph::Reboot:
        // 白色环形箭头 + 彩色箭头头部
        strokeArc(kGlyphWhite, std::max(2, stroke + 1), 0.12, 0.12, 0.88, 0.88,
                  0.50, 0.12, 0.72, 0.18);
        fillPoly(glyph == DesktopGlyph::Refresh ? kGlyphOrange : kGlyphBlue,
                 {{0.44, 0.02}, {0.44, 0.30}, {0.78, 0.16}});
        break;

    case DesktopGlyph::Power:
        // 红色电源弧 + 白色竖线
        strokeArc(kGlyphRed, std::max(2, stroke + 1), 0.14, 0.20, 0.86, 0.92,
                  0.14, 0.42, 0.86, 0.42);
        strokeLine(kGlyphWhite, std::max(2, stroke + 1), 0.50, 0.06, 0.50, 0.50);
        break;

    case DesktopGlyph::Launcher:
        // 蓝色窗口 + 红/琥珀/绿三个窗口按钮
        fillRound(kGlyphBlue, 0.06, 0.14, 0.94, 0.86, 0.10);
        strokeLine(kGlyphWhite, 1, 0.06, 0.34, 0.94, 0.34);
        dot(kGlyphRed, 0.19, 0.24, 0.05);
        dot(kGlyphAmber, 0.32, 0.24, 0.05);
        dot(kGlyphGreen, 0.45, 0.24, 0.05);
        break;

    case DesktopGlyph::Folder:
        // 琥珀文件夹背板 + 橙色前盖
        fillPoly(kGlyphAmber, {{0.06, 0.26}, {0.36, 0.26}, {0.44, 0.36}, {0.94, 0.36},
                              {0.94, 0.84}, {0.06, 0.84}});
        fillPoly(kGlyphOrange, {{0.06, 0.42}, {0.94, 0.42}, {0.94, 0.90}, {0.06, 0.90}});
        strokeLine(RGB(255, 196, 150), 1, 0.14, 0.46, 0.86, 0.46);
        break;

    case DesktopGlyph::Document:
        // 白色纸张 + 灰色折角 + 蓝色文字线
        fillRect(kGlyphWhite, 0.24, 0.06, 0.76, 0.94);
        fillPoly(kGlyphSlate, {{0.56, 0.06}, {0.76, 0.26}, {0.56, 0.26}});
        strokeLine(kGlyphBlue, stroke, 0.34, 0.46, 0.66, 0.46);
        strokeLine(kGlyphBlue, stroke, 0.34, 0.60, 0.66, 0.60);
        strokeLine(kGlyphBlue, stroke, 0.34, 0.74, 0.56, 0.74);
        break;

    case DesktopGlyph::Console:
        // 深色终端 + 绿色提示符 + 白色光标
        fillRound(kGlyphDark, 0.04, 0.14, 0.96, 0.86, 0.08);
        strokeRect(kGlyphSlate, 1, 0.04, 0.14, 0.96, 0.86);
        strokeLine(kGlyphGreen, std::max(2, stroke + 1), 0.22, 0.34, 0.38, 0.50);
        strokeLine(kGlyphGreen, std::max(2, stroke + 1), 0.38, 0.50, 0.22, 0.66);
        strokeLine(kGlyphWhite, std::max(1, stroke), 0.50, 0.68, 0.74, 0.68);
        break;

    case DesktopGlyph::Archive:
        // 琥珀压缩包 + 橙色包盖 + 白色拉链
        fillRound(kGlyphAmber, 0.10, 0.26, 0.90, 0.90, 0.06);
        fillRect(kGlyphOrange, 0.06, 0.12, 0.94, 0.30);
        fillRect(kGlyphWhite, 0.46, 0.30, 0.54, 0.90);
        fillRect(kGlyphDark, 0.42, 0.40, 0.58, 0.44);
        fillRect(kGlyphDark, 0.42, 0.54, 0.58, 0.58);
        fillRect(kGlyphDark, 0.42, 0.68, 0.58, 0.72);
        fillRect(kGlyphDark, 0.42, 0.82, 0.58, 0.86);
        break;

    case DesktopGlyph::Chip:
        // 青色芯片 + 白色引脚 + 深色内核
        for (int i = 0; i < 3; ++i)
        {
            const double offset = 0.30 + i * 0.20;
            strokeLine(kGlyphWhite, std::max(1, stroke), 0.24, offset, 0.08, offset);
            strokeLine(kGlyphWhite, std::max(1, stroke), 0.76, offset, 0.92, offset);
            strokeLine(kGlyphWhite, std::max(1, stroke), offset, 0.24, offset, 0.08);
            strokeLine(kGlyphWhite, std::max(1, stroke), offset, 0.76, offset, 0.92);
        }
        fillRound(kGlyphTeal, 0.24, 0.24, 0.76, 0.76, 0.06);
        fillRound(kGlyphDark, 0.38, 0.38, 0.62, 0.62, 0.04);
        dot(kGlyphWhite, 0.50, 0.50, 0.05);
        break;

    case DesktopGlyph::Search:
        // 白色镜圈 + 青蓝镜片 + 蓝色手柄
        fillEllipse(RGB(44, 74, 92), 0.12, 0.12, 0.72, 0.72);
        strokeCircle(kGlyphWhite, std::max(2, stroke + 1), 0.42, 0.42, 0.30);
        strokeLine(kGlyphBlue, std::max(2, stroke + 1), 0.64, 0.64, 0.90, 0.90);
        break;

    case DesktopGlyph::Key:
        // 琥珀钥匙 + 白色齿
        strokeCircle(kGlyphAmber, std::max(2, stroke + 1), 0.28, 0.50, 0.18);
        strokeLine(kGlyphAmber, std::max(2, stroke + 1), 0.44, 0.50, 0.92, 0.50);
        strokeLine(kGlyphWhite, std::max(1, stroke), 0.72, 0.50, 0.72, 0.70);
        strokeLine(kGlyphWhite, std::max(1, stroke), 0.84, 0.50, 0.84, 0.66);
        break;

    case DesktopGlyph::Disc:
        // 蓝色光盘 + 白色反光 + 深色中心孔
        fillEllipse(kGlyphBlue, 0.06, 0.06, 0.94, 0.94);
        strokeArc(kGlyphWhite, std::max(2, stroke), 0.28, 0.28, 0.92, 0.92,
                  0.24, 0.48, 0.50, 0.14);
        strokeCircle(kGlyphWhite, 1, 0.50, 0.50, 0.34);
        dot(kGlyphDark, 0.50, 0.50, 0.10);
        break;

    case DesktopGlyph::Display:
        // 深色显示器 + 蓝色屏幕 + 白色支架
        fillRound(kGlyphDark, 0.08, 0.14, 0.92, 0.70, 0.08);
        fillRound(kGlyphBlue, 0.16, 0.22, 0.84, 0.62, 0.04);
        strokeLine(kGlyphWhite, std::max(2, stroke), 0.50, 0.70, 0.50, 0.82);
        strokeLine(kGlyphWhite, std::max(2, stroke), 0.32, 0.84, 0.68, 0.84);
        break;

    default:
        // 通用程序窗口 + 红/琥珀/绿三个按钮
        fillRound(kGlyphBlue, 0.06, 0.14, 0.94, 0.86, 0.10);
        strokeLine(kGlyphWhite, 1, 0.06, 0.32, 0.94, 0.32);
        dot(kGlyphRed, 0.18, 0.23, 0.05);
        dot(kGlyphAmber, 0.31, 0.23, 0.05);
        dot(kGlyphGreen, 0.44, 0.23, 0.05);
        break;
    }
}

// ---------------------------------------------------------------------------
// 桌面 / 开始菜单条目
// ---------------------------------------------------------------------------
// 从 exe 里按 48×48 取第一个图标；取不到时返回 nullptr，绘制侧会退回矢量图标
HICON LoadProgramIcon(const std::wstring& exePath)
{
    if (exePath.empty() || !FileExists(exePath))
    {
        return nullptr;
    }

    // PrivateExtractIconsW 会从 exe 的多尺寸图标里挑最接近 48×48 的那张，
    // 比 ExtractIconEx 固定给 32×32 再放大清楚得多
    HICON icon = nullptr;
    if (PrivateExtractIconsW(exePath.c_str(), 0, 48, 48, &icon, nullptr, 1, 0) > 0 && icon != nullptr)
    {
        return icon;
    }

    HICON large = nullptr;
    HICON small = nullptr;
    if (ExtractIconExW(exePath.c_str(), 0, &large, &small, 1) <= 0)
    {
        return nullptr;
    }

    if (small != nullptr)
    {
        DestroyIcon(small);
    }

    return large;
}

DesktopEntry MakeExeEntry(const std::wstring& title, const std::wstring& exePath,
                          DesktopGlyph glyph = DesktopGlyph::App)
{
    DesktopEntry entry;
    entry.title = title;
    entry.exePath = exePath;
    entry.glyph = glyph;
    entry.icon = LoadProgramIcon(exePath);
    return entry;
}

DesktopEntry MakeActionEntry(const std::wstring& title, int actionId,
                             DesktopGlyph glyph = DesktopGlyph::None)
{
    DesktopEntry entry;
    entry.title = title;
    entry.actionId = actionId;
    entry.glyph = glyph;
    return entry;
}

DesktopEntry MakeSeparator()
{
    return DesktopEntry{};
}

void ClearEntries(std::vector<DesktopEntry>& entries)
{
    for (DesktopEntry& entry : entries)
    {
        if (entry.icon != nullptr)
        {
            DestroyIcon(entry.icon);
            entry.icon = nullptr;
        }
    }

    entries.clear();
}

// 动态工具没有固定图标：按名称关键字挑一个同风格的矢量图标
DesktopGlyph GlyphForTitle(const std::wstring& title)
{
    const std::wstring lower = ToLower(title);

    struct Rule
    {
        const wchar_t* keyword;
        DesktopGlyph glyph;
    };
    static const Rule kRules[] = {
        {L"7-zip", DesktopGlyph::Archive},
        {L"explorer", DesktopGlyph::Folder},
        {L"q-dir", DesktopGlyph::Folder},
        {L"文件管理", DesktopGlyph::Folder},
        {L"notepad", DesktopGlyph::Document},
        {L"记事本", DesktopGlyph::Document},
        {L"命令提示符", DesktopGlyph::Console},
        {L"diskgenius", DesktopGlyph::Disk},
        {L"分区", DesktopGlyph::Disk},
        {L"磁盘", DesktopGlyph::Disk},
        {L"硬盘", DesktopGlyph::Disk},
        {L"testdisk", DesktopGlyph::Disk},
        {L"drive", DesktopGlyph::Driver},
        {L"驱动", DesktopGlyph::Driver},
        {L"网络", DesktopGlyph::Network},
        {L"tcpview", DesktopGlyph::Network},
        {L"cpu", DesktopGlyph::Chip},
        {L"gpu", DesktopGlyph::Chip},
        {L"密码", DesktopGlyph::Key},
        {L"ntpwedit", DesktopGlyph::Key},
        {L"everything", DesktopGlyph::Search},
        {L"搜索", DesktopGlyph::Search},
        {L"光驱", DesktopGlyph::Disc},
        {L"osfmount", DesktopGlyph::Disc},
    };

    for (const Rule& rule : kRules)
    {
        if (lower.find(rule.keyword) != std::wstring::npos)
        {
            return rule.glyph;
        }
    }

    return DesktopGlyph::App;
}

void BuildDesktopItems()
{
    ClearEntries(g_desktop.icons);

    const std::wstring systemDir = GetSystemDirectoryPath();
    const std::wstring appsDir = JoinPath(g_launcher.toolsDir, L"Apps");

    // 有 deploy.json 时提供「一键开始安装」，否则只给手动安装入口
    if (!g_launcher.configPath.empty())
    {
        g_desktop.icons.push_back(MakeActionEntry(L"开始安装", kCommandDeploy, DesktopGlyph::Install));
    }

    g_desktop.icons.push_back(MakeActionEntry(L"安装系统", kCommandInstall, DesktopGlyph::Install));
    g_desktop.icons.push_back(MakeActionEntry(L"加载驱动", kCommandLoadDriver, DesktopGlyph::Driver));
    g_desktop.icons.push_back(MakeActionEntry(L"设置分辨率", kCommandDisplayMode, DesktopGlyph::Display));

    // 资源管理器常驻桌面（Windows 习惯位置）：优先 Explorer++，缺失时由 kCommandExplorer 兜底
    g_desktop.icons.push_back(MakeActionEntry(L"资源管理器", kCommandExplorer, DesktopGlyph::Folder));

    const std::wstring sevenZipExe = JoinPath(JoinPath(appsDir, L"7-Zip"), L"7zFM.exe");
    if (FileExists(sevenZipExe))
    {
        g_desktop.icons.push_back(MakeExeEntry(L"7-Zip", sevenZipExe, DesktopGlyph::Archive));
    }

    const std::wstring notepadPlusExe = JoinPath(JoinPath(appsDir, L"Notepad++"), L"notepad++.exe");
    if (FileExists(notepadPlusExe))
    {
        g_desktop.icons.push_back(MakeExeEntry(L"Notepad++", notepadPlusExe, DesktopGlyph::Document));
    }

    const wchar_t* const kSystemExes[] = {L"notepad.exe", L"regedit.exe", L"cmd.exe", L"diskpart.exe"};
    const wchar_t* const kSystemTitles[] = {L"记事本", L"注册表编辑器", L"命令提示符", L"磁盘管理"};
    const DesktopGlyph kSystemGlyphs[] = {DesktopGlyph::Document, DesktopGlyph::App, DesktopGlyph::Console,
                                          DesktopGlyph::Disk};
    for (size_t i = 0; i < ARRAYSIZE(kSystemExes); ++i)
    {
        const std::wstring exe = JoinPath(systemDir, kSystemExes[i]);
        if (FileExists(exe))
        {
            g_desktop.icons.push_back(MakeExeEntry(kSystemTitles[i], exe, kSystemGlyphs[i]));
        }
    }

    const std::wstring bootTool = JoinPath(g_launcher.toolsDir, L"FireBoot.exe");
    if (FileExists(bootTool))
    {
        g_desktop.icons.push_back(MakeExeEntry(L"引导工具", bootTool, DesktopGlyph::App));
    }

    g_desktop.icons.push_back(MakeActionEntry(L"网络初始化", kCommandInitNetwork, DesktopGlyph::Network));

    // 外置工具：复用启动器的扫描结果，避免维护第二份工具表
    for (const auto& tool : g_launcher.appTools)
    {
        g_desktop.icons.push_back(MakeExeEntry(tool.title, tool.exe, GlyphForTitle(tool.title)));
    }
}

// 开始菜单两列的左边界
int StartMenuColumnLeft(int column)
{
    return kStartMenuPadding + column * (kStartMenuColumnWidth + kStartMenuColumnGap);
}

void BuildStartItems(int screenHeight)
{
    ClearEntries(g_desktop.startItems);

    auto addHeader = [](const wchar_t* title) {
        DesktopEntry entry;
        entry.title = title;
        entry.isHeader = true;
        g_desktop.startItems.push_back(entry);
    };
    auto addTool = [](const std::wstring& title, const std::wstring& exe) {
        g_desktop.startItems.push_back(MakeExeEntry(title, exe, GlyphForTitle(title)));
    };
    auto hasCategory = [](const wchar_t* category) {
        for (const AppTool& tool : g_launcher.appTools)
        {
            if (tool.category == category)
            {
                return true;
            }
        }
        return false;
    };
    auto addCategoryTools = [&](const wchar_t* category) {
        for (const AppTool& tool : g_launcher.appTools)
        {
            if (tool.category == category)
            {
                addTool(tool.title, tool.exe);
            }
        }
    };

    const std::wstring systemDir = GetSystemDirectoryPath();
    const std::wstring appsDir = JoinPath(g_launcher.toolsDir, L"Apps");

    // ---- 安装与部署 ----
    addHeader(L"安装与部署");
    if (!g_launcher.configPath.empty())
    {
        g_desktop.startItems.push_back(MakeActionEntry(L"开始安装", kCommandDeploy, DesktopGlyph::Install));
    }
    g_desktop.startItems.push_back(MakeActionEntry(L"安装系统…", kCommandInstall, DesktopGlyph::Install));
    g_desktop.startItems.push_back(MakeActionEntry(L"加载驱动…", kCommandLoadDriver, DesktopGlyph::Driver));
    g_desktop.startItems.push_back(MakeActionEntry(L"设置分辨率…", kCommandDisplayMode, DesktopGlyph::Display));
    const std::wstring bootTool = JoinPath(g_launcher.toolsDir, L"FireBoot.exe");
    if (FileExists(bootTool))
    {
        g_desktop.startItems.push_back(MakeExeEntry(L"引导工具", bootTool, DesktopGlyph::App));
    }
    g_desktop.startItems.push_back(MakeActionEntry(L"网络初始化", kCommandInitNetwork, DesktopGlyph::Network));
    addCategoryTools(L"安装与部署");

    // ---- 分区与磁盘 ----
    addHeader(L"分区与磁盘");
    const std::wstring diskpartExe = JoinPath(systemDir, L"diskpart.exe");
    if (FileExists(diskpartExe))
    {
        g_desktop.startItems.push_back(MakeExeEntry(L"磁盘管理", diskpartExe, DesktopGlyph::Disk));
    }
    addCategoryTools(L"分区与磁盘");

    // ---- 硬件检测 ----
    if (hasCategory(L"硬件检测"))
    {
        addHeader(L"硬件检测");
        addCategoryTools(L"硬件检测");
    }

    // ---- 系统维护 ----
    addHeader(L"系统维护");
    const wchar_t* const kSystemExes[] = {L"notepad.exe", L"regedit.exe", L"cmd.exe"};
    const wchar_t* const kSystemTitles[] = {L"记事本", L"注册表编辑器", L"命令提示符"};
    const DesktopGlyph kSystemGlyphs[] = {DesktopGlyph::Document, DesktopGlyph::App, DesktopGlyph::Console};
    for (size_t i = 0; i < ARRAYSIZE(kSystemExes); ++i)
    {
        const std::wstring exe = JoinPath(systemDir, kSystemExes[i]);
        if (FileExists(exe))
        {
            g_desktop.startItems.push_back(MakeExeEntry(kSystemTitles[i], exe, kSystemGlyphs[i]));
        }
    }
    addCategoryTools(L"系统维护");
    g_desktop.startItems.push_back(MakeActionEntry(L"刷新桌面", kDesktopRefresh, DesktopGlyph::Refresh));
    g_desktop.startItems.push_back(MakeActionEntry(L"经典启动器", kDesktopOpenLauncher, DesktopGlyph::Launcher));

    // ---- 文件与网络 ----
    addHeader(L"文件与网络");
    g_desktop.startItems.push_back(MakeActionEntry(L"资源管理器", kCommandExplorer, DesktopGlyph::Folder));
    const std::wstring sevenZipExe = JoinPath(JoinPath(appsDir, L"7-Zip"), L"7zFM.exe");
    if (FileExists(sevenZipExe))
    {
        g_desktop.startItems.push_back(MakeExeEntry(L"7-Zip", sevenZipExe, DesktopGlyph::Archive));
    }
    const std::wstring notepadPlusExe = JoinPath(JoinPath(appsDir, L"Notepad++"), L"notepad++.exe");
    if (FileExists(notepadPlusExe))
    {
        g_desktop.startItems.push_back(MakeExeEntry(L"Notepad++", notepadPlusExe, DesktopGlyph::Document));
    }
    addCategoryTools(L"文件与网络");

    // ---- 其它工具（未收录目录扫描结果） ----
    if (hasCategory(L"其它工具"))
    {
        addHeader(L"其它工具");
        addCategoryTools(L"其它工具");
    }

    // ---- 电源 ----
    addHeader(L"电源");
    g_desktop.startItems.push_back(MakeActionEntry(L"重启", kCommandReboot, DesktopGlyph::Reboot));
    g_desktop.startItems.push_back(MakeActionEntry(L"关机", kCommandShutdown, DesktopGlyph::Power));

    // 两列布局：在接近一半的分组标题处断开，避免把同一个分组劈成两半
    const int total = static_cast<int>(g_desktop.startItems.size());
    int split = total / 2;
    int bestScore = total + 1;
    for (int i = 1; i < total; ++i)
    {
        if (!g_desktop.startItems[static_cast<size_t>(i)].isHeader)
        {
            continue;
        }

        const int score = std::abs(i - total / 2);
        if (score < bestScore)
        {
            bestScore = score;
            split = i;
        }
    }
    g_desktop.menuSplit = split;

    // 行高自适应：两列后一般无需压缩，768p 屏幕也能完整显示
    auto columnHeight = [&](int from, int to, int row) {
        int height = 0;
        for (int i = from; i < to; ++i)
        {
            height += g_desktop.startItems[static_cast<size_t>(i)].title.empty() ? kMenuSeparator : row;
        }
        return height;
    };
    const int maxHeight = screenHeight - kTaskbarHeight - UiScale(24);
    int rowHeight = UiScale(32);
    const int minRowHeight = UiScale(22);
    while (rowHeight > minRowHeight &&
           kStartMenuHeader + UiScale(10) +
                   std::max(columnHeight(0, split, rowHeight), columnHeight(split, total, rowHeight)) >
               maxHeight)
    {
        --rowHeight;
    }

    g_desktop.menuRowHeight = rowHeight;
    g_desktop.menuHeight = kStartMenuHeader + UiScale(10) +
                           std::max(columnHeight(0, split, rowHeight), columnHeight(split, total, rowHeight));
}

int StartMenuHitTest(int x, int y)
{
    if (y < kStartMenuHeader)
    {
        return kDesktopHitNone;
    }

    const int column = x >= StartMenuColumnLeft(1) ? 1 : 0;
    const int from = column == 0 ? 0 : g_desktop.menuSplit;
    const int to = column == 0 ? g_desktop.menuSplit : static_cast<int>(g_desktop.startItems.size());

    int top = kStartMenuHeader;
    for (int i = from; i < to; ++i)
    {
        const DesktopEntry& entry = g_desktop.startItems[static_cast<size_t>(i)];
        if (entry.isHeader)
        {
            top += g_desktop.menuRowHeight;
            continue;
        }

        if (entry.title.empty())
        {
            top += kMenuSeparator;
            continue;
        }

        if (y >= top && y < top + g_desktop.menuRowHeight)
        {
            return i;
        }

        top += g_desktop.menuRowHeight;
    }

    return kDesktopHitNone;
}

// ---------------------------------------------------------------------------
// 桌面图标 / 任务栏几何
// ---------------------------------------------------------------------------
RECT DesktopCellRect(int index)
{
    // 竖向排列：先从上到下填满一列，再换到右边一列（和 Windows 桌面一致）
    const int rows = std::max(1, g_desktop.rows);
    RECT rect{};
    rect.left = kDesktopMargin + (index / rows) * kCellWidth;
    rect.top = kDesktopMargin + (index % rows) * kCellHeight;
    rect.right = rect.left + kCellWidth;
    rect.bottom = rect.top + kCellHeight;
    return rect;
}

// 图标格是否完整落在任务栏之上的可见区域：低分辨率 + 大量外置工具时避免压到任务栏
bool DesktopCellOnScreen(int index)
{
    const RECT cell = DesktopCellRect(index);
    return cell.bottom <= GetSystemMetrics(SM_CYSCREEN) - kTaskbarHeight &&
           cell.right <= GetSystemMetrics(SM_CXSCREEN) - 2;
}

int DesktopHitTest(int x, int y)
{
    for (size_t i = 0; i < g_desktop.icons.size(); ++i)
    {
        if (!DesktopCellOnScreen(static_cast<int>(i)))
        {
            continue;
        }

        const RECT cell = DesktopCellRect(static_cast<int>(i));
        if (x >= cell.left && x < cell.right && y >= cell.top && y < cell.bottom)
        {
            return static_cast<int>(i);
        }
    }

    return kDesktopHitNone;
}

BOOL CALLBACK CollectTaskWindow(HWND window, LPARAM parameter)
{
    auto* windows = reinterpret_cast<std::vector<HWND>*>(parameter);

    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
    {
        return TRUE;
    }

    if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0 ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0)
    {
        return TRUE;
    }

    if (GetWindowTextLengthW(window) == 0)
    {
        return TRUE;
    }

    wchar_t className[64]{};
    GetClassNameW(window, className, ARRAYSIZE(className));
    if (wcscmp(className, kDesktopClass) == 0 || wcscmp(className, kTaskbarClass) == 0 ||
        wcscmp(className, kStartMenuClass) == 0)
    {
        return TRUE;
    }

    windows->push_back(window);
    return TRUE;
}

void RefreshTaskWindows()
{
    g_desktop.taskWindows.clear();
    EnumWindows(CollectTaskWindow, reinterpret_cast<LPARAM>(&g_desktop.taskWindows));
}

RECT TaskButtonRect(int index)
{
    RECT client{};
    if (g_desktop.taskbar == nullptr || GetClientRect(g_desktop.taskbar, &client) == FALSE)
    {
        return RECT{0, 0, 0, 0};
    }

    const int count = static_cast<int>(g_desktop.taskWindows.size());
    const int gap = UiScale(6);
    const int left = UiScale(8) + kStartButtonWidth + UiScale(10);
    const int available = client.right - left - kClockWidth - UiScale(8);
    if (count <= 0 || available <= 0)
    {
        return RECT{0, 0, 0, 0};
    }

    int width = (available - (count - 1) * gap) / count;
    width = std::min(UiScale(190), std::max(UiScale(76), width));

    RECT rect{};
    rect.left = left + index * (width + gap);
    rect.top = UiScale(6);
    rect.right = rect.left + width;
    rect.bottom = kTaskbarHeight - UiScale(6);
    return rect;
}

int TaskButtonHitTest(int x, int y)
{
    for (size_t i = 0; i < g_desktop.taskWindows.size(); ++i)
    {
        const RECT rect = TaskButtonRect(static_cast<int>(i));
        if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
        {
            return static_cast<int>(i);
        }
    }

    return kDesktopHitNone;
}

// ---------------------------------------------------------------------------
// 启动条目
// ---------------------------------------------------------------------------
bool IsConsoleTool(const std::wstring& exePath)
{
    const std::wstring lowered = ToLower(exePath);
    static const wchar_t* const kConsoleHints[] = {L"cmd.exe", L"diskpart", L"testdisk", L"qphotorec"};
    for (const wchar_t* hint : kConsoleHints)
    {
        if (lowered.find(hint) != std::wstring::npos)
        {
            return true;
        }
    }

    return false;
}

void LaunchDesktopEntry(const DesktopEntry& entry)
{
    if (g_desktop.menuOpen)
    {
        ShowStartMenu(false);
    }

    if (!entry.exePath.empty())
    {
        const size_t slash = entry.exePath.find_last_of(L"\\/");
        const std::wstring directory = slash == std::wstring::npos ? std::wstring()
                                                                   : entry.exePath.substr(0, slash);
        LaunchTool(entry.exePath, std::wstring(), IsConsoleTool(entry.exePath), directory);
        return;
    }

    switch (entry.actionId)
    {
    case kDesktopRefresh:
        BuildDesktopItems();
        BuildStartItems(GetSystemMetrics(SM_CYSCREEN));
        g_desktop.selected = kDesktopHitNone;
        g_desktop.hover = kDesktopHitNone;
        if (g_desktop.window != nullptr)
        {
            InvalidateRect(g_desktop.window, nullptr, FALSE);
        }
        break;
    case kDesktopOpenLauncher:
    {
        wchar_t self[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, self, ARRAYSIZE(self)) > 0)
        {
            LaunchTool(self, L"/shell", false, GetExeDirectory());
        }
        break;
    }
    default:
        HandleLauncherCommand(entry.actionId);
        break;
    }
}

// ---------------------------------------------------------------------------
// 绘制：壁纸 / 桌面 / 任务栏 / 开始菜单
// ---------------------------------------------------------------------------
// 等比铺满（cover）：裁掉多余部分，不变形
void PaintImageCover(HDC dc, HBITMAP image, int width, int height, BYTE alpha = 255)
{
    RECT full{0, 0, width, height};
    FillRect(dc, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    BITMAP bitmap{};
    if (image == nullptr || GetObjectW(image, sizeof(bitmap), &bitmap) == 0 ||
        bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0)
    {
        return;
    }

    HDC source = CreateCompatibleDC(dc);
    if (source == nullptr)
    {
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(source, image);
    const double scale = std::max(static_cast<double>(width) / bitmap.bmWidth,
                                  static_cast<double>(height) / bitmap.bmHeight);
    const int sourceWidth = static_cast<int>(width / scale);
    const int sourceHeight = static_cast<int>(height / scale);
    const int sourceX = std::max<int>(0, (bitmap.bmWidth - sourceWidth) / 2);
    const int sourceY = std::max<int>(0, (bitmap.bmHeight - sourceHeight) / 2);

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = alpha;
    AlphaBlend(dc, 0, 0, width, height, source, sourceX, sourceY, sourceWidth, sourceHeight, blend);

    SelectObject(source, oldBitmap);
    DeleteDC(source);
}

void PaintDesktopWallpaper(HDC dc, int width, int height)
{
    PaintImageCover(dc, g_desktop.background, width, height);
}

void PaintDesktopIcon(HDC dc, int index)
{
    const DesktopEntry& entry = g_desktop.icons[static_cast<size_t>(index)];
    const RECT cell = DesktopCellRect(index);
    const bool selected = g_desktop.selected == index;
    const bool hovered = g_desktop.hover == index;

    if (selected || hovered)
    {
        RECT highlight{cell.left + 3, cell.top + 2, cell.right - 3, cell.bottom - 2};
        UiRoundRect(dc, highlight, 8, selected ? RGB(48, 40, 34) : RGB(34, 34, 42),
                    selected ? kUiAccent : RGB(72, 72, 84));
    }

    const int iconLeft = cell.left + (kCellWidth - kIconSize) / 2;
    const int iconTop = cell.top + UiScale(8);

    if (entry.icon != nullptr)
    {
        // 有程序自带图标就直接画原图（Windows 桌面也是这个观感，不加底板）
        DrawIconEx(dc, iconLeft, iconTop, entry.icon, kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
    }
    else
    {
        // 内置动作（安装系统 / 加载驱动等）没有 exe：统一走深色圆角底板 + 矢量图形
        const RECT tile{iconLeft, iconTop + 2, iconLeft + kIconSize, iconTop + kIconSize};
        PaintGlyphTile(dc, tile, UiScale(12));

        RECT glyphBox{tile.left + UiScale(8), tile.top + UiScale(8), tile.right - UiScale(8),
                      tile.bottom - UiScale(8)};
        PaintVectorGlyph(dc, glyphBox, entry.glyph);
    }

    // 标签：先画 1px 黑色阴影，保证在壁纸上清晰可读
    RECT label{cell.left + 2, iconTop + kIconSize + UiScale(6), cell.right - 2, cell.bottom - UiScale(4)};
    RECT shadow = label;
    OffsetRect(&shadow, 1, 1);

    HGDIOBJ oldFont = SelectObject(dc, g_desktop.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, entry.title.c_str(), -1, &shadow,
              DT_CENTER | DT_END_ELLIPSIS | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, kUiText);
    DrawTextW(dc, entry.title.c_str(), -1, &label,
              DT_CENTER | DT_END_ELLIPSIS | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

// 右下角水印：跟常见 PE 一样，低调地标明系统身份
void PaintDesktopWatermark(HDC dc, int width, int height)
{
    const int bottom = height - kTaskbarHeight - UiScale(14);

    RECT titleRect{0, bottom - UiScale(44), width - UiScale(24), bottom - UiScale(20)};
    RECT titleShadow = titleRect;
    OffsetRect(&titleShadow, 1, 1);
    HGDIOBJ oldFont = SelectObject(dc, g_desktop.titleFont);
    SetBkMode(dc, TRANSPARENT);

    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, L"FireWin PE", -1, &titleShadow, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, RGB(236, 236, 241));
    DrawTextW(dc, L"FireWin PE", -1, &titleRect, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);

    RECT subRect{0, bottom - UiScale(22), width - UiScale(24), bottom};
    RECT subShadow = subRect;
    OffsetRect(&subShadow, 1, 1);
    SelectObject(dc, g_desktop.smallFont);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, L"系统维护工具箱 · 离线部署环境", -1, &subShadow, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, RGB(186, 186, 196));
    DrawTextW(dc, L"系统维护工具箱 · 离线部署环境", -1, &subRect, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, oldFont);
}

void PaintDesktopWindow(HWND window, HDC target)
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    HDC dc = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, buffer);

    PaintDesktopWallpaper(dc, width, height);
    for (size_t i = 0; i < g_desktop.icons.size(); ++i)
    {
        if (!DesktopCellOnScreen(static_cast<int>(i)))
        {
            continue;
        }

        PaintDesktopIcon(dc, static_cast<int>(i));
    }

    PaintDesktopWatermark(dc, width, height);

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);

    SelectObject(dc, oldBitmap);
    DeleteObject(buffer);
    DeleteDC(dc);
}

void PaintTaskbar(HWND window, HDC target)
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    HDC dc = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, buffer);

    UiFillRect(dc, client, kUiBg);
    RECT topLine{0, 0, width, 1};
    UiFillRect(dc, topLine, kUiBorder);

    // 开始按钮
    RECT startRect{UiScale(8), UiScale(6), UiScale(8) + kStartButtonWidth, kTaskbarHeight - UiScale(6)};
    const bool startHot = g_desktop.menuOpen || g_desktop.startButtonHover;
    UiRoundRect(dc, startRect, UiScale(8), startHot ? kUiHover : kUiCard, startHot ? kUiAccent : kUiBorder);

    if (g_desktop.appIcon != nullptr)
    {
        DrawIconEx(dc, startRect.left + UiScale(12), startRect.top + UiScale(8), g_desktop.appIcon,
                   UiScale(18), UiScale(18), 0, nullptr, DI_NORMAL);
    }

    RECT startText{startRect.left + UiScale(36), startRect.top, startRect.right - UiScale(6), startRect.bottom};
    UiText(dc, L"开始", startText, g_desktop.titleFont, kUiText,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // 运行中的窗口
    const HWND foreground = GetForegroundWindow();
    for (size_t i = 0; i < g_desktop.taskWindows.size(); ++i)
    {
        const HWND taskWindow = g_desktop.taskWindows[i];
        const RECT button = TaskButtonRect(static_cast<int>(i));
        if (button.right > width - kClockWidth)
        {
            break; // 空间不足时截断，避免压住时钟
        }

        const bool active = taskWindow == foreground;
        const bool hovered = g_desktop.taskHover == static_cast<int>(i);
        UiRoundRect(dc, button, 7, hovered ? kUiHover : kUiBg, hovered ? kUiBorder : kUiBg);

        // Win11 风格：运行中的窗口底部一条短的强调色下划线
        if (active)
        {
            const int centerX = (button.left + button.right) / 2;
            RECT indicator{centerX - 8, button.bottom - 4, centerX + 8, button.bottom - 1};
            UiRoundRect(dc, indicator, 2, kUiAccent, kUiAccent);
        }

        wchar_t title[160]{};
        GetWindowTextW(taskWindow, title, ARRAYSIZE(title));

        DWORD_PTR iconResult = 0;
        SendMessageTimeoutW(taskWindow, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 150, &iconResult);
        HICON icon = reinterpret_cast<HICON>(iconResult);
        if (icon == nullptr)
        {
            icon = reinterpret_cast<HICON>(GetClassLongPtrW(taskWindow, GCLP_HICONSM));
        }

        int textLeft = button.left + UiScale(10);
        if (icon != nullptr)
        {
            DrawIconEx(dc, button.left + UiScale(8), button.top + UiScale(6), icon, UiScale(16),
                       UiScale(16), 0, nullptr, DI_NORMAL);
            textLeft = button.left + UiScale(30);
        }

        RECT textRect{textLeft, button.top, button.right - UiScale(6), button.bottom};
        UiText(dc, title, textRect, g_desktop.font, (active || hovered) ? kUiText : kUiMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    // 时钟
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t clockText[32]{};
    swprintf_s(clockText, L"%02u:%02u", st.wHour, st.wMinute);

    static const wchar_t* const kWeekdays[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    wchar_t dateText[48]{};
    swprintf_s(dateText, L"%u月%u日 %s", st.wMonth, st.wDay, kWeekdays[st.wDayOfWeek % 7]);

    RECT clockRect{width - kClockWidth - 2, UiScale(3), width - UiScale(10), UiScale(25)};
    UiText(dc, clockText, clockRect, g_desktop.clockFont, kUiText,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT dateRect{width - kClockWidth - 2, UiScale(24), width - UiScale(10), kTaskbarHeight - UiScale(4)};
    UiText(dc, dateText, dateRect, g_desktop.smallFont, kUiMuted,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);

    SelectObject(dc, oldBitmap);
    DeleteObject(buffer);
    DeleteDC(dc);
}

void PaintStartMenu(HWND window, HDC target)
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    HDC dc = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, buffer);

    UiFillRect(dc, client, kUiCard);

    HBRUSH borderBrush = CreateSolidBrush(kUiBorder);
    FrameRect(dc, &client, borderBrush);
    DeleteObject(borderBrush);

    // 顶部品牌区
    if (g_desktop.appIcon != nullptr)
    {
        DrawIconEx(dc, UiScale(16), UiScale(14), g_desktop.appIcon, UiScale(26), UiScale(26),
                   0, nullptr, DI_NORMAL);
    }

    RECT titleRect{UiScale(52), UiScale(8), width - UiScale(12), UiScale(34)};
    UiText(dc, L"FireWin PE", titleRect, g_desktop.titleFont, kUiText,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT subtitleRect{UiScale(52), UiScale(28), width - UiScale(12), UiScale(50)};
    UiText(dc, L"系统维护与部署工具箱", subtitleRect, g_desktop.smallFont, kUiMuted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT headerLine{UiScale(12), kStartMenuHeader - UiScale(4), width - UiScale(12),
                    kStartMenuHeader - UiScale(3)};
    UiFillRect(dc, headerLine, kUiBorder);

    // 两列布局：左列 [0, menuSplit)，右列 [menuSplit, n)
    const int total = static_cast<int>(g_desktop.startItems.size());
    for (int column = 0; column < 2; ++column)
    {
        const int from = column == 0 ? 0 : g_desktop.menuSplit;
        const int to = column == 0 ? g_desktop.menuSplit : total;
        const int left = StartMenuColumnLeft(column);
        const int right = left + kStartMenuColumnWidth;

        int top = kStartMenuHeader;
        for (int i = from; i < to; ++i)
        {
            const DesktopEntry& entry = g_desktop.startItems[static_cast<size_t>(i)];

            // 分组标题：小号灰字，不可点击
            if (entry.isHeader)
            {
                RECT headerRect{left + UiScale(10), top, right - UiScale(6), top + g_desktop.menuRowHeight};
                UiText(dc, entry.title, headerRect, g_desktop.smallFont, kUiMuted,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                top += g_desktop.menuRowHeight;
                continue;
            }

            if (entry.title.empty())
            {
                RECT line{left + UiScale(10), top + kMenuSeparator / 2, right - UiScale(4),
                          top + kMenuSeparator / 2 + 1};
                UiFillRect(dc, line, kUiBorder);
                top += kMenuSeparator;
                continue;
            }

            const bool hovered = g_desktop.menuHover == i;
            RECT row{left, top, right, top + g_desktop.menuRowHeight};
            if (hovered)
            {
                UiRoundRect(dc, row, 6, kUiHover, kUiHover);
                RECT bar{row.left + 1, row.top + 7, row.left + 3, row.bottom - 7};
                UiFillRect(dc, bar, kUiAccent);
            }

            const int iconSize = UiScale(20);
            const int iconTop = top + (g_desktop.menuRowHeight - iconSize) / 2;
            RECT tile{left + UiScale(8), iconTop, left + UiScale(28), iconTop + iconSize};
            if (entry.icon != nullptr)
            {
                DrawIconEx(dc, tile.left + UiScale(2), tile.top + UiScale(2), entry.icon,
                           UiScale(16), UiScale(16), 0, nullptr, DI_NORMAL);
            }
            else
            {
                PaintGlyphTile(dc, tile, UiScale(5));
                RECT glyphBox{tile.left + UiScale(3), tile.top + UiScale(3), tile.right - UiScale(3),
                              tile.bottom - UiScale(3)};
                PaintVectorGlyph(dc, glyphBox, entry.glyph);
            }

            RECT textRect{left + UiScale(36), top, right - UiScale(6), top + g_desktop.menuRowHeight};
            UiText(dc, entry.title, textRect, g_desktop.font, kUiText,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            top += g_desktop.menuRowHeight;
        }
    }

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);

    SelectObject(dc, oldBitmap);
    DeleteObject(buffer);
    DeleteDC(dc);
}

// ---------------------------------------------------------------------------
// 开始菜单 / 右键菜单
// ---------------------------------------------------------------------------
void ShowStartMenu(bool show)
{
    if (g_desktop.startMenu == nullptr)
    {
        return;
    }

    g_desktop.menuOpen = show;
    g_desktop.menuHover = kDesktopHitNone;

    if (show)
    {
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        const int y = screenHeight - kTaskbarHeight - g_desktop.menuHeight - UiScale(6);
        SetWindowPos(g_desktop.startMenu, HWND_TOPMOST, UiScale(8), y, kStartMenuWidth, g_desktop.menuHeight,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);

        // 菜单高度随条目变化，每次显示时重新套圆角区域，避免出现直角边
        SetWindowRgn(g_desktop.startMenu,
                     CreateRoundRectRgn(0, 0, kStartMenuWidth + 1, g_desktop.menuHeight + 1,
                                        UiScale(14), UiScale(14)), TRUE);
        InvalidateRect(g_desktop.startMenu, nullptr, FALSE);
    }
    else
    {
        ShowWindow(g_desktop.startMenu, SW_HIDE);
    }

    if (g_desktop.taskbar != nullptr)
    {
        InvalidateRect(g_desktop.taskbar, nullptr, FALSE);
    }
}

void ShowDesktopContextMenu(int x, int y)
{
    static const struct
    {
        const wchar_t* text;
        int command;
    } kItems[] = {
        {L"刷新桌面", kDesktopRefresh},
        {nullptr, 0},
        {L"安装系统…", kCommandInstall},
        {L"加载驱动…", kCommandLoadDriver},
        {L"设置分辨率…", kCommandDisplayMode},
        {L"网络初始化", kCommandInitNetwork},
        {L"经典启动器", kDesktopOpenLauncher},
        {nullptr, 0},
        {L"重启", kCommandReboot},
        {L"关机", kCommandShutdown},
    };

    POINT point{x, y};
    ClientToScreen(g_desktop.window, &point);

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    for (const auto& item : kItems)
    {
        if (item.text == nullptr)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        else
        {
            AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(item.command), item.text);
        }
    }

    const int command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y,
                                         g_desktop.window, nullptr);
    DestroyMenu(menu);

    if (command != 0)
    {
        DesktopEntry entry;
        entry.actionId = command;
        LaunchDesktopEntry(entry);
    }
}

void ShowTaskContextMenu(int index, int x, int y)
{
    if (index < 0 || index >= static_cast<int>(g_desktop.taskWindows.size()))
    {
        return;
    }

    POINT point{x, y};
    ClientToScreen(g_desktop.taskbar, &point);

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(kDesktopCloseWindow), L"关闭窗口");
    const int command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y,
                                         g_desktop.taskbar, nullptr);
    DestroyMenu(menu);

    if (command == kDesktopCloseWindow)
    {
        PostMessageW(g_desktop.taskWindows[static_cast<size_t>(index)], WM_CLOSE, 0, 0);
        RefreshTaskWindows();
        InvalidateRect(g_desktop.taskbar, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK DesktopProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintDesktopWindow(window, dc);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        if (g_desktop.menuOpen)
        {
            ShowStartMenu(false);
            return 0;
        }

        const int index = DesktopHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (index != g_desktop.selected)
        {
            g_desktop.selected = index;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
        ShowStartMenu(false);
        const int index = DesktopHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (index >= 0)
        {
            g_desktop.selected = index;
            LaunchDesktopEntry(g_desktop.icons[static_cast<size_t>(index)]);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        const int index = DesktopHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (index != g_desktop.hover)
        {
            g_desktop.hover = index;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }

    case WM_RBUTTONUP:
        ShowStartMenu(false);
        ShowDesktopContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_F5)
        {
            DesktopEntry refreshEntry;
            refreshEntry.actionId = kDesktopRefresh;
            LaunchDesktopEntry(refreshEntry);
        }
        else if (wParam == VK_RETURN && g_desktop.selected >= 0 &&
                 g_desktop.selected < static_cast<int>(g_desktop.icons.size()))
        {
            LaunchDesktopEntry(g_desktop.icons[static_cast<size_t>(g_desktop.selected)]);
        }
        else if (wParam == VK_ESCAPE)
        {
            ShowStartMenu(false);
        }
        return 0;

    case kMessageDeployFinished:
        // 部署线程结束：复位状态并回收线程，避免退出时 joinable 崩溃
        g_launcher.deploying = false;
        if (g_launcher.deployThread.joinable())
        {
            g_launcher.deployThread.join();
        }
        g_launcher.manualConfig = nullptr;
        return 0;

    case WM_CLOSE:
        if (g_launcher.deploying)
        {
            MessageBoxW(window, L"正在部署，请等待完成后再关闭桌面。", kLauncherTitle,
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        if (MessageBoxW(window, L"确定要关闭桌面并退出 FireWin PE 吗？", kLauncherTitle,
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        {
            return 0;
        }

        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (g_desktop.startMenu != nullptr)
        {
            DestroyWindow(g_desktop.startMenu);
            g_desktop.startMenu = nullptr;
        }

        if (g_desktop.taskbar != nullptr)
        {
            DestroyWindow(g_desktop.taskbar);
            g_desktop.taskbar = nullptr;
        }

        ClearEntries(g_desktop.icons);
        ClearEntries(g_desktop.startItems);

        if (g_desktop.background != nullptr)
        {
            DeleteObject(g_desktop.background);
            g_desktop.background = nullptr;
        }

        if (g_desktop.font != nullptr)
        {
            DeleteObject(g_desktop.font);
            g_desktop.font = nullptr;
        }

        if (g_desktop.titleFont != nullptr)
        {
            DeleteObject(g_desktop.titleFont);
            g_desktop.titleFont = nullptr;
        }

        if (g_desktop.smallFont != nullptr)
        {
            DeleteObject(g_desktop.smallFont);
            g_desktop.smallFont = nullptr;
        }

        if (g_desktop.clockFont != nullptr)
        {
            DeleteObject(g_desktop.clockFont);
            g_desktop.clockFont = nullptr;
        }

        if (g_launcher.font != nullptr)
        {
            DeleteObject(g_launcher.font);
            g_launcher.font = nullptr;
        }

        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK TaskbarProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        SetTimer(window, kTaskbarTimerId, 1000, nullptr);
        RefreshTaskWindows();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintTaskbar(window, dc);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_TIMER:
        if (wParam == kTaskbarTimerId)
        {
            RefreshTaskWindows();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const bool startHover = x < 8 + kStartButtonWidth;
        const int taskIndex = startHover ? kDesktopHitNone : TaskButtonHitTest(x, y);
        if (startHover != g_desktop.startButtonHover || taskIndex != g_desktop.taskHover)
        {
            g_desktop.startButtonHover = startHover;
            g_desktop.taskHover = taskIndex;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        if (x < 8 + kStartButtonWidth)
        {
            ShowStartMenu(!g_desktop.menuOpen);
            return 0;
        }

        const int index = TaskButtonHitTest(x, y);
        if (index >= 0)
        {
            ShowStartMenu(false);
            HWND target = g_desktop.taskWindows[static_cast<size_t>(index)];
            if (IsIconic(target))
            {
                ShowWindow(target, SW_RESTORE);
            }
            SetForegroundWindow(target);
        }
        return 0;
    }

    case WM_RBUTTONUP:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (x >= 8 + kStartButtonWidth)
        {
            ShowTaskContextMenu(TaskButtonHitTest(x, y), x, y);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(window, kTaskbarTimerId);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK StartMenuProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);

    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintStartMenu(window, dc);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        const int index = StartMenuHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (index != g_desktop.menuHover)
        {
            g_desktop.menuHover = index;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const int index = StartMenuHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (index >= 0)
        {
            // 先拷贝条目：刷新桌面时会重建列表并销毁旧图标
            const DesktopEntry entry = g_desktop.startItems[static_cast<size_t>(index)];
            ShowStartMenu(false);
            LaunchDesktopEntry(entry);
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void RegisterDesktopClass(const wchar_t* name, WNDPROC procedure, UINT style)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = static_cast<UINT>(sizeof(windowClass));
    windowClass.style = style;
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = name;
    RegisterClassExW(&windowClass); // 已注册时忽略失败
}

// ---------------------------------------------------------------------------
// 启动加载页：全屏品牌页 + 后台初始化（自动分辨率 / 驱动加载）
// ---------------------------------------------------------------------------
constexpr wchar_t kSplashClass[] = L"FireWinPESplash";
constexpr UINT kSplashMessageRefresh = WM_APP + 101;
constexpr int kSplashTimerId = 7;
constexpr ULONGLONG kSplashTimeoutMs = 60000;

// 32 位 BMP（含 alpha）从 RCDATA 载入为 DIB，读完手动预乘 alpha，
// 这样 AlphaBlend 用 AC_SRC_ALPHA 就能得到干净的透明边缘。
// 不用 LoadImage 是因为它对 32 位资源图的 alpha 处理随系统而异，容易画出黑框。
HBITMAP LoadAlphaBitmapFromResource(HINSTANCE instance, int resourceId)
{
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (resource == nullptr)
    {
        return nullptr;
    }

    const DWORD size = SizeofResource(instance, resource);
    HGLOBAL loaded = LoadResource(instance, resource);
    const BYTE* data = loaded == nullptr ? nullptr : static_cast<const BYTE*>(LockResource(loaded));
    if (data == nullptr || size < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))
    {
        return nullptr;
    }

    BITMAPFILEHEADER fileHeader{};
    memcpy(&fileHeader, data, sizeof(fileHeader));
    BITMAPINFOHEADER info{};
    memcpy(&info, data + sizeof(fileHeader), sizeof(info));
    if (fileHeader.bfType != 0x4D42 || info.biBitCount != 32 || info.biCompression != BI_RGB ||
        info.biWidth <= 0 || info.biHeight == 0)
    {
        return nullptr;
    }

    const int width = info.biWidth;
    const int height = info.biHeight < 0 ? -info.biHeight : info.biHeight;
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    if (fileHeader.bfOffBits > size || rowBytes * static_cast<size_t>(height) > size - fileHeader.bfOffBits)
    {
        return nullptr;
    }

    BITMAPINFO dib{};
    dib.bmiHeader = info;
    dib.bmiHeader.biHeight = -height;  // 统一转成自上而下，方便逐行拷贝
    dib.bmiHeader.biSizeImage = static_cast<DWORD>(rowBytes * static_cast<size_t>(height));

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &dib, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr)
    {
        if (bitmap != nullptr)
        {
            DeleteObject(bitmap);
        }
        return nullptr;
    }

    const BYTE* source = data + fileHeader.bfOffBits;
    BYTE* target = static_cast<BYTE*>(pixels);
    for (int y = 0; y < height; ++y)
    {
        // BMP 默认自下而上存储，负高度才是自上而下
        const int sourceRow = info.biHeight > 0 ? height - 1 - y : y;
        memcpy(target + static_cast<size_t>(y) * rowBytes, source + static_cast<size_t>(sourceRow) * rowBytes, rowBytes);
    }

    for (size_t i = 0; i < rowBytes * static_cast<size_t>(height); i += 4)
    {
        const unsigned alpha = target[i + 3];
        if (alpha == 255)
        {
            continue;
        }

        target[i] = static_cast<BYTE>(target[i] * alpha / 255);
        target[i + 1] = static_cast<BYTE>(target[i + 1] * alpha / 255);
        target[i + 2] = static_cast<BYTE>(target[i + 2] * alpha / 255);
    }

    return bitmap;
}

struct SplashState
{
    std::mutex mutex;              // 保护 step 文本
    std::wstring step = L"正在准备运行环境…";
    std::atomic<int> percent{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> skipped{false};
    std::atomic<HWND> window{nullptr};
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;
    HBITMAP wallpaper = nullptr;
    HBITMAP logo = nullptr;
    HICON icon = nullptr;
};

SplashState g_splash;

void SplashSetProgress(int percent, const std::wstring& step)
{
    {
        std::lock_guard<std::mutex> lock(g_splash.mutex);
        g_splash.percent.store(std::max(0, std::min(100, percent)));
        if (!step.empty())
        {
            g_splash.step = step;
        }
    }

    if (HWND window = g_splash.window.load())
    {
        PostMessageW(window, kSplashMessageRefresh, 0, 0);
    }
}

// 尝试切换到指定分辨率；枚举到的模式必须真实存在
bool TryApplyDisplayMode(int width, int height)
{
    DEVMODEW probe{};
    probe.dmSize = sizeof(probe);

    for (int i = 0; EnumDisplaySettingsW(nullptr, static_cast<DWORD>(i), &probe); ++i)
    {
        if (probe.dmBitsPerPel < 32 || probe.dmPelsWidth != static_cast<DWORD>(width) ||
            probe.dmPelsHeight != static_cast<DWORD>(height))
        {
            continue;
        }

        DEVMODEW mode = probe;
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        if (ChangeDisplaySettingsW(&mode, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL)
        {
            return true;
        }
    }

    return false;
}

// 自动分辨率：优先常见高清档位，其次最大面积，最后兜底 1024x768 / 800x600
bool ApplyBestDisplayMode()
{
    const struct
    {
        int width;
        int height;
    } kPreferred[] = {{1920, 1080}, {2560, 1440}, {1600, 900}, {1366, 768}, {1280, 720}};

    for (const auto& mode : kPreferred)
    {
        if (TryApplyDisplayMode(mode.width, mode.height))
        {
            return true;
        }
    }

    DEVMODEW probe{};
    probe.dmSize = sizeof(probe);
    int bestWidth = 0;
    int bestHeight = 0;
    long bestArea = 0;
    for (int i = 0; EnumDisplaySettingsW(nullptr, static_cast<DWORD>(i), &probe); ++i)
    {
        if (probe.dmBitsPerPel < 32 || probe.dmPelsWidth < 1024 || probe.dmPelsHeight < 600)
        {
            continue;
        }

        const long area = static_cast<long>(probe.dmPelsWidth) * static_cast<long>(probe.dmPelsHeight);
        if (area > bestArea)
        {
            bestArea = area;
            bestWidth = static_cast<int>(probe.dmPelsWidth);
            bestHeight = static_cast<int>(probe.dmPelsHeight);
        }
    }

    if (bestWidth > 0 && TryApplyDisplayMode(bestWidth, bestHeight))
    {
        return true;
    }

    return TryApplyDisplayMode(1024, 768) || TryApplyDisplayMode(800, 600);
}

// ---------------------------------------------------------------------------
// 「设置分辨率」小工具
// WinPE 没有显示设置面板，这里模仿微PE/大白菜：列出驱动支持的所有模式，点应用立刻切换
// ---------------------------------------------------------------------------
struct DisplayModeItem
{
    int width = 0;
    int height = 0;
    int refresh = 0;
};

HWND g_displayDialog = nullptr;
HWND g_displayList = nullptr;
HWND g_displayApply = nullptr;
std::vector<DisplayModeItem> g_displayModes;
bool g_displayChanged = false;
constexpr int kDisplayListId = 3001;

// 去重后按面积从大到小排序（同面积按宽度）
std::vector<DisplayModeItem> EnumerateDisplayModes()
{
    std::vector<DisplayModeItem> modes;

    DEVMODEW probe{};
    probe.dmSize = sizeof(probe);
    for (int i = 0; EnumDisplaySettingsW(nullptr, static_cast<DWORD>(i), &probe); ++i)
    {
        if (probe.dmBitsPerPel < 32 || probe.dmPelsWidth < 800 || probe.dmPelsHeight < 600)
        {
            continue;
        }

        const int width = static_cast<int>(probe.dmPelsWidth);
        const int height = static_cast<int>(probe.dmPelsHeight);
        const int refresh = static_cast<int>(probe.dmDisplayFrequency);

        auto existing = std::find_if(modes.begin(), modes.end(), [width, height](const DisplayModeItem& item) {
            return item.width == width && item.height == height;
        });
        if (existing == modes.end())
        {
            modes.push_back({width, height, refresh});
        }
        else if (refresh > existing->refresh)
        {
            existing->refresh = refresh;
        }
    }

    std::sort(modes.begin(), modes.end(), [](const DisplayModeItem& a, const DisplayModeItem& b) {
        const long areaA = static_cast<long>(a.width) * a.height;
        const long areaB = static_cast<long>(b.width) * b.height;
        if (areaA != areaB)
        {
            return areaA > areaB;
        }
        return a.width > b.width;
    });

    return modes;
}

// 分辨率变了以后，重新算缩放比例并重排桌面/任务栏/开始菜单（或经典启动器）
void RelayoutAfterDisplayChange()
{
    UpdateUiScale();
    InitUiMetrics();

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    if (g_desktop.window != nullptr)
    {
        if (g_desktop.menuOpen)
        {
            ShowStartMenu(false);
        }

        // 字体高度依赖缩放比例，必须按新比例重建
        for (HFONT* font : {&g_desktop.font, &g_desktop.titleFont, &g_desktop.smallFont, &g_desktop.clockFont})
        {
            if (*font != nullptr)
            {
                DeleteObject(*font);
                *font = nullptr;
            }
        }
        g_desktop.font = CreateDesktopFont(-13, FW_NORMAL);
        g_desktop.titleFont = CreateDesktopFont(-15, FW_SEMIBOLD);
        g_desktop.smallFont = CreateDesktopFont(-11, FW_NORMAL);
        g_desktop.clockFont = CreateDesktopFont(-15, FW_SEMIBOLD);

        const int usableHeight = std::max(kCellHeight, screenHeight - kTaskbarHeight - kDesktopMargin);
        g_desktop.rows = std::max(1, usableHeight / kCellHeight);
        g_desktop.selected = kDesktopHitNone;
        g_desktop.hover = kDesktopHitNone;
        g_desktop.menuHover = kDesktopHitNone;

        BuildStartItems(screenHeight);

        SetWindowPos(g_desktop.window, nullptr, 0, 0, screenWidth, screenHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_desktop.taskbar != nullptr)
        {
            SetWindowPos(g_desktop.taskbar, HWND_TOPMOST, 0, screenHeight - kTaskbarHeight, screenWidth,
                         kTaskbarHeight, SWP_NOACTIVATE);
            InvalidateRect(g_desktop.taskbar, nullptr, TRUE);
        }
        if (g_desktop.startMenu != nullptr)
        {
            SetWindowRgn(g_desktop.startMenu,
                         CreateRoundRectRgn(0, 0, kStartMenuWidth + 1, g_desktop.menuHeight + 1,
                                            UiScale(14), UiScale(14)), TRUE);
        }
        InvalidateRect(g_desktop.window, nullptr, TRUE);
        UpdateWindow(g_desktop.window);
        return;
    }

    if (g_launcher.window == nullptr || g_launcher.deployThread.joinable())
    {
        return;
    }

    // 经典启动器模式：控件位置都是按缩放算好的，直接销毁重建最省事
    for (HWND child = GetWindow(g_launcher.window, GW_CHILD); child != nullptr;)
    {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        DestroyWindow(child);
        child = next;
    }

    g_launcher.statusText = nullptr;
    g_launcher.deployButton = nullptr;
    g_launcher.installButton = nullptr;
    if (g_launcher.font != nullptr)
    {
        DeleteObject(g_launcher.font);
        g_launcher.font = nullptr;
    }

    g_launcher.clientHeight = ComputeLauncherClientHeight(!g_launcher.configPath.empty(),
                                                          g_launcher.appTools.size());
    CreateLauncherControls(g_launcher.window);

    const int width = g_launcher.clientWidth;
    const int height = g_launcher.clientHeight;
    SetWindowPos(g_launcher.window, nullptr, (screenWidth - width) / 2, (screenHeight - height) / 2,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowRgn(g_launcher.window,
                 CreateRoundRectRgn(0, 0, width + 1, height + 1, kLauncherCorner, kLauncherCorner), TRUE);
    InvalidateRect(g_launcher.window, nullptr, TRUE);
}

void ApplySelectedDisplayMode(HWND dialog)
{
    const int index = static_cast<int>(SendMessageW(g_displayList, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_displayModes.size()))
    {
        MessageBoxW(dialog, L"请先在列表里选择一个分辨率。", L"设置分辨率", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const DisplayModeItem mode = g_displayModes[static_cast<size_t>(index)];
    if (mode.width == GetSystemMetrics(SM_CXSCREEN) && mode.height == GetSystemMetrics(SM_CYSCREEN))
    {
        DestroyWindow(dialog);
        return;
    }

    if (!TryApplyDisplayMode(mode.width, mode.height))
    {
        wchar_t text[160]{};
        swprintf_s(text, L"切换到 %d × %d 失败，当前分辨率保持不变。", mode.width, mode.height);
        MessageBoxW(dialog, text, L"设置分辨率", MB_OK | MB_ICONWARNING);
        return;
    }

    g_displayChanged = true;
    DestroyWindow(dialog);
}

LRESULT CALLBACK DisplayDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        RECT client{};
        GetClientRect(window, &client);

        const int margin = UiScale(16);
        const int gap = UiScale(10);
        const int buttonHeight = UiScale(32);
        const int buttonWidth = UiScale(96);
        const int infoHeight = UiScale(22);

        wchar_t caption[160]{};
        swprintf_s(caption, L"当前分辨率：%d × %d", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        CreateWindowExW(0, L"STATIC", caption, WS_CHILD | WS_VISIBLE | SS_LEFT,
                        margin, margin, client.right - margin * 2, infoHeight,
                        window, nullptr, GetModuleHandleW(nullptr), nullptr);

        const int listTop = margin + infoHeight + UiScale(8);
        const int listHeight = std::max(UiScale(90),
                                        static_cast<int>(client.bottom) - listTop - buttonHeight - gap * 2 - margin);
        g_displayList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY,
                                        margin, listTop, client.right - margin * 2, listHeight,
                                        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDisplayListId)),
                                        GetModuleHandleW(nullptr), nullptr);

        const int buttonsTop = listTop + listHeight + gap + UiScale(4);
        g_displayApply = CreateWindowExW(0, L"BUTTON", L"应用",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                         client.right - margin - buttonWidth * 2 - gap, buttonsTop,
                                         buttonWidth, buttonHeight,
                                         window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                         GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        client.right - margin - buttonWidth, buttonsTop, buttonWidth, buttonHeight,
                        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                        GetModuleHandleW(nullptr), nullptr);

        HFONT font = CreateDesktopFont(-14, FW_NORMAL);
        for (HWND child = GetWindow(window, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
        {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        // 字体随窗口一起释放
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(font));

        const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        int currentIndex = 0;
        for (size_t i = 0; i < g_displayModes.size(); ++i)
        {
            const auto& mode = g_displayModes[i];
            wchar_t text[64]{};
            swprintf_s(text, L"%d × %d    (%d Hz)", mode.width, mode.height, mode.refresh);
            SendMessageW(g_displayList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));

            if (mode.width == screenWidth && mode.height == screenHeight)
            {
                currentIndex = static_cast<int>(i);
            }
        }
        SendMessageW(g_displayList, LB_SETCURSEL, currentIndex, 0);
        SetFocus(g_displayList);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            ApplySelectedDisplayMode(window);
            return 0;
        case IDCANCEL:
            DestroyWindow(window);
            return 0;
        case kDisplayListId:
            if (HIWORD(wParam) == LBN_DBLCLK)
            {
                ApplySelectedDisplayMode(window);
                return 0;
            }
            break;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
    {
        HFONT font = reinterpret_cast<HFONT>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (font != nullptr)
        {
            DeleteObject(font);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        g_displayDialog = nullptr;
        g_displayList = nullptr;
        g_displayApply = nullptr;
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowDisplayModeDialog(HWND owner)
{
    if (g_displayDialog != nullptr)
    {
        SetForegroundWindow(g_displayDialog);
        return;
    }

    g_displayModes = EnumerateDisplayModes();
    if (g_displayModes.empty())
    {
        MessageBoxW(owner, L"没有枚举到可用的显示模式，显示驱动可能只提供当前分辨率。",
                    L"设置分辨率", MB_OK | MB_ICONWARNING);
        return;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    static bool classRegistered = false;
    if (!classRegistered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = DisplayDialogProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = CreateSolidBrush(RGB(32, 34, 38));
        windowClass.lpszClassName = L"FireWinPEDisplayMode";
        classRegistered = RegisterClassExW(&windowClass) != 0;
        if (!classRegistered)
        {
            return;
        }
    }

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_CONTROLPARENT;
    RECT frame{0, 0, UiScale(440), UiScale(400)};
    AdjustWindowRectEx(&frame, style, FALSE, exStyle);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;

    int centerX = GetSystemMetrics(SM_CXSCREEN) / 2;
    int centerY = GetSystemMetrics(SM_CYSCREEN) / 2;
    if (owner != nullptr && IsWindow(owner))
    {
        RECT ownerRect{};
        GetWindowRect(owner, &ownerRect);
        centerX = (ownerRect.left + ownerRect.right) / 2;
        centerY = (ownerRect.top + ownerRect.bottom) / 2;
    }

    HWND dialog = CreateWindowExW(exStyle, L"FireWinPEDisplayMode", L"设置分辨率",
                                  style, centerX - width / 2, centerY - height / 2, width, height,
                                  owner, nullptr, instance, nullptr);
    if (dialog == nullptr)
    {
        return;
    }

    g_displayDialog = dialog;
    if (owner != nullptr && IsWindow(owner))
    {
        EnableWindow(owner, FALSE);
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    MSG message{};
    while (g_displayDialog != nullptr && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(dialog, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (owner != nullptr && IsWindow(owner))
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }

    g_displayModes.clear();

    if (g_displayChanged)
    {
        g_displayChanged = false;
        RelayoutAfterDisplayChange();
    }
}

// ---------------------------------------------------------------------------
// 虚拟机检测：CPUID 0x40000000 返回的是 hypervisor 自报的厂商字符串
// ---------------------------------------------------------------------------
enum class HypervisorKind
{
    Physical,
    VMware,
    HyperV,
    VirtualBox,
    Kvm,
};

HypervisorKind DetectHypervisor()
{
    int regs[4]{};
    __cpuid(regs, 0x40000000);

    char vendor[13]{};
    std::memcpy(vendor, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[2], 4);
    std::memcpy(vendor + 8, &regs[3], 4);

    const std::string name(vendor);
    if (name == "VMwareVMware")
    {
        return HypervisorKind::VMware;
    }

    if (name == "Microsoft Hv")
    {
        return HypervisorKind::HyperV;
    }

    if (name == "VBoxVBoxVBox")
    {
        return HypervisorKind::VirtualBox;
    }

    if (name == "KVMKVMKVM")
    {
        return HypervisorKind::Kvm;
    }

    return HypervisorKind::Physical;
}

const wchar_t* HypervisorName(HypervisorKind kind)
{
    switch (kind)
    {
    case HypervisorKind::VMware: return L"VMware";
    case HypervisorKind::HyperV: return L"Hyper-V";
    case HypervisorKind::VirtualBox: return L"VirtualBox";
    case HypervisorKind::Kvm: return L"KVM";
    default: return L"物理机";
    }
}

// 记录启动环境，PE 里出问题时让用户把这个日志发回来即可定位
void AppendBootLog(const std::wstring& line)
{
    wchar_t tempPath[MAX_PATH]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
    if (length == 0 || length >= ARRAYSIZE(tempPath))
    {
        return;
    }

    std::ofstream file(WideToUtf8(JoinPath(std::wstring(tempPath, length), L"FireWinPE-boot.log")),
                       std::ios::out | std::ios::app | std::ios::binary);
    if (!file.is_open())
    {
        return;
    }

    const std::string utf8 = WideToUtf8(line + L"\r\n");
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

// ---------------------------------------------------------------------------
// 镜像自带驱动的检索：DriverStore\FileRepository 里「包目录名 + 包内 INF」
// ---------------------------------------------------------------------------
struct DriverStorePackage
{
    std::wstring name;    // 包目录名，如 lsi_sas.inf_amd64_74bb5e3e01cfd526
    std::wstring infPath; // 包内 INF 全路径
};

std::vector<DriverStorePackage> EnumerateDriverStorePackages()
{
    std::vector<DriverStorePackage> packages;
    const std::wstring repository = JoinPath(GetSystemDirectoryPath(), L"DriverStore\\FileRepository");

    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(repository, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return packages;
    }

    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || data.cFileName[0] == L'.')
        {
            continue;
        }

        const std::wstring packageDir = JoinPath(repository, data.cFileName);
        WIN32_FIND_DATAW infData{};
        HANDLE infFind = FindFirstFileW(JoinPath(packageDir, L"*.inf").c_str(), &infData);
        if (infFind == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        do
        {
            packages.push_back({data.cFileName, JoinPath(packageDir, infData.cFileName)});
        } while (FindNextFileW(infFind, &infData));
        FindClose(infFind);
    } while (FindNextFileW(find, &data));
    FindClose(find);

    return packages;
}

// 按 token 顺序取包，ialpss2i_gpio2 / ialpss2i_i2c 这类前缀会命中多个芯片组版本
std::vector<std::wstring> SelectInboxInfs(const std::vector<DriverStorePackage>& packages,
                                          const wchar_t* const* tokens, size_t tokenCount)
{
    std::vector<std::wstring> results;
    for (size_t i = 0; i < tokenCount; ++i)
    {
        const std::wstring lowered = ToLower(tokens[i]);
        for (const DriverStorePackage& package : packages)
        {
            if (ToLower(package.name).find(lowered) == std::wstring::npos)
            {
                continue;
            }

            if (std::find(results.begin(), results.end(), package.infPath) == results.end())
            {
                results.push_back(package.infPath);
            }
        }
    }

    return results;
}

// 开机自动加载的镜像内置驱动，顺序即优先级：
//   存储控制器（VMware PVSCSI / LSI SAS、NVMe、Intel RST、AMD SATA）
//   → 通用触控板（GPIO → I2C → HID）
//   → 虚拟机网卡（仅虚拟机环境）
// 存储类必须先于磁盘扫描加载，否则 VMware 的虚拟磁盘在安装向导里看不到。
std::vector<std::wstring> CollectInboxDriverInfs(HypervisorKind hypervisor)
{
    static const wchar_t* const kStorage[] = {
        L"pvscsii.inf",   // VMware 半虚拟化 SCSI
        L"lsi_sas.inf",   // VMware 默认的 LSI Logic SAS
        L"lsi_sas2i", L"lsi_sas3i",
        L"stornvme.inf", L"nvmedisk.inf", // VMware / 实体机 NVMe
        L"iastorav.inf", L"amdsata.inf",  // Intel RST（VMD/RAID）、AMD SATA
    };

    // 触控板依赖链：GPIO 控制器（中断线）→ I2C 总线 → I2C HID → HID 类过滤
    static const wchar_t* const kHid[] = {
        L"iagpio.inf", L"ialpss2i_gpio2", L"ialpssi_gpio.inf", L"amdgpio2.inf",
        L"ialpss2i_i2c", L"iai2c.inf", L"amdi2c.inf",
        L"hidi2c.inf", L"hidspi_km.inf", L"hidinterrupt.inf", L"c_hidclass.inf",
    };

    // 虚拟机网卡：vmxnet3 需要 VMware Tools 的驱动（镜像里通常没有），e1000e 是内置的
    static const wchar_t* const kVirtualNet[] = {
        L"vmxnet3", L"nete1e3e.inf", L"netefe3e.inf", L"nete1g3e.inf", L"net1ix64.inf",
    };

    const std::vector<DriverStorePackage> packages = EnumerateDriverStorePackages();

    std::vector<std::wstring> infs = SelectInboxInfs(packages, kStorage, ARRAYSIZE(kStorage));

    const std::vector<std::wstring> hid = SelectInboxInfs(packages, kHid, ARRAYSIZE(kHid));
    infs.insert(infs.end(), hid.begin(), hid.end());

    if (hypervisor != HypervisorKind::Physical)
    {
        const std::vector<std::wstring> net = SelectInboxInfs(packages, kVirtualNet, ARRAYSIZE(kVirtualNet));
        infs.insert(infs.end(), net.begin(), net.end());
    }

    return infs;
}

// 需要开机自动加载的驱动：X:\Tools\Drivers + 各分区 \Drivers 与 \FWIMG\Drivers
std::vector<std::wstring> CollectBootDriverInfs()
{
    std::vector<std::wstring> results;
    CollectInfFiles(JoinPath(GetExeDirectory(), L"Drivers"), 0, results);

    wchar_t buffer[512]{};
    const DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(buffer) - 1, buffer);
    if (length != 0 && length < ARRAYSIZE(buffer))
    {
        for (const wchar_t* cursor = buffer; *cursor != L'\0'; cursor += lstrlenW(cursor) + 1)
        {
            const std::wstring root(cursor);
            if (root.size() < 3)
            {
                continue;
            }

            CollectInfFiles(root.substr(0, 2) + L"\\Drivers", 0, results);
            CollectInfFiles(root.substr(0, 2) + L"\\FWIMG\\Drivers", 0, results);
        }
    }

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
}

// 后台线程：设置分辨率 → 扫描驱动 → drvload 逐个加载（失败只记录，不影响启动）
void InitializePeBackground()
{
    const ULONGLONG deadline = GetTickCount64() + kSplashTimeoutMs;
    const HypervisorKind hypervisor = DetectHypervisor();

    SplashSetProgress(6, L"正在设置最佳显示分辨率…");
    ApplyBestDisplayMode();

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    AppendBootLog(std::wstring(L"运行环境：") + HypervisorName(hypervisor) + L"，分辨率 " +
                  std::to_wstring(screenWidth) + L"x" + std::to_wstring(screenHeight));

    const bool virtualMachine = hypervisor != HypervisorKind::Physical;
    SplashSetProgress(14, virtualMachine
                              ? std::wstring(L"正在加载 ") + HypervisorName(hypervisor) + L" 虚拟机驱动…"
                              : L"正在加载磁盘与触控板驱动…");

    // 先加载镜像自带的存储 / HID 驱动栈（虚拟机会追加网卡），再加载 U 盘上补充的驱动
    std::vector<std::wstring> infs = CollectInboxDriverInfs(hypervisor);
    const std::vector<std::wstring> extraInfs = CollectBootDriverInfs();
    infs.insert(infs.end(), extraInfs.begin(), extraInfs.end());

    size_t loaded = 0;
    const std::wstring drvload = JoinPath(GetSystemDirectoryPath(), L"drvload.exe");
    if (!infs.empty() && FileExists(drvload))
    {
        for (size_t i = 0; i < infs.size(); ++i)
        {
            if (g_splash.skipped.load() || GetTickCount64() > deadline)
            {
                break;
            }

            DWORD exitCode = 0;
            std::wstring output;
            RunCapture(drvload, L"\"" + infs[i] + L"\"", exitCode, output);
            if (exitCode == 0)
            {
                ++loaded;
            }

            const int percent = 14 + static_cast<int>((i + 1) * 80 / infs.size());
            SplashSetProgress(percent, L"正在加载驱动（" + std::to_wstring(i + 1) + L"/" +
                                           std::to_wstring(infs.size()) + L"）…");
        }
    }

    AppendBootLog(L"驱动加载：" + std::to_wstring(loaded) + L"/" + std::to_wstring(infs.size()) + L" 成功");

    SplashSetProgress(100, L"环境准备完成");
    Sleep(250);
    g_splash.finished.store(true);
    if (HWND window = g_splash.window.load())
    {
        PostMessageW(window, kSplashMessageRefresh, 0, 0);
    }
}

void PaintSplash(HWND window, HDC target)
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    HDC dc = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, buffer);

    // 壁纸压暗，保证白字对比度
    PaintImageCover(dc, g_splash.wallpaper, width, height, 120);

    std::wstring step;
    int percent = 0;
    {
        std::lock_guard<std::mutex> lock(g_splash.mutex);
        step = g_splash.step;
        percent = g_splash.percent.load();
    }

    const int centerX = width / 2;
    if (g_splash.logo != nullptr)
    {
        BITMAP bitmap{};
        if (GetObjectW(g_splash.logo, sizeof(bitmap), &bitmap) != 0 && bitmap.bmWidth > 0 && bitmap.bmHeight > 0)
        {
            const int logoWidth = std::min(width * 36 / 100, UiScale(460));
            const int logoHeight = std::max<int>(1, logoWidth * static_cast<int>(bitmap.bmHeight) /
                                                        static_cast<int>(bitmap.bmWidth));
            const int logoTop = height / 2 - UiScale(96) - logoHeight;

            HDC source = CreateCompatibleDC(dc);
            if (source != nullptr)
            {
                HGDIOBJ previousBitmap = SelectObject(source, g_splash.logo);
                BLENDFUNCTION blend{};
                blend.BlendOp = AC_SRC_OVER;
                blend.SourceConstantAlpha = 255;
                blend.AlphaFormat = AC_SRC_ALPHA;
                AlphaBlend(dc, centerX - logoWidth / 2, logoTop, logoWidth, logoHeight,
                           source, 0, 0, bitmap.bmWidth, bitmap.bmHeight, blend);
                SelectObject(source, previousBitmap);
                DeleteDC(source);
            }
        }
    }
    else if (g_splash.icon != nullptr)
    {
        // 资源缺失时退回图标，保证加载页总有品牌标识
        const int fallbackIcon = UiScale(64);
        DrawIconEx(dc, centerX - fallbackIcon / 2, height / 2 - UiScale(168), g_splash.icon,
                   fallbackIcon, fallbackIcon, 0, nullptr, DI_NORMAL);
    }

    RECT titleRect{0, height / 2 - UiScale(92), width, height / 2 - UiScale(40)};
    UiText(dc, L"FireWin PE", titleRect, g_splash.titleFont, kUiText,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT subtitleRect{0, height / 2 - UiScale(44), width, height / 2 - UiScale(16)};
    UiText(dc, L"Windows 离线部署与系统维护环境", subtitleRect, g_splash.bodyFont, kUiMuted,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // 进度条
    const int barWidth = UiScale(440);
    const int barHeight = UiScale(6);
    const int barLeft = centerX - barWidth / 2;
    const int barTop = height / 2 + UiScale(36);
    RECT bar{barLeft, barTop, barLeft + barWidth, barTop + barHeight};
    UiRoundRect(dc, bar, barHeight / 2, RGB(58, 58, 58), RGB(58, 58, 58));

    if (percent > 0)
    {
        RECT fill = bar;
        fill.right = bar.left + barWidth * percent / 100;
        if (fill.right > fill.left + 2)
        {
            UiRoundRect(dc, fill, barHeight / 2, kUiAccent, kUiAccent);
        }
    }

    RECT stepRect{barLeft, barTop + UiScale(16), barLeft + barWidth - UiScale(44), barTop + UiScale(40)};
    UiText(dc, step, stepRect, g_splash.smallFont, kUiMuted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT percentRect{barLeft + barWidth - UiScale(40), barTop + UiScale(16), barLeft + barWidth,
                     barTop + UiScale(40)};
    UiText(dc, std::to_wstring(percent) + L"%", percentRect, g_splash.smallFont, kUiMuted,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT hintRect{0, height - UiScale(56), width, height - UiScale(28)};
    UiText(dc, L"初始化在后台进行，按 Esc 或点击任意位置可跳过", hintRect, g_splash.smallFont,
           RGB(120, 120, 120), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(buffer);
    DeleteDC(dc);
}

LRESULT CALLBACK SplashProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintSplash(window, dc);
        EndPaint(window, &paint);
        return 0;
    }

    case kSplashMessageRefresh:
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (wParam == kSplashTimerId)
        {
            // 分辨率在后台被改动后，让加载页重新铺满
            const int width = GetSystemMetrics(SM_CXSCREEN);
            const int height = GetSystemMetrics(SM_CYSCREEN);
            RECT rect{};
            GetWindowRect(window, &rect);
            if (rect.right - rect.left != width || rect.bottom - rect.top != height)
            {
                SetWindowPos(window, HWND_TOPMOST, 0, 0, width, height, SWP_NOACTIVATE);
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            g_splash.skipped.store(true);
        }
        return 0;

    case WM_LBUTTONUP:
        g_splash.skipped.store(true);
        return 0;

    case WM_DESTROY:
        KillTimer(window, kSplashTimerId);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

// 停在一个全屏加载页上，等后台初始化结束（或用户跳过）后返回
void RunSplashStage()
{
    RegisterDesktopClass(kSplashClass, SplashProc, 0);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    g_splash.wallpaper = static_cast<HBITMAP>(LoadImageW(instance, MAKEINTRESOURCEW(IDB_BACKGROUND),
                                                         IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    g_splash.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0,
                                                  LR_DEFAULTSIZE | LR_SHARED));
    g_splash.logo = LoadAlphaBitmapFromResource(instance, IDB_LOGO);
    g_splash.titleFont = CreateDesktopFont(-34, FW_SEMIBOLD);
    g_splash.bodyFont = CreateDesktopFont(-15, FW_NORMAL);
    g_splash.smallFont = CreateDesktopFont(-12, FW_NORMAL);

    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSplashClass, L"FireWin PE", WS_POPUP,
                                  0, 0, width, height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr)
    {
        return;
    }

    g_splash.window.store(window);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetTimer(window, kSplashTimerId, 400, nullptr);

    std::thread worker(InitializePeBackground);
    MSG message{};
    for (;;)
    {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (g_splash.skipped.load())
        {
            // 跳过后让后台线程继续装完剩余驱动，不阻塞进桌面
            if (worker.joinable())
            {
                worker.detach();
            }
            break;
        }

        if (g_splash.finished.load())
        {
            // 让 100% 停留在屏幕上一下，避免一闪而过
            const ULONGLONG until = GetTickCount64() + 350;
            while (GetTickCount64() < until)
            {
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(15);
            }

            if (worker.joinable())
            {
                worker.join();
            }
            break;
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }

    g_splash.window.store(nullptr);
    if (IsWindow(window))
    {
        DestroyWindow(window);
    }

    if (g_splash.wallpaper != nullptr)
    {
        DeleteObject(g_splash.wallpaper);
        g_splash.wallpaper = nullptr;
    }

    if (g_splash.logo != nullptr)
    {
        DeleteObject(g_splash.logo);
        g_splash.logo = nullptr;
    }

    DeleteObject(g_splash.titleFont);
    DeleteObject(g_splash.bodyFont);
    DeleteObject(g_splash.smallFont);
}

int RunDesktopShell()
{
    // 不再显示 startnet.cmd 的 cmd 黑窗
    if (HWND console = GetConsoleWindow(); console != nullptr)
    {
        ShowWindow(console, SW_HIDE);
    }

    RunSplashStage();

    // 加载页阶段可能已把分辨率切换到更高模式，按最终屏幕重新计算缩放与尺寸
    UpdateUiScale();
    InitUiMetrics();

    g_launcher.configPath = LocateConfig(std::wstring());
    g_launcher.toolsDir = GetExeDirectory();
    ScanAppTools();
    g_launcher.font = CreateLauncherFont();

    const HINSTANCE instance = GetModuleHandleW(nullptr);

    g_desktop.background = static_cast<HBITMAP>(LoadImageW(instance, MAKEINTRESOURCEW(IDB_BACKGROUND),
                                                           IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    g_desktop.appIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0,
                                                     0, LR_DEFAULTSIZE | LR_SHARED));

    g_desktop.font = CreateDesktopFont(-13, FW_NORMAL);
    g_desktop.titleFont = CreateDesktopFont(-15, FW_SEMIBOLD);
    g_desktop.smallFont = CreateDesktopFont(-11, FW_NORMAL);
    g_desktop.clockFont = CreateDesktopFont(-15, FW_SEMIBOLD);

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    // 竖向排列：先按可用高度算出一列能放几个图标
    const int usableHeight = std::max(kCellHeight, screenHeight - kTaskbarHeight - kDesktopMargin);
    g_desktop.rows = std::max(1, usableHeight / kCellHeight);

    BuildDesktopItems();
    BuildStartItems(screenHeight);

    RegisterDesktopClass(kDesktopClass, DesktopProc, CS_DBLCLKS);
    RegisterDesktopClass(kTaskbarClass, TaskbarProc, 0);
    RegisterDesktopClass(kStartMenuClass, StartMenuProc, 0);

    g_desktop.window = CreateWindowExW(0, kDesktopClass, L"", WS_POPUP, 0, 0, screenWidth, screenHeight,
                                       nullptr, nullptr, instance, nullptr);
    if (g_desktop.window == nullptr)
    {
        MessageBoxW(nullptr, L"桌面窗口创建失败。", kLauncherTitle, MB_OK | MB_ICONERROR);
        return 5;
    }

    // 现有安装 / 部署流程把 g_launcher.window 当作父窗口，这里指向桌面窗口
    g_launcher.window = g_desktop.window;

    ShowWindow(g_desktop.window, SW_SHOW);
    UpdateWindow(g_desktop.window);

    // 任务栏：置顶弹出窗，最大化窗口也盖不住
    g_desktop.taskbar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kTaskbarClass, L"", WS_POPUP, 0,
                                        screenHeight - kTaskbarHeight, screenWidth, kTaskbarHeight,
                                        nullptr, nullptr, instance, nullptr);

    // 开始菜单：同样置顶，位置在任务栏正上方，默认隐藏
    g_desktop.startMenu = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kStartMenuClass, L"", WS_POPUP, 8,
        screenHeight - kTaskbarHeight - g_desktop.menuHeight - 6, kStartMenuWidth, g_desktop.menuHeight,
        nullptr, nullptr, instance, nullptr);

    if (g_desktop.startMenu != nullptr)
    {
        SetWindowRgn(g_desktop.startMenu,
                     CreateRoundRectRgn(0, 0, kStartMenuWidth + 1, g_desktop.menuHeight + 1, 14, 14), TRUE);
    }

    if (g_desktop.taskbar != nullptr)
    {
        ShowWindow(g_desktop.taskbar, SW_SHOW);
        UpdateWindow(g_desktop.taskbar);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_launcher.deployThread.joinable())
    {
        g_launcher.deployThread.join();
    }

    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
    // 先声明 DPI 感知并按屏幕高度算出全局缩放，后面所有窗口 / 字体都按它换算
    EnableDpiAwareness();
    UpdateUiScale();
    InitUiMetrics();

    // 没有控制台时（例如在 PE 中双击运行）不影响流程，日志 + MessageBox 兜底
    std::wstring configPath;
    bool autoMode = false;
    bool shellMode = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"/autodeploy" || arg == L"-autodeploy")
        {
            autoMode = true;
        }
        else if (arg == L"/shell" || arg == L"-shell")
        {
            shellMode = true;
        }
        else if (arg.rfind(L"/config:", 0) == 0)
        {
            configPath = arg.substr(8);
        }
        else if (arg == L"/?" || arg == L"/help" || arg == L"-h")
        {
            MessageBoxW(nullptr,
                        L"FireWinPE\r\n\r\n"
                        L"用法：\r\n"
                        L"  FireWinPE.exe                  进入 FireWin PE 桌面\r\n"
                        L"  FireWinPE.exe /shell           打开经典启动器窗口\r\n"
                        L"  FireWinPE.exe /autodeploy [/config:<deploy.json>]\r\n\r\n"
                        L"参数：\r\n"
                        L"  /autodeploy        按配置文件自动部署（无人值守入口）\r\n"
                        L"  /shell             打开经典启动器，先检查磁盘再点「开始安装」\r\n"
                        L"  /config:<路径>     指定 deploy.json 路径\r\n\r\n"
                        L"许可证：GPL-3.0-only",
                        L"FireWin PE",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    }

    // 无参数：进入桌面模式（壁纸 + 图标 + 任务栏 + 开始菜单）
    // /shell：退回经典单窗口启动器，作为桌面模式异常时的兜底入口
    if (shellMode)
    {
        return RunLauncher();
    }

    if (argc <= 1)
    {
        return RunDesktopShell();
    }

    const std::wstring resolvedConfig = LocateConfig(configPath);
    if (resolvedConfig.empty())
    {
        MessageBoxW(nullptr,
                    L"未找到 deploy.json。\r\n请确认桌面端已正确执行「写入临时 PE 引导」。",
                    L"FireWin PE",
                    MB_OK | MB_ICONERROR);
        return 2;
    }

    DeployConfig config;
    std::wstring error;
    if (!LoadConfig(resolvedConfig, config, error))
    {
        MessageBoxW(nullptr, (L"deploy.json 读取失败：" + error).c_str(),
                    L"FireWin PE", MB_OK | MB_ICONERROR);
        return 3;
    }

    const std::wstring toolsDir = GetExeDirectory();
    std::wstring logPath = config.logPath.empty()
                               ? JoinPath(toolsDir, L"deploy.log")
                               : config.logPath;

    Logger log;
    if (!log.Open(logPath))
    {
        // 日志写不了也要继续，退回 exe 目录下的日志文件
        logPath = JoinPath(toolsDir, L"deploy.log");
        log.Open(logPath);
    }

    log.Info(L"==== FireWin PE 启动 ====");
    log.Info(L"配置文件：" + resolvedConfig);
    log.Info(L"工具目录：" + toolsDir);
    log.Info(L"自动模式：" + std::wstring(autoMode ? L"是" : L"否"));

    DeploymentResult result = Deploy(log, config, toolsDir);
    log.Info(result.success ? L"==== 部署成功 ====" : L"==== 部署失败 ====");

    ShowResult(result);
    return result.success ? 0 : 1;
}
