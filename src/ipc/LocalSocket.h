/// @file LocalSocket.h
/// @brief IPC 跨平台流式端点：POSIX 为 AF_UNIX 流 socket，Windows 为命名管道流句柄
///
/// 平台 socket 类型经 pimpl 完全隐藏于 .cpp 实现（头文件不出现任何平台
/// 条件编译）；读写接口以 std::error_code / std::function 表达，供 Task 9
/// 的 IpcServer 会话层与 Task 10 的 cli-client 使用。
///
/// 线程与生命周期约定：
/// 1. 非线程安全：所有方法须在驱动 io_context 的线程（或其停止后）调用
/// 2. 调用方须保证对象存活至所有已发起异步操作的回调触发完毕（或先
///    close() 并等回调排空后再销毁）

#ifndef IPC_LOCALSOCKET_H
#define IPC_LOCALSOCKET_H

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <system_error>

namespace asio = boost::asio;

namespace ipc {

class LocalAcceptor;

/// 跨平台流式端点
class LocalSocket
{
public:
    explicit LocalSocket(asio::io_context &io);
    /// 关闭并触发写队列残留回调（见 close()），要求无在途异步操作
    ~LocalSocket();

    LocalSocket(const LocalSocket &) = delete;
    LocalSocket &operator=(const LocalSocket &) = delete;

    bool isOpen() const;

    /// 关闭端点（幂等）：asio 在线操作由其自行以 operation_aborted 完成；
    /// 写队列中排队未发送的回调在此以 operation_aborted 逐一触发，
    /// 避免回调被静默丢弃（对齐 Qt 销毁语义）
    void close();

    asio::io_context &getIoContext();

    /// 异步读：转发平台 socket 的 async_read_some；handler(ec, bytes) 在 io 线程调用
    void asyncReadSome(asio::mutable_buffer buf,
                       std::function<void(std::error_code, std::size_t)> handler);

    /// 串行写：data 连同 handler 入队，按 FIFO 逐个 async_write；
    /// 每个 handler 恰好触发一次（成功无错 / 传输错误 / 关闭中止），
    /// 传输出错时其后的排队 handler 以同一错误码触发
    void asyncWrite(std::string data,
                    std::function<void(std::error_code)> handler);

    /// 客户端连接（cli-client 用，同步阻塞）；成功返回 true，失败时 error 填写原因
    bool connect(const std::string &name, std::string &error);

private:
    /// asyncAccept 需将平台原生 socket 作为接受端点交付（定义所在编译单元可见 Impl）
    friend class LocalAcceptor;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ipc

#endif // IPC_LOCALSOCKET_H
