/// @file LocalSocketPosix.cpp
/// @brief IPC 平台抽象层 POSIX 实现（Linux/macOS）：AF_UNIX 流 socket 全链路
///
/// 本文件是 POSIX 平台的唯一实现编译单元，同时承载 LocalSocket 与
/// LocalAcceptor 的 Impl 定义及全部方法实现——两类 Impl 须同单元定义，
/// 因为 asyncAccept 经友元直接访问对端 LocalSocket::Impl 的平台 socket。
/// Windows 实现见 LocalSocketWindows.cpp（Task 11 填充）。
///
/// 写队列语义：asyncWrite 入队 {data, handler}，同一时刻仅一个
/// async_write 在途（队首项），完成回调按 FIFO 弹出并触发对应 handler；
/// 传输出错时以同一错误码清空其余排队项；close() 时排队未发送项以
/// operation_aborted 触发（在途队首项除外——其缓冲被 async_write 引用，
/// 由 asio 的中止完成回调弹出，避免悬空访问）。

#ifndef _WIN32

#include "ipc/LocalAcceptor.h"
#include "ipc/LocalEndpoint.h"
#include "ipc/LocalSocket.h"
#include "log.h"

#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <deque>
#include <system_error>
#include <utility>

namespace ipc {

// ────────────────────────────── LocalSocket ──────────────────────────────

/// POSIX 端点实现状态：平台 socket + 串行写队列
struct LocalSocket::Impl
{
    explicit Impl(asio::io_context &ioContext)
        : io(ioContext), socket(ioContext)
    {
    }

    /// 发起队首项的 async_write（前置：writing 且队列非空）
    void startNextWrite();

    /// 以 error 逐一触发并清空全部排队回调（FIFO）
    void drainWriteQueue(const std::error_code &error);

    asio::io_context &io;
    asio::local::stream_protocol::socket socket;
    /// {待写数据, 完成回调} 队列：队首为在途项时其 data 缓冲被 async_write 引用
    std::deque<std::pair<std::string, std::function<void(std::error_code)>>> writeQueue;
    bool writing = false;
};

void LocalSocket::Impl::startNextWrite()
{
    asio::async_write(socket, asio::buffer(writeQueue.front().first),
                      [this](const boost::system::error_code &ec, std::size_t /*bytesTransferred*/) {
                          // 队首完成：先弹出再触发，保证 handler 与 data 一一对应
                          auto completed = std::move(writeQueue.front().second);
                          writeQueue.pop_front();
                          if (ec) {
                              // 传输出错：本项收到该错误，其余排队项以同一错误码触发
                              const std::error_code error(ec);
                              completed(error);
                              drainWriteQueue(error);
                              writing = false;
                              return;
                          }
                          completed(std::error_code());
                          if (!writeQueue.empty())
                              startNextWrite();
                          else
                              writing = false;
                      });
}

void LocalSocket::Impl::drainWriteQueue(const std::error_code &error)
{
    // 先换队再触发：回调内若重入 asyncWrite 不会延长本次清空过程
    decltype(writeQueue) remaining;
    remaining.swap(writeQueue);
    for (auto &entry : remaining)
        entry.second(error);
}

LocalSocket::LocalSocket(asio::io_context &io)
    : m_impl(std::make_unique<Impl>(io))
{
}

LocalSocket::~LocalSocket()
{
    close();
}

bool LocalSocket::isOpen() const
{
    return m_impl->socket.is_open();
}

void LocalSocket::close()
{
    // 幂等：对已关闭端点再次 close 无副作用；在线异步操作由 asio 自行以
    // operation_aborted 完成。排队未发送项在此触发回调以免静默丢失；
    // 在途队首项（writing）的缓冲正被 async_write 引用，保留给中止完成
    // 回调弹出并触发，避免悬空访问
    boost::system::error_code ec;
    m_impl->socket.close(ec);
    const std::error_code aborted = std::make_error_code(std::errc::operation_canceled);
    if (m_impl->writing) {
        while (m_impl->writeQueue.size() > 1) {
            auto pending = std::move(m_impl->writeQueue[1].second);
            m_impl->writeQueue.erase(m_impl->writeQueue.begin() + 1);
            pending(aborted);
        }
    } else {
        m_impl->drainWriteQueue(aborted);
    }
}

asio::io_context &LocalSocket::getIoContext()
{
    return m_impl->io;
}

void LocalSocket::asyncReadSome(asio::mutable_buffer buf,
                                std::function<void(std::error_code, std::size_t)> handler)
{
    m_impl->socket.async_read_some(
        buf, [handler = std::move(handler)](const boost::system::error_code &ec,
                                            std::size_t bytesTransferred) {
            handler(std::error_code(ec), bytesTransferred);
        });
}

void LocalSocket::asyncWrite(std::string data, std::function<void(std::error_code)> handler)
{
    m_impl->writeQueue.emplace_back(std::move(data), std::move(handler));
    if (!m_impl->writing) {
        m_impl->writing = true;
        m_impl->startNextWrite();
    }
}

bool LocalSocket::connect(const std::string &name, std::string &error)
{
    const std::string path = localEndpointPath(name);
    boost::system::error_code ec;
    m_impl->socket.open(asio::local::stream_protocol(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    sockaddr_un address;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        error = "socket path too long: " + path;
        boost::system::error_code closeEc;
        m_impl->socket.close(closeEc);
        return false;
    }
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

    // 非阻塞发起 + poll 有界等待（3s，对齐 Qt 版 waitForConnected 超时）：
    // 避免对端 backlog 耗尽时阻塞式 connect 无限期卡住调用线程。
    // 等待完成后恢复原（阻塞）标志，后续 asio 异步读写行为与直连时一致
    const int fd = m_impl->socket.native_handle();
    const int originalFlags = ::fcntl(fd, F_GETFL, 0);
    if (originalFlags >= 0)
        ::fcntl(fd, F_SETFL, originalFlags | O_NONBLOCK);

    int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                       sizeof(address));
    if (rc != 0 && errno == EINPROGRESS) {
        pollfd waiter;
        waiter.fd = fd;
        waiter.events = POLLOUT;
        waiter.revents = 0;
        if (::poll(&waiter, 1, 3000) <= 0 || !(waiter.revents & POLLOUT)) {
            error = "connect timed out";
            boost::system::error_code closeEc;
            m_impl->socket.close(closeEc);
            return false;
        }
        int soError = 0;
        socklen_t soErrorLen = sizeof(soError);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soErrorLen);
        if (soError != 0) {
            error = std::strerror(soError);
            boost::system::error_code closeEc;
            m_impl->socket.close(closeEc);
            return false;
        }
        rc = 0; // poll 确认连接已建立
    }

    if (originalFlags >= 0)
        ::fcntl(fd, F_SETFL, originalFlags);

    if (rc != 0) {
        error = std::strerror(errno);
        boost::system::error_code closeEc;
        m_impl->socket.close(closeEc);
        return false;
    }
    return true;
}

// ───────────────────────────── LocalAcceptor ─────────────────────────────

/// POSIX 接受器实现状态：平台 acceptor + 监听标志
struct LocalAcceptor::Impl
{
    explicit Impl(asio::io_context &ioContext)
        : acceptor(ioContext)
    {
    }

    asio::local::stream_protocol::acceptor acceptor;
    bool listening = false;
};

LocalAcceptor::LocalAcceptor(asio::io_context &io)
    : m_impl(std::make_unique<Impl>(io))
{
}

LocalAcceptor::~LocalAcceptor()
{
    close();
}

bool LocalAcceptor::hasActiveInstance(const std::string &name)
{
    // 纯系统调用探测，不依赖 io_context：无监听者时 AF_UNIX 连接即时失败
    // （ENOENT/ECONNREFUSED）。socket 置非阻塞，避免对端 backlog 耗尽时
    // 阻塞式 connect 无限期卡住 daemon 启动（对齐 Qt 版 waitForConnected
    // 的有界性）
    const std::string path = localEndpointPath(name);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_un address;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return false;
    }
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

    // 非阻塞 connect：0 = 立即连上；EINPROGRESS = 监听者存在但 backlog 满
    // ——二者均说明已有活动实例；其余错误（ENOENT/ECONNREFUSED 等）视为无实例
    const int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                             sizeof(address));
    const bool active = rc == 0 || errno == EINPROGRESS;
    ::close(fd);
    return active;
}

bool LocalAcceptor::listen(const std::string &name, std::string &error)
{
    // 单实例探测：能连上同名端点说明已有活动 daemon
    if (hasActiveInstance(name)) {
        error = "Another qtet-daemon instance is already running on " + name;
        return false;
    }

    const std::string path = localEndpointPath(name);
    ::unlink(path.c_str()); // 清理上次退出的残留 socket 文件（忽略 ENOENT 等）

    boost::system::error_code ec;
    m_impl->acceptor.open(asio::local::stream_protocol(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    m_impl->acceptor.bind(asio::local::stream_protocol::endpoint(path), ec);
    if (ec) {
        error = ec.message();
        boost::system::error_code closeEc;
        m_impl->acceptor.close(closeEc);
        return false;
    }
    m_impl->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        error = ec.message();
        boost::system::error_code closeEc;
        m_impl->acceptor.close(closeEc);
        return false;
    }

    // 权限设置对齐 Qt WorldAccess 语义：尽力而为，失败仅告警不致监听失败
    if (::chmod(path.c_str(), 0666) != 0)
        LOG_WARNING(std::string("Failed to chmod socket file: ") + std::strerror(errno));

    m_impl->listening = true;
    return true;
}

void LocalAcceptor::asyncAccept(LocalSocket &sock, std::function<void(std::error_code)> handler)
{
    // 经友元访问对端平台 socket 作为交付端点；错误码适配为 std::error_code
    m_impl->acceptor.async_accept(
        sock.m_impl->socket, [handler = std::move(handler)](const boost::system::error_code &ec) {
            handler(std::error_code(ec));
        });
}

void LocalAcceptor::close()
{
    boost::system::error_code ec;
    m_impl->acceptor.close(ec);
    m_impl->listening = false;
}

} // namespace ipc

#endif // !_WIN32
