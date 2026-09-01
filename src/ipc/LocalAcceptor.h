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
///
/// 单监听实例：listen() 成功后经 asyncAccept 逐个交付连接到 LocalSocket；
/// Windows 实现为命名管道多实例模型（见 LocalSocketWindows.cpp 头注释）。
class LocalAcceptor
{
public:
    /// 构造未监听的接受器；须再调用 listen() 绑定端点
    /// @param io 关联的 io_context（接受回调在其上触发）
    explicit LocalAcceptor(asio::io_context &io);
    /// 关闭监听端点（幂等），要求无在途异步接受操作
    ~LocalAcceptor();

    LocalAcceptor(const LocalAcceptor &) = delete;
    LocalAcceptor &operator=(const LocalAcceptor &) = delete;

    /// 探测 name 上是否已有活动服务实例（能连上 = true）
    /// @param name socket 名称
    /// @return true 表示已有 daemon 在监听（调用方应退出以保单实例）
    static bool hasActiveInstance(const std::string &name);

    /// 监听：探测重名 → 清残留 → bind/listen → 权限设置（POSIX chmod 0666，尽力而为）
    /// @param name socket 名称（经 localEndpointPath 解析为平台端点）
    /// @param[out] error 失败时填写可读原因（含"已有实例运行"）
    /// @return 监听成功与否
    bool listen(const std::string &name, std::string &error);

    /// 接受一个连接并交付到 sock；handler(error_code) 在 io 线程调用。
    /// sock 须保持存活直至回调触发（交付的句柄由 sock 接管所有权）
    /// @param[out] sock 接受成功的连接交付目标
    /// @param handler 完成回调（错误码为空表示已交付）
    void asyncAccept(LocalSocket &sock, std::function<void(std::error_code)> handler);

    /// 关闭监听端点（幂等）；在途异步接受以 operation_canceled 完成
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ipc

#endif // IPC_LOCALACCEPTOR_H
