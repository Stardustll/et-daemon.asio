/// @file WindowsService.cpp
/// @brief Windows 服务注册与控制实现（仅 _WIN32 下包含有效代码）
///
/// 服务内部名取编译期宏 DAEMON_SOCKET_NAME（即 IPC socket 名称），
/// 显示名为 "QtEasyTier Daemon Service"，启动类型为自动（SERVICE_AUTO_START）。
/// 服务命令行形如："<exe 绝对路径>" --daemon，其中 --daemon 必不可少，
/// 否则进程进入普通前台模式而非服务入口，服务会立即退出。
///
/// 自父工程 src/windows-register/WindowsService.cpp 移植：QString →
/// std::wstring/std::string 机械替换（UTF-8 ↔ UTF-16 经 MultiByteToWideChar/
/// WideCharToMultiByte），Qt 日志 → LOG_* 宏，Qt 事件循环退出 → 注入的
/// requestStop 回调（见 WindowsService.h 的 ServiceCallbacks 说明）。

#include "WindowsService.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>

#include <string>

#include "log.h"

namespace {

// ── 服务参数 ──
constexpr wchar_t kDisplayName[] = L"QtEasyTier Daemon Service";
constexpr wchar_t kDescription[] =
    L"QtEasyTier Daemon 本地 IPC 管理服务";

/// UTF-8 → UTF-16（CP_UTF8）
std::wstring utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return std::wstring();
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return std::wstring();
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        &wide[0], length);
    return wide;
}

/// UTF-16 → UTF-8（CP_UTF8）
std::string wideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
        return std::string();
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return std::string();
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        &utf8[0], length, nullptr, nullptr);
    return utf8;
}

/// 去除首尾空白（对齐 QString::trimmed，FormatMessageW 输出常带尾部换行）
std::string trimmed(const std::string& text)
{
    const auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin]))
        ++begin;
    while (end > begin && isSpace(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

/// 服务内部名 = IPC socket 名称（与 DAEMON_SOCKET_NAME 保持一致）
const std::wstring& serviceName()
{
    static const std::wstring name = utf8ToWide(DAEMON_SOCKET_NAME);
    return name;
}

/// 服务启动回调（由 runDaemon 注入，供 ServiceMain 调用）
WindowsService::ServiceCallbacks g_callbacks;
/// 服务状态句柄与状态记录
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
DWORD g_checkpoint = 0;

/// 上报服务状态到服务控制管理器
bool setServiceStatus(DWORD state, DWORD waitHint, DWORD exitCode = NO_ERROR)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = (state == SERVICE_RUNNING)
        ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwWaitHint = waitHint;
    g_status.dwCheckPoint =
        (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? ++g_checkpoint : 0;
    return SetServiceStatus(g_statusHandle, &g_status) != FALSE;
}

/// 将系统错误码格式化为可读文本（UTF-8 输出）
std::string formatError(DWORD err)
{
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (n > 0 && buffer) {
        const std::string message = trimmed(wideToUtf8(std::wstring(buffer, n)));
        LocalFree(buffer);
        if (!message.empty())
            return message;
    }
    return "错误码 " + std::to_string(err);
}

/// 服务停止请求：调用注入的 requestStop（跨线程安全——由 main 接到
/// io_context::stop，SCM 控制线程调用；父工程等价实现为向 Qt 事件循环
/// 投递 QCoreApplication::quit）
void requestServiceStop()
{
    if (g_callbacks.requestStop)
        g_callbacks.requestStop();
}

/// 服务控制处理器（SCM 线程回调）
DWORD WINAPI serviceControlHandler(DWORD control, DWORD, LPVOID, LPVOID)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        // 等待在途 FFI 调用完成，给出较长等待时间
        setServiceStatus(SERVICE_STOP_PENDING, 30000);
        requestServiceStop();
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_statusHandle, &g_status);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

/// 服务主入口（由服务控制管理器在独立线程调用）
void WINAPI serviceMain(DWORD /*argc*/, LPWSTR* /*argv*/)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(
        serviceName().c_str(), serviceControlHandler, nullptr);
    if (!g_statusHandle) {
        LOG_ERROR("注册服务控制处理器失败: " + formatError(GetLastError()));
        return;
    }

    if (!setServiceStatus(SERVICE_START_PENDING, 5000)) {
        LOG_ERROR("上报服务启动中状态失败: " + formatError(GetLastError()));
        return;
    }

    if (!g_callbacks.start) {
        LOG_ERROR("未注入服务启动回调");
        setServiceStatus(SERVICE_STOPPED, 0, ERROR_CALL_NOT_IMPLEMENTED);
        return;
    }

    if (!g_callbacks.start()) {
        LOG_ERROR("daemon 服务启动失败（IPC 监听失败）");
        setServiceStatus(SERVICE_STOPPED, 0, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }

    if (!setServiceStatus(SERVICE_RUNNING, 0)) {
        LOG_ERROR("上报服务运行状态失败: " + formatError(GetLastError()));
    }

    const int exitCode = g_callbacks.run();

    setServiceStatus(SERVICE_STOPPED, 0, exitCode == 0 ? NO_ERROR : static_cast<DWORD>(exitCode));
    g_statusHandle = nullptr;
}

/// 当前进程是否以管理员权限运行
bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)
                    && elevation.TokenIsElevated != FALSE;
    CloseHandle(token);
    return ok;
}

/// 是否为本程序 UAC 提权产生的子进程（继承自父进程的环境变量）
bool isElevatedChild()
{
    wchar_t value[2] = { 0 };
    return GetEnvironmentVariableW(L"QTET_ELEVATED_CHILD", value, 2) == 1 && value[0] == L'1';
}

/// 服务管理操作失败提示：控制台输出；UAC 提权子进程的控制台不可见，改用弹窗
void notifyFailure(const std::wstring& message)
{
    LOG_ERROR(wideToUtf8(message));
    if (isElevatedChild())
        MessageBoxW(nullptr, message.c_str(), kDisplayName, MB_OK | MB_ICONERROR);
}

/// 未提权时通过 UAC 重新启动自身执行服务管理命令，并等待提权子进程结束。
/// 提权子进程以相同参数重新解析执行，原进程透传其退出结果。
bool ensureElevated(const wchar_t* option, std::string* error)
{
    if (isElevated())
        return true;

    wchar_t exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        if (error) *error = "获取可执行文件路径失败: " + formatError(GetLastError());
        return false;
    }

    SetEnvironmentVariableW(L"QTET_ELEVATED_CHILD", L"1");
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = option;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        std::string msg;
        if (err == ERROR_CANCELLED)
            msg = "已取消 UAC 提权，" + wideToUtf8(option) + " 需要管理员权限";
        else
            msg = "启动提权进程失败: " + formatError(err);
        notifyFailure(utf8ToWide(msg));
        if (error) *error = msg;
        return false;
    }

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    if (exitCode == 0)
        return true;
    if (error) *error = "服务管理操作失败（退出码 " + std::to_string(exitCode)
                        + "），详情见提权进程输出";
    return false;
}

/// 打开服务并处理常见失败提示
SC_HANDLE openService(SC_HANDLE scm, DWORD access, std::string* error, bool* notInstalled)
{
    *notInstalled = false;
    SC_HANDLE service = OpenServiceW(scm, serviceName().c_str(), access);
    if (service || GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        return service;
    *notInstalled = true;
    const std::string msg = "服务未注册，请先执行 --install";
    notifyFailure(utf8ToWide(msg));
    if (error) *error = msg;
    return nullptr;
}

/// 打开服务控制管理器
SC_HANDLE openScm(DWORD access, std::string* error)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, access);
    if (scm)
        return scm;
    const std::string msg = "打开服务控制管理器失败: " + formatError(GetLastError());
    notifyFailure(utf8ToWide(msg));
    if (error) *error = msg;
    return nullptr;
}

/// 等待服务进入指定状态（最多约 30 秒）
bool waitServiceState(SC_HANDLE service, DWORD targetState, std::string* error)
{
    SERVICE_STATUS status{};
    for (int i = 0; i < 120; ++i) {
        if (!QueryServiceStatus(service, &status))
            return false;
        if (status.dwCurrentState == targetState)
            return true;
        Sleep(250);
    }
    if (error) *error = "等待服务状态超时";
    return false;
}

} // namespace

namespace WindowsService {

int runDaemon(const ServiceCallbacks& callbacks)
{
    g_callbacks = callbacks;

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(serviceName().c_str()), serviceMain },
        { nullptr, nullptr }
    };

    if (StartServiceCtrlDispatcherW(table))
        return 0; // 服务已停止，dispatcher 返回

    const DWORD err = GetLastError();
    if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        // 手动运行 --daemon（非 SCM 启动）：回退前台模式，便于调试
        LOG_WARNING("--daemon 未由服务控制管理器启动，回退前台模式运行");
        if (!callbacks.start) {
            LOG_ERROR("未注入服务启动回调");
            return 1;
        }
        if (!callbacks.start())
            return 1;
        return callbacks.run();
    }

    LOG_ERROR("注册服务控制入口失败: " + formatError(err));
    return 1;
}

bool install(std::string* error)
{
    if (!ensureElevated(L"--install", error))
        return false;

    wchar_t exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        if (error) *error = "获取可执行文件路径失败: " + formatError(GetLastError());
        return false;
    }
    // 服务命令行必须自带 --daemon，否则进程进入普通前台模式直接退出
    const std::wstring commandLine = L"\"" + std::wstring(exePath) + L"\" --daemon";

    SC_HANDLE scm = openScm(SC_MANAGER_ALL_ACCESS, error);
    if (!scm)
        return false;

    SC_HANDLE service = OpenServiceW(scm, serviceName().c_str(), SERVICE_ALL_ACCESS);
    if (service) {
        // 服务已存在：更新配置
        if (!ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                  SERVICE_ERROR_NORMAL, commandLine.c_str(), nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr)) {
            const std::string msg = "更新服务配置失败: " + formatError(GetLastError());
            notifyFailure(utf8ToWide(msg));
            if (error) *error = msg;
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return false;
        }
        LOG_INFO("服务已存在，已更新配置: " + wideToUtf8(commandLine));
    } else {
        if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
            const std::string msg = "打开服务失败: " + formatError(GetLastError());
            notifyFailure(utf8ToWide(msg));
            if (error) *error = msg;
            CloseServiceHandle(scm);
            return false;
        }
        service = CreateServiceW(scm, serviceName().c_str(), kDisplayName,
                                 SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 commandLine.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!service) {
            const std::string msg = "创建服务失败: " + formatError(GetLastError());
            notifyFailure(utf8ToWide(msg));
            if (error) *error = msg;
            CloseServiceHandle(scm);
            return false;
        }
        LOG_INFO("服务注册成功");
    }

    SERVICE_DESCRIPTIONW description{ const_cast<LPWSTR>(kDescription) };
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

bool start(std::string* error)
{
    if (!ensureElevated(L"--start", error))
        return false;

    SC_HANDLE scm = openScm(SC_MANAGER_CONNECT, error);
    if (!scm)
        return false;

    bool notInstalled = false;
    SC_HANDLE service = openService(scm, SERVICE_START | SERVICE_QUERY_STATUS, error, &notInstalled);
    if (!service) {
        CloseServiceHandle(scm);
        return false;
    }

    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            LOG_INFO("服务已在运行");
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return true;
        }
        std::string msg = "启动服务失败: " + formatError(err);
        if (err == ERROR_SERVICE_REQUEST_TIMEOUT)
            msg += "（服务响应超时，请查看 Windows 事件日志）";
        notifyFailure(utf8ToWide(msg));
        if (error) *error = msg;
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return false;
    }

    if (waitServiceState(service, SERVICE_RUNNING, error)) {
        LOG_INFO("服务已启动");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return true;
    }

    const std::string msg = "服务启动超时或启动后异常退出，请通过服务管理器查看状态";
    notifyFailure(utf8ToWide(msg));
    if (error) *error = msg;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return false;
}

bool stop(std::string* error)
{
    if (!ensureElevated(L"--stop", error))
        return false;

    SC_HANDLE scm = openScm(SC_MANAGER_CONNECT, error);
    if (!scm)
        return false;

    bool notInstalled = false;
    SC_HANDLE service = openService(scm, SERVICE_STOP | SERVICE_QUERY_STATUS, error, &notInstalled);
    if (!service) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status{};
    if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED) {
        LOG_INFO("服务未在运行");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return true;
    }

    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        const std::string msg = "停止服务失败: " + formatError(GetLastError());
        notifyFailure(utf8ToWide(msg));
        if (error) *error = msg;
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return false;
    }

    if (waitServiceState(service, SERVICE_STOPPED, error)) {
        LOG_INFO("服务已停止");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return true;
    }

    const std::string msg = "停止服务超时，请稍后重试或通过服务管理器停止";
    notifyFailure(utf8ToWide(msg));
    if (error) *error = msg;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return false;
}

bool uninstall(std::string* error)
{
    if (!ensureElevated(L"--uninstall", error))
        return false;

    SC_HANDLE scm = openScm(SC_MANAGER_ALL_ACCESS, error);
    if (!scm)
        return false;

    bool notInstalled = false;
    SC_HANDLE service = openService(
        scm, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS, error, &notInstalled);
    if (!service) {
        CloseServiceHandle(scm);
        if (notInstalled) {
            LOG_INFO("服务未安装，无需卸载");
            return true;
        }
        return false;
    }

    // 运行中先停止，再删除
    SERVICE_STATUS status{};
    if (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED) {
        LOG_INFO("服务正在运行，先停止...");
        if (ControlService(service, SERVICE_CONTROL_STOP, &status))
            waitServiceState(service, SERVICE_STOPPED, nullptr);
    }

    if (!DeleteService(service)) {
        const std::string msg = "卸载服务失败: " + formatError(GetLastError());
        notifyFailure(utf8ToWide(msg));
        if (error) *error = msg;
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return false;
    }
    LOG_INFO("服务已卸载");
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

} // namespace WindowsService

#else // !_WIN32

#include "log.h"

namespace WindowsService {

int runDaemon(const ServiceCallbacks& callbacks)
{
    (void)callbacks;
    LOG_ERROR("--daemon 仅支持 Windows 平台");
    return 1;
}

bool install(std::string* error)
{
    if (error) *error = "服务注册仅支持 Windows 平台";
    return false;
}

bool start(std::string* error)
{
    if (error) *error = "服务启动仅支持 Windows 平台";
    return false;
}

bool stop(std::string* error)
{
    if (error) *error = "服务停止仅支持 Windows 平台";
    return false;
}

bool uninstall(std::string* error)
{
    if (error) *error = "服务卸载仅支持 Windows 平台";
    return false;
}

} // namespace WindowsService

#endif // _WIN32
