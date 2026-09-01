/// @file WindowsService.h
/// @brief Windows 服务注册与控制接口（Win32 SCM API 封装）
///
/// 提供 --daemon 服务模式入口与 --install/--start/--stop/--uninstall 服务管理命令。
/// 仅在 Windows 平台生效；其他平台编译为返回失败的空实现。
/// 本模块不依赖 IPC/FFI 业务逻辑，daemon 服务本体由调用方通过回调注入。

#ifndef WINDOWSSERVICE_H
#define WINDOWSSERVICE_H

#include <functional>
#include <string>

namespace WindowsService {

/// daemon 服务端启动回调（由应用入口注入，保持本模块与业务解耦）
struct ServiceCallbacks {
    /// 创建 io_context/线程池/IPC 服务并开始监听；成功返回 true（随后会调用 run）
    std::function<bool()> start;
    /// 运行事件循环（io_context::run + 线程池 join），返回进程退出码
    std::function<int()> run;
    /// 请求停止事件循环（SCM 停止请求时在 SCM 线程调用，须线程安全）。
    /// asio 版新增（父工程经 QMetaObject::invokeMethod(app, "quit") 向 Qt 事件
    /// 循环投递退出；asio 无等价机制，由 main 将本回调接到 io_context::stop）
    std::function<void()> requestStop;
};

/// 以 Windows 服务模式运行：注册服务入口并阻塞直至服务停止。
/// 若进程并非由服务控制管理器启动（手动运行 --daemon），回退为前台模式。
int runDaemon(const ServiceCallbacks& callbacks);

/// 将本程序注册为系统服务（服务命令行自带 "--daemon" 参数）。
/// 未以管理员身份运行时自动触发 UAC 提权。返回是否成功。
bool install(std::string* error = nullptr);

/// 启动已注册的服务并等待其进入运行状态（约 30 秒超时）。
bool start(std::string* error = nullptr);

/// 停止已注册的服务并等待其进入停止状态。
bool stop(std::string* error = nullptr);

/// 卸载已注册的服务（运行中会先停止）。
bool uninstall(std::string* error = nullptr);

} // namespace WindowsService

#endif // WINDOWSSERVICE_H
