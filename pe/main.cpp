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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <map>
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

DeploymentResult Deploy(Logger& log, const DeployConfig& config, const std::wstring& toolsDir)
{
    DeploymentResult result;
    result.logPath = log.Path();

    // ---- 1. 解析镜像路径（优先卷 GUID 路径，避免 PE 盘符漂移） ----
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
    const std::wstring wimlib = JoinPath(toolsDir, L"wimlib-imagex.exe");
    const std::wstring applyArgs = L"apply \"" + wimPath + L"\" " + std::to_wstring(config.wimIndex) +
                                   L" \"" + targetRoot + L"\" --check";

    DWORD exitCode = 0;
    if (!RunCommand(wimlib, applyArgs, log, exitCode))
    {
        result.message = L"镜像应用失败（wimlib-imagex 退出码 " + std::to_wstring(exitCode) + L"）。";
        log.Error(result.message);
        return result;
    }

    log.Info(L"镜像应用完成。");

    // ---- 4. bcdboot 写入引导 ----
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
// 图形启动器
// 无参数（或 /shell）时进入：先用常用工具检查磁盘、浏览文件，再点「开始安装」。
// 纯 Win32 API 代码建窗，不使用 .rc 资源文件，也不引入第三方库。
// ---------------------------------------------------------------------------
constexpr wchar_t kLauncherClass[] = L"FireWinPELauncher";
constexpr wchar_t kLauncherTitle[] = L"FireWin PE";
constexpr UINT kMessageDeployFinished = WM_APP + 1;

constexpr int kLauncherClientWidth = 520;
// 按钮最多 11 个（含部署）→ 6 行；状态行随后，底部留边
constexpr int kLauncherClientHeight = 448;

enum LauncherCommand : int
{
    kCommandDeploy = 1001,
    kCommandExplorer,
    kCommandSevenZip,
    kCommandNotepadPlusPlus,
    kCommandNotepad,
    kCommandRegedit,
    kCommandCommandPrompt,
    kCommandDiskpart,
    kCommandBootTool,
    kCommandReboot,
    kCommandShutdown,
};

struct LauncherState
{
    HWND window = nullptr;
    HWND statusText = nullptr;
    HWND deployButton = nullptr;
    HFONT font = nullptr;
    std::wstring configPath;
    std::wstring toolsDir;
    bool deploying = false;
    std::thread deployThread;
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
    description.lfHeight = -16;
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

// 后台线程：与 /autodeploy 走完全相同的 LoadConfig → Deploy 流程
void DeployWorker()
{
    std::wstring error;
    DeployConfig config;
    bool success = false;

    if (!LoadConfig(g_launcher.configPath, config, error))
    {
        const std::wstring message = L"deploy.json 读取失败：" + error;
        MessageBoxW(g_launcher.window, message.c_str(), kLauncherTitle, MB_OK | MB_ICONERROR);
    }
    else
    {
        std::wstring logPath = config.logPath.empty() ? JoinPath(g_launcher.toolsDir, L"deploy.log")
                                                      : config.logPath;

        Logger log;
        if (!log.Open(logPath))
        {
            logPath = JoinPath(g_launcher.toolsDir, L"deploy.log");
            log.Open(logPath);
        }

        log.Info(L"==== FireWin PE 启动（图形启动器） ====");
        log.Info(L"配置文件：" + g_launcher.configPath);
        log.Info(L"工具目录：" + g_launcher.toolsDir);

        const DeploymentResult result = Deploy(log, config, g_launcher.toolsDir);
        log.Info(result.success ? L"==== 部署成功 ====" : L"==== 部署失败 ====");
        ShowResult(result);
        success = result.success;
    }

    // 结果交回 UI 线程，由 UI 线程恢复按钮状态
    PostMessageW(g_launcher.window, kMessageDeployFinished, static_cast<WPARAM>(success ? 1 : 0), 0);
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
        SetWindowTextW(g_launcher.statusText, L"正在部署…");
    }

    g_launcher.deployThread = std::thread(DeployWorker);
}

void HandleLauncherCommand(int command)
{
    const std::wstring appsDir = JoinPath(g_launcher.toolsDir, L"Apps");

    switch (static_cast<LauncherCommand>(command))
    {
    case kCommandDeploy:
        StartDeployment();
        break;
    case kCommandExplorer:
    {
        const std::wstring directory = JoinPath(appsDir, L"Explorer++");
        LaunchTool(JoinPath(directory, L"Explorer++.exe"), std::wstring(), false, directory);
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
    case kCommandReboot:
        PerformPowerAction(true);
        break;
    case kCommandShutdown:
        PerformPowerAction(false);
        break;
    default:
        break;
    }
}

void CreateLauncherControls(HWND window)
{
    g_launcher.font = CreateLauncherFont();

    const bool hasConfig = !g_launcher.configPath.empty();
    const std::wstring hint = hasConfig
                                  ? L"可以先用下面的工具检查磁盘、浏览文件，准备好后再点『开始安装』。"
                                  : L"未找到部署配置 deploy.json，只能使用工具。";

    HWND hintText = CreateWindowExW(0, L"STATIC", hint.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    16, 16, kLauncherClientWidth - 32, 56,
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
    buttons.push_back({kCommandExplorer, L"文件管理器"});
    buttons.push_back({kCommandSevenZip, L"7-Zip"});
    buttons.push_back({kCommandNotepadPlusPlus, L"Notepad++"});
    buttons.push_back({kCommandNotepad, L"记事本"});
    buttons.push_back({kCommandRegedit, L"注册表编辑器"});
    buttons.push_back({kCommandCommandPrompt, L"命令提示符"});
    buttons.push_back({kCommandDiskpart, L"磁盘管理"});
    buttons.push_back({kCommandBootTool, L"引导工具"});
    buttons.push_back({kCommandReboot, L"重启"});
    buttons.push_back({kCommandShutdown, L"关机"});

    const int margin = 16;
    const int gap = 12;
    const int buttonWidth = (kLauncherClientWidth - margin * 2 - gap) / 2;
    const int buttonHeight = 40;
    const int rowPitch = buttonHeight + gap;
    const int firstRowTop = 88;

    for (size_t index = 0; index < buttons.size(); ++index)
    {
        const int column = static_cast<int>(index % 2);
        const int row = static_cast<int>(index / 2);
        HWND button = CreateWindowExW(0, L"BUTTON", buttons[index].text,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
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
    }

    // 状态行位置随按钮行数变化（有部署按钮时多一行）
    const int rowCount = static_cast<int>((buttons.size() + 1) / 2);
    const int statusTop = firstRowTop + rowCount * rowPitch + 8;
    g_launcher.statusText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            margin, statusTop, kLauncherClientWidth - margin * 2, 24,
                                            window, nullptr, GetModuleHandleW(nullptr), nullptr);
    ApplyLauncherFont(g_launcher.statusText);
}

LRESULT CALLBACK LauncherProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (message)
    {
    case WM_CREATE:
        CreateLauncherControls(window);
        return 0;
    case WM_COMMAND:
        HandleLauncherCommand(static_cast<int>(LOWORD(wParam)));
        return 0;
    case kMessageDeployFinished:
        g_launcher.deploying = false;
        if (g_launcher.deployThread.joinable())
        {
            g_launcher.deployThread.join();
        }

        if (g_launcher.deployButton != nullptr)
        {
            EnableWindow(g_launcher.deployButton, TRUE);
        }

        if (g_launcher.statusText != nullptr)
        {
            SetWindowTextW(g_launcher.statusText,
                           wParam != 0 ? L"部署完成。" : L"部署失败，详情见日志。");
        }
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

    // 固定大小：只有标题栏、系统菜单和最小化按钮，没有可缩放边框
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rect{0, 0, kLauncherClientWidth, kLauncherClientHeight};
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_launcher.window = CreateWindowExW(0, kLauncherClass, kLauncherTitle, style,
                                        x, y, width, height,
                                        nullptr, nullptr, instance, nullptr);
    if (g_launcher.window == nullptr)
    {
        MessageBoxW(nullptr, L"启动器窗口创建失败。", kLauncherTitle, MB_OK | MB_ICONERROR);
        return 5;
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

} // namespace

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
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
                        L"  FireWinPE.exe                  打开图形启动器\r\n"
                        L"  FireWinPE.exe /autodeploy [/config:<deploy.json>]\r\n\r\n"
                        L"参数：\r\n"
                        L"  /autodeploy        按配置文件自动部署（无人值守入口）\r\n"
                        L"  /shell             打开图形启动器，先使用工具再安装\r\n"
                        L"  /config:<路径>     指定 deploy.json 路径\r\n\r\n"
                        L"许可证：GPL-3.0-only",
                        L"FireWin PE",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    }

    // 无参数或 /shell：进入图形启动器，用户先检查磁盘再点「开始安装」
    if (argc <= 1 || shellMode)
    {
        return RunLauncher();
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
