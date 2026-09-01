/// @file LocalAcceptor.h
/// @brief IPC 跨平台服务端接受器：POSIX 为 AF_UNIX 监听 socket，Windows 为命名管道服务端
///
/// 平台 acceptor 类型经 pimpl 完全隐藏于 .cpp 实现（头文件不出现任何
/// 平台条件编译）；listen() 负责单实例探测、残留 socket 文件清理与
/// 权限设置，供 Task 9 的 IpcServer 使用。

#ifndef IPC_LOCALACCEPTOR_H
#define IPC_LOCALACCEPTOR_H

#include <boost/asio/io_context.hpp>

#include <functional>
#include <memory>
#include <string>
#include <system_error>

namespace asio = boost::asio;

namespace ipc {

class LocalSocket;

/// 跨平台服务端接受器
class LocalAcceptor
{
public:
    explicit LocalAcceptor(asio::io_context &io);
    /// 关闭监听端点（幂等），要求无在途异步接受操作
    ~LocalAcceptor();

    LocalAcceptor(const LocalAcceptor &) = delete;
    LocalAcceptor &operator=(const LocalAcceptor &) = delete;

    /// 探测 name 上是否已有活动服务实例（能连上 = true）
    static bool hasActiveInstance(const std::string &name);

    /// 监听：探测重名 → 清残留 → bind/listen → 权限设置（POSIX chmod 0666，尽力而为）
    bool listen(const std::string &name, std::string &error);

    /// 接受一个连接并交付到 sock；handler(error_code) 在 io 线程调用
    void asyncAccept(LocalSocket &sock, std::function<void(std::error_code)> handler);

    /// 关闭监听端点（幂等）
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ipc

#endif // IPC_LOCALACCEPTOR_H
