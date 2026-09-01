/// @file LocalEndpoint.h
/// @brief IPC 本地端点路径推导：POSIX socket 文件路径 / Windows 命名管道名
///
/// 平台差异（AF_UNIX 路径 vs \\.\pipe\ 前缀）收敛于本头文件的唯一内联函数，
/// 供 LocalSocket::connect 与 LocalAcceptor::listen/hasActiveInstance 复用。

#ifndef IPC_LOCALENDPOINT_H
#define IPC_LOCALENDPOINT_H

#include <cstdlib>
#include <string>

namespace ipc {

/// POSIX: 返回 socket 文件绝对路径——name 以 '/' 开头则原样使用，否则解析到临时目录
/// （TMPDIR 环境变量若设置则用之，否则 /tmp），对齐 Qt QLocalServer/QDir::tempPath()
/// 的路径语义，保证 Qt 前端（QLocalSocket）与 asio daemon 监听/连接同一文件；
/// Windows: 返回 "\\\\.\\pipe\\" + name
inline std::string localEndpointPath(const std::string& name)
{
#ifdef _WIN32
    return "\\\\.\\pipe\\" + name;
#else
    if (!name.empty() && name.front() == '/')
        return name;
    const char* tmpDir = std::getenv("TMPDIR");
    return (tmpDir && *tmpDir ? std::string(tmpDir) : std::string("/tmp")) + "/" + name;
#endif
}

} // namespace ipc

#endif // IPC_LOCALENDPOINT_H
