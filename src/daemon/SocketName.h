/// @file SocketName.h
/// @brief 监听 socket 名解析：命令行 > 环境变量 > 平台默认
///
/// 单一内联函数收敛解析优先级，供 main.cpp 调用、单元测试直接包含验证（header-only）。

#ifndef DAEMON_SOCKETNAME_H
#define DAEMON_SOCKETNAME_H

#include <string>

// 命名空间取名对齐文件名（同 app_paths）；不可用 `daemon`——macOS 的 <cstdlib>
// 在全局域声明了 BSD 遗留函数 `int daemon(int, int)`，会与之冲突。
namespace socket_name {

/// 解析 daemon 监听的 IPC socket 名/路径，优先级从高到低：
///   1. 命令行 `--socket <path>`（socketArg 非空）
///   2. 环境变量 `QTET_DAEMON_SOCKET`（envValue 非空）
///   3. 编译期平台默认（fallback，即 DAEMON_SOCKET_NAME）
///
/// 名称以 '/' 开头时按绝对路径使用，否则由 ipc::localEndpointPath 解析到临时目录
/// （TMPDIR 或 /tmp）。因此 macOS 下经 LaunchDaemon 传入
/// `/var/run/qtet-daemon.sock` 可让 root daemon 与普通用户前端连到同一端点——
/// 二者 TMPDIR 不同（/tmp vs /var/folders/…/T），裸名会解析成不同文件。
///
/// @param socketArg 命令行 --socket 的取值；未提供传空串
/// @param envValue  QTET_DAEMON_SOCKET 环境变量值；未设置传空串
/// @param fallback  平台默认 socket 名/路径，需非空
/// @return 解析出的 socket 名/路径
inline std::string resolveSocketName(const std::string& socketArg,
                                     const std::string& envValue,
                                     const std::string& fallback)
{
    if (!socketArg.empty())
        return socketArg;
    if (!envValue.empty())
        return envValue;
    return fallback;
}

} // namespace socket_name

#endif // DAEMON_SOCKETNAME_H
