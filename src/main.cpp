/// @file main.cpp
/// @brief qtet-daemon-asio 应用入口
///
/// 默认以前台模式运行并监听本地 IPC socket，处理 SIGINT/SIGTERM 优雅退出。
/// 监听端点解析优先级：`--socket <path>` > `QTET_DAEMON_SOCKET` 环境变量 >
/// 编译期平台默认（DAEMON_SOCKET_NAME，macOS 为 /var/run/qtet-daemon.sock）。
/// Windows 平台额外支持：
///   --daemon     以 Windows 服务模式运行（由服务控制管理器启动，手动运行时回退前台）
///   --install    注册 Windows 服务（自动 UAC 提权）
///   --start      启动已注册的服务（自动 UAC 提权）
///   --stop       停止已注册的服务（自动 UAC 提权）
///   --uninstall  卸载已注册的服务（自动 UAC 提权）
/// 服务管理命令仅操作服务控制管理器：不初始化 FFI、不监听 socket。

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/thread_pool.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "daemon/IpcServer.h"
#include "daemon/SocketName.h"
#include "log.h"

#ifdef _WIN32
#include "windows-register/WindowsService.h"
#endif

namespace {

/// 唯一应用对象（前台模式/服务模式共用，父工程 g_app/g_server 风格）。
/// 拆解顺序在 runDaemonLoop 中显式控制：server → signals → pool → io
std::unique_ptr<asio::io_context> g_io;
std::unique_ptr<asio::thread_pool> g_pool;
std::unique_ptr<asio::signal_set> g_signals;
std::unique_ptr<IpcServer> g_server;
/// 解析后的监听端点（前台/服务模式共用，由 main 在启动前写入）
std::string g_socketName;

/// 打印用法（列出全部选项）
void printUsage(std::FILE* out)
{
    std::fprintf(out,
                 "Usage: qtet-daemon [options]\n"
                 "\n"
                 "Options:\n"
                 "  --help       显示本帮助信息\n"
                 "  --version    显示版本信息\n"
                 "  --socket <path>  指定 IPC 端点（socket 文件路径或名称；亦可经\n"
                 "                   环境变量 QTET_DAEMON_SOCKET 提供；默认 qtet-daemon.sock，\n"
                 "                   macOS 默认 /var/run/qtet-daemon.sock）\n"
                 "  --daemon     以 Windows 服务模式运行（仅 Windows，由服务控制管理器启动）\n"
                 "  --install    注册 Windows 服务（仅 Windows，需要管理员权限）\n"
                 "  --start      启动已注册的 Windows 服务（仅 Windows，需要管理员权限）\n"
                 "  --stop       停止已注册的 Windows 服务（仅 Windows，需要管理员权限）\n"
                 "  --uninstall  卸载已注册的 Windows 服务（仅 Windows，需要管理员权限）\n");
}

/// 创建 io 事件循环/FFI 线程池/IPC 服务并监听；成功返回 true
///（前台模式与 Windows 服务模式共用；服务停止走 SCM 的 requestStop 而非信号，
/// 控制台信号注册在服务模式下无控制台可用，注册本身无副作用）
bool createDaemonApp()
{
    g_io = std::make_unique<asio::io_context>();
    g_pool = std::make_unique<asio::thread_pool>(4);

    // 注册系统信号处理：Ctrl+C 或 SIGTERM 时优雅退出
    g_signals = std::make_unique<asio::signal_set>(*g_io, SIGINT, SIGTERM);
    g_signals->async_wait([](const std::error_code&, int) {
        LOG_INFO("Shutting down...");
        if (g_server)
            g_server->close();
        if (g_io)
            g_io->stop();
    });

    g_server = std::make_unique<IpcServer>(*g_io, *g_pool);
    std::string error;
    if (!g_server->listen(g_socketName, error)) {
        LOG_ERROR(error);
        g_server.reset();
        return false;
    }
    return true;
}

/// 运行事件循环并按依赖序拆解全局对象；返回进程退出码
int runDaemonLoop()
{
    g_io->run();
    // 池任务完成后其投递的 completion 已在 io 队列中；join 先于后续排干，
    // 保证 close() 等待 future 时不会遇到仍在执行的任务
    g_pool->join();
    // 取消挂起的信号等待（服务路径未经信号退出时仍有一个 async_wait 在途），
    // 避免第二轮 run 因等待信号而挂起
    g_signals.reset();
    // 幂等（信号路径已在回调中 close 过）：确保全部端点关闭，在途 socket
    // 操作以 operation_aborted 入队，FFI completion 亦已在队列中
    if (g_server)
        g_server->close();
    // 第二轮事件循环：排干全部残留回调（aborted 读/写/接受、completion），
    // 使退休会话的 pendingSocketOps 归零并释放——满足 LocalSocket
    // “析构前无在途异步操作”的契约（此前 io.stop() 直接丢弃队列会违反它）。
    // 回调内捕获的 IpcServer 指针仍有效（server 在排干后才析构）
    g_io->restart();
    g_io->run();
    // 拆解顺序：server（此刻 m_sessions/m_retiring 已排空）→ 线程池 → io
    //（io 须最后：队列残留的回调对象仍引用其内部状态）
    g_server.reset();
    g_pool.reset();
    g_io.reset();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    bool daemon = false;
    bool install = false;
    bool start = false;
    bool stop = false;
    bool uninstall = false;
    std::string socketArg; // --socket 显式取值；空串表示未提供

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            printUsage(stdout);
            return 0;
        }
        if (arg == "--version") {
            std::printf("qtet-daemon %s (EasyTier FFI %s)\n", PROJECT_VERSION, ET_FFI_VERSION);
            return 0;
        }
        if (arg == "--daemon") {
            daemon = true;
            continue;
        }
        if (arg == "--install") {
            install = true;
            continue;
        }
        if (arg == "--start") {
            start = true;
            continue;
        }
        if (arg == "--stop") {
            stop = true;
            continue;
        }
        if (arg == "--uninstall") {
            uninstall = true;
            continue;
        }
        if (arg == "--socket") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Option --socket requires a value\n");
                return 1;
            }
            socketArg = argv[++i];
            continue;
        }
        if (arg.rfind("--socket=", 0) == 0) {
            socketArg = arg.substr(std::string("--socket=").size());
            continue;
        }
        std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
        printUsage(stderr);
        return 1;
    }

    // 解析监听端点：--socket > QTET_DAEMON_SOCKET 环境变量 > 平台默认
    const char* envSocket = std::getenv("QTET_DAEMON_SOCKET");
    g_socketName = socket_name::resolveSocketName(
        socketArg, envSocket != nullptr ? std::string(envSocket) : std::string(),
        DAEMON_SOCKET_NAME);

#ifdef _WIN32
    // 服务管理命令：仅操作服务控制管理器，不初始化 FFI、不监听 socket
    //（WindowsService 内部已输出失败详情，见 notifyFailure）
    // 多个选项同时出现时按 install → start → stop → uninstall 优先级处理
    if (install)
        return WindowsService::install() ? 0 : 1;
    if (start)
        return WindowsService::start() ? 0 : 1;
    if (stop)
        return WindowsService::stop() ? 0 : 1;
    if (uninstall)
        return WindowsService::uninstall() ? 0 : 1;

    // --daemon：以 Windows 服务模式运行（非 SCM 启动时回退前台模式）
    if (daemon) {
        WindowsService::ServiceCallbacks callbacks;
        callbacks.start = []() -> bool { return createDaemonApp(); };
        callbacks.run = []() -> int { return runDaemonLoop(); };
        // SCM 停止请求（SCM 线程调用）：仅停 io 事件循环；close 与残留回调
        // 排干由 runDaemonLoop 统一执行（避免 SCM 线程与 io 线程并发访问
        // server），在途 FFI 由 pool.join 等待完成后优雅上报 STOPPED
        callbacks.requestStop = []() {
            if (g_io)
                g_io->stop();
        };
        return WindowsService::runDaemon(callbacks);
    }
#else
    if (install || start || stop || uninstall) {
        LOG_ERROR("服务管理选项（--install/--start/--stop/--uninstall）仅支持 Windows");
        return 1;
    }
    if (daemon)
        LOG_WARNING("--daemon 仅支持 Windows，按前台模式运行");
#endif

    // 默认：前台模式
    if (!createDaemonApp())
        return 1;
    return runDaemonLoop();
}
