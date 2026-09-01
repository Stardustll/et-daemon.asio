/// @file LocalSocketWindows.cpp
/// @brief IPC 平台抽象层 Windows 实现（命名管道）
///
/// 本文件是 Windows 平台的唯一实现编译单元，同时承载 LocalSocket 与
/// LocalAcceptor 的 Impl 定义及全部方法实现——与 LocalSocketPosix.cpp 的
/// POSIX 版职责对应（asyncAccept 经友元向会话流 LocalSocket::Impl 交付
/// 已连接的命名管道实例句柄）。
///
/// 服务端模型：监听器预创建命名管道实例队列（首实例带
/// FILE_FLAG_FIRST_PIPE_INSTANCE 探测重名；每次 CreateNamedPipeW 均传入
/// 授予 Everyone（WinWorldSid）GENERIC_READ|GENERIC_WRITE 的 DACL——低权限
/// 前端可连接管理员 daemon，对齐 Qt QLocalServer 的 WorldAccess 语义）。
/// asyncAccept 取一个待连接实例调 ConnectNamedPipe（重叠模式），以手动
/// 重置事件 + asio::windows::object_handle 异步等待完成；成功后句柄交付
/// 会话流并立即补充新实例，保持待连接队列不枯竭（IpcServer 的 doAccept
/// 重新挂接受驱动补充节奏）。
///
/// 写队列语义与 POSIX 版逐字一致（见 LocalSocketPosix.cpp 头注释）：
/// asyncWrite 入队 {data, handler}，FIFO 串行 async_write，传输出错以
/// 同一错误码清空排队项；close() 时排队未发送项以 operation_aborted 触发，
/// 在途队首项留给 asio 的中止完成回调弹出。

#ifdef _WIN32

// windows.h 控制宏须先于任何（直接或经 asio 头间接）引入 windows.h 的头文件
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ipc/LocalAcceptor.h"
#include "ipc/LocalEndpoint.h"
#include "ipc/LocalSocket.h"
#include "log.h"

#include <boost/asio/windows/object_handle.hpp>
#include <boost/asio/windows/stream_handle.hpp>
#include <boost/asio/write.hpp>

#include <aclapi.h>
#include <windows.h>

#include <deque>
#include <memory>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace ipc {

namespace {

/// UTF-8 → UTF-16（CP_UTF8）
std::wstring toWide(const std::string &utf8)
{
    if (utf8.empty())
        return std::wstring();
    const int length = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return std::wstring();
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                          &wide[0], length);
    return wide;
}

/// UTF-16 → UTF-8（CP_UTF8）
std::string toUtf8(const std::wstring &wide)
{
    if (wide.empty())
        return std::string();
    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return std::string();
    std::string utf8(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                          &utf8[0], length, nullptr, nullptr);
    return utf8;
}

/// 去除首尾空白（对齐 QString::trimmed，FormatMessageW 输出常带尾部换行）
std::string trimmed(const std::string &text)
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

/// Win32 错误码 → 可读文本（FormatMessageW 系统描述，UTF-8 输出）
std::string formatWin32Error(DWORD error)
{
    wchar_t *buffer = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (n > 0 && buffer) {
        const std::string message = trimmed(toUtf8(std::wstring(buffer, n)));
        ::LocalFree(buffer);
        if (!message.empty())
            return message;
    }
    return "错误码 " + std::to_string(error);
}

} // namespace

// ────────────────────────────── LocalSocket ──────────────────────────────

/// Windows 端点实现状态：命名管道流句柄 + 串行写队列
struct LocalSocket::Impl
{
    explicit Impl(asio::io_context &ioContext)
        : io(ioContext), handle(ioContext)
    {
    }

    /// 发起队首项的 async_write（前置：writing 且队列非空）
    void startNextWrite();

    /// 以 error 逐一触发并清空全部排队回调（FIFO）
    void drainWriteQueue(const std::error_code &error);

    asio::io_context &io;
    /// 命名管道流句柄（要求 OVERLAPPED 句柄：connect/accept 均带
    /// FILE_FLAG_OVERLAPPED 创建后 assign）
    asio::windows::stream_handle handle;
    /// {待写数据, 完成回调} 队列：队首为在途项时其 data 缓冲被 async_write 引用
    std::deque<std::pair<std::string, std::function<void(std::error_code)>>> writeQueue;
    bool writing = false;
};

void LocalSocket::Impl::startNextWrite()
{
    asio::async_write(handle, asio::buffer(writeQueue.front().first),
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
    return m_impl->handle.is_open();
}

void LocalSocket::close()
{
    // 幂等：对已关闭端点再次 close 无副作用；在线异步操作由 asio 自行以
    // operation_aborted 完成（IOCP 句柄 close 内部取消重叠操作）。排队未发送
    // 项在此触发回调以免静默丢失；在途队首项（writing）的缓冲正被
    // async_write 引用，保留给中止完成回调弹出并触发，避免悬空访问
    boost::system::error_code ec;
    m_impl->handle.close(ec);
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
    m_impl->handle.async_read_some(
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
    const std::wstring pipePath = toWide(localEndpointPath(name));

    // asio::windows::stream_handle 仅支持 OVERLAPPED 句柄（异步读写经重叠
    // ReadFile/WriteFile 完成），故 CreateFileW 须带 FILE_FLAG_OVERLAPPED
    const auto openPipe = [&pipePath]() {
        return ::CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    };

    HANDLE h = openPipe();
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // 全部实例忙：等待任一实例可用（至多 5 秒）后仅重试一次
        if (err == ERROR_PIPE_BUSY && ::WaitNamedPipeW(pipePath.c_str(), 5000) != FALSE)
            h = openPipe();
        if (h == INVALID_HANDLE_VALUE) {
            error = "Failed to connect to pipe: " + formatWin32Error(::GetLastError());
            return false;
        }
    }

    boost::system::error_code ec;
    m_impl->handle.assign(h, ec);
    if (ec) {
        error = ec.message();
        ::CloseHandle(h);
        return false;
    }
    return true;
}

// ───────────────────────────── LocalAcceptor ─────────────────────────────

/// Windows 接受器实现状态：预创建管道实例队列 + Everyone DACL + 在途接受
struct LocalAcceptor::Impl
{
    /// 单次异步接受操作状态（shared_ptr 保活至回调触发完毕）
    struct AcceptOperation : std::enable_shared_from_this<AcceptOperation>
    {
        /// 已连接实例句柄向会话流的交付闭包（在 LocalAcceptor::asyncAccept
        /// 内创建——该成员函数作用域经友元可访问 LocalSocket::Impl）
        using Deliver = std::function<void(HANDLE, std::error_code &)>;

        ~AcceptOperation()
        {
            // 兜底：事件流未走完（如 io 停止后销毁）时释放实例句柄，避免泄漏；
            // waiter 析构自动注销等待并关闭事件句柄
            if (pipe != INVALID_HANDLE_VALUE)
                ::CloseHandle(pipe);
        }

        /// 等待事件完成回调（io 线程）：经存活哨兵探测接受器仍在时才回访
        void onWaitComplete(const boost::system::error_code &waitEc);

        Impl *acceptor = nullptr;
        /// 接受器存活哨兵（防止 io 仍在运行期间接受器被析构后的悬空回访）
        std::weak_ptr<void> acceptorAlive;
        Deliver deliver;
        std::function<void(std::error_code)> handler;
        /// 正在等待连接的管道实例（所有权在未交付期间归本对象）
        HANDLE pipe = INVALID_HANDLE_VALUE;
        /// ConnectNamedPipe 的重叠结构（hEvent 指向 waiter 持有的事件）
        OVERLAPPED overlapped{};
        /// 事件等待器：独占持有事件句柄，析构时关闭
        std::unique_ptr<asio::windows::object_handle> waiter;
        bool finished = false; // finishAccept 重入保护
    };

    explicit Impl(asio::io_context &ioContext)
        : io(ioContext)
    {
    }

    ~Impl()
    {
        // DACL 由 SetEntriesInAclW 分配：绝对格式 SECURITY_DESCRIPTOR 引用其
        // 指针，须存活至全部 CreateNamedPipeW 之后——选择保持至 Impl 析构释放
        if (acl)
            ::LocalFree(acl);
    }

    /// 构建 Everyone（WinWorldSid）读写授权 DACL 与安全属性
    bool buildPipeSecurity();

    /// 创建一个命名管道实例；失败返回 INVALID_HANDLE_VALUE 且 lastError 有值
    HANDLE createPipeInstance(bool firstInstance, DWORD &lastError);

    /// 补充一个后备实例入队（失败仅告警，不致监听失败）
    void enqueueSpareInstance();

    /// 取待连接实例发起 ConnectNamedPipe；硬错误（非 IO_PENDING/PIPE_CONNECTED）
    /// 时销毁实例并消耗下一个重试——有界（attemptsLeft 递减，防止实例
    /// 创建-失败无限循环）
    void startAccept(const std::shared_ptr<AcceptOperation> &op, int attemptsLeft);

    /// 连接事件已触发（io 线程）：GetOverlappedResult 判定成败
    void onAcceptEvent(const std::shared_ptr<AcceptOperation> &op);

    /// 终结一次接受：connected 时交付句柄，随后补充实例并触发 handler 恰好一次
    void finishAccept(const std::shared_ptr<AcceptOperation> &op, std::error_code ec,
                      bool connected);

    asio::io_context &io;
    /// 管道全名（\\.\pipe\<name>，listen 时填充）
    std::wstring pipeName;
    /// Everyone SID / DACL / 绝对格式安全描述符存储（listen 期构建）
    std::vector<BYTE> worldSid;
    PACL acl = nullptr; // SetEntriesInAclW 分配，LocalFree 释放
    std::vector<BYTE> securityDescriptor;
    SECURITY_ATTRIBUTES securityAttributes{};
    /// 待连接管道实例句柄队列（保持 ≥1：接受成功后立即补充）
    std::deque<HANDLE> pendingInstances;
    /// 在途接受操作（close 时逐个 CancelIoEx 使其以中止完成）
    std::set<std::shared_ptr<AcceptOperation>> inFlight;
    /// 存活哨兵：与 worldSid 等同生命周期，供等待回调探测接受器存活
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    bool listening = false;
};

bool LocalAcceptor::Impl::buildPipeSecurity()
{
    // Everyone SID：低权限前端可连接管理员 daemon（Qt WorldAccess 语义）
    worldSid.assign(SECURITY_MAX_SID_SIZE, 0);
    DWORD sidSize = SECURITY_MAX_SID_SIZE;
    if (!::CreateWellKnownSid(WinWorldSid, nullptr, worldSid.data(), &sidSize))
        return false;
    worldSid.resize(sidSize);

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWCH>(worldSid.data());

    if (::SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS) {
        acl = nullptr;
        return false;
    }

    securityDescriptor.assign(SECURITY_DESCRIPTOR_MIN_LENGTH, 0);
    PSECURITY_DESCRIPTOR sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDescriptor.data());
    if (!::InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION))
        return false;
    // 绝对格式 SD 以指针引用 acl：acl 须存活至所有 CreateNamedPipeW 调用之后
    //（保持至 close/析构，见 ~Impl 注释）
    if (!::SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE))
        return false;

    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = sd;
    securityAttributes.bInheritHandle = FALSE;
    return true;
}

HANDLE LocalAcceptor::Impl::createPipeInstance(bool firstInstance, DWORD &lastError)
{
    // 出/入缓冲 0（系统缺省）；PIPE_UNLIMITED_INSTANCES 允许后续补充实例
    const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED
        | (firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE h = ::CreateNamedPipeW(
        pipeName.c_str(), openMode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
        0, 0, 0, &securityAttributes);
    if (h == INVALID_HANDLE_VALUE)
        lastError = ::GetLastError();
    return h;
}

void LocalAcceptor::Impl::enqueueSpareInstance()
{
    DWORD err = 0;
    const HANDLE h = createPipeInstance(false, err);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_WARNING("Failed to create pipe instance: " + formatWin32Error(err));
        return;
    }
    pendingInstances.push_back(h);
}

void LocalAcceptor::Impl::startAccept(const std::shared_ptr<AcceptOperation> &op,
                                      int attemptsLeft)
{
    // 取待连接实例；队列空则现建一个（保持监听能力）
    if (pendingInstances.empty()) {
        DWORD err = 0;
        const HANDLE fresh = createPipeInstance(false, err);
        if (fresh == INVALID_HANDLE_VALUE) {
            finishAccept(op,
                         std::error_code(static_cast<int>(err), std::system_category()),
                         false);
            return;
        }
        pendingInstances.push_back(fresh);
    }

    const HANDLE h = pendingInstances.front();
    pendingInstances.pop_front();

    // 手动重置事件：ConnectNamedPipe 完成（含取消）时置位并保持；object_handle
    // 等待器独占持有该事件，析构时负责关闭
    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h); // 本实例销毁
        finishAccept(op,
                     std::error_code(static_cast<int>(err), std::system_category()),
                     false);
        return;
    }

    op->pipe = h;
    op->overlapped = OVERLAPPED{};
    op->overlapped.hEvent = event;
    op->waiter = std::make_unique<asio::windows::object_handle>(io, event);

    if (::ConnectNamedPipe(h, &op->overlapped) != FALSE
        || ::GetLastError() == ERROR_PIPE_CONNECTED) {
        // 客户端在 CreateNamedPipe 与 ConnectNamedPipe 之间抢连（或同步完成）：
        // 立即成功，无需等待事件
        finishAccept(op, std::error_code(), true);
        return;
    }

    const DWORD err = ::GetLastError();
    if (err != ERROR_IO_PENDING) {
        // 实例故障（如 ERROR_NO_DATA：客户端连上又立即断开）：销毁实例，
        // 有界重试下一个待连接实例
        LOG_WARNING("ConnectNamedPipe failed: " + formatWin32Error(err));
        ::CloseHandle(h);
        op->pipe = INVALID_HANDLE_VALUE;
        op->waiter.reset(); // 关闭本实例事件
        if (attemptsLeft > 0)
            startAccept(op, attemptsLeft - 1);
        else
            finishAccept(op,
                         std::error_code(static_cast<int>(err), std::system_category()),
                         false);
        return;
    }

    // ERROR_IO_PENDING：连接等待中；事件置位后在 io 线程回调
    op->waiter->async_wait([op](const boost::system::error_code &ec) {
        op->onWaitComplete(ec);
    });
}

void LocalAcceptor::Impl::AcceptOperation::onWaitComplete(
    const boost::system::error_code &waitEc)
{
    // 接受器已析构（io 仍在运行的异常拆序场景）：不再回访，静默终结
    //（对齐 asio 平台对象析构即销毁未完成操作、不触发 handler 的语义）
    if (acceptorAlive.expired())
        return;
    const auto self = shared_from_this();
    if (waitEc)
        acceptor->finishAccept(self, std::error_code(waitEc), false);
    else
        acceptor->onAcceptEvent(self);
}

void LocalAcceptor::Impl::onAcceptEvent(const std::shared_ptr<AcceptOperation> &op)
{
    // 事件已置位：以非阻塞 GetOverlappedResult 取最终结果
    DWORD bytes = 0;
    if (::GetOverlappedResult(op->pipe, &op->overlapped, &bytes, FALSE)) {
        finishAccept(op, std::error_code(), true);
        return;
    }
    const DWORD err = ::GetLastError();
    if (err == ERROR_OPERATION_ABORTED)
        // close() 的 CancelIoEx 取消：以 operation_canceled 完成回调
        finishAccept(op, std::make_error_code(std::errc::operation_canceled), false);
    else
        finishAccept(op, std::error_code(static_cast<int>(err), std::system_category()),
                     false);
}

void LocalAcceptor::Impl::finishAccept(const std::shared_ptr<AcceptOperation> &op,
                                       std::error_code ec, bool connected)
{
    if (op->finished) // 恰好触发一次的兜底保护
        return;
    op->finished = true;
    inFlight.erase(op);

    // 关闭事件等待器（注销等待并关闭事件句柄；此后不再触碰 overlapped）
    op->waiter.reset();

    if (connected) {
        // 已连接实例句柄所有权交付会话流（deliver 经友元访问构造）
        std::error_code assignEc;
        op->deliver(op->pipe, assignEc);
        if (assignEc) {
            ec = assignEc;
            connected = false; // 交付失败走销毁路径
        } else {
            op->pipe = INVALID_HANDLE_VALUE; // 所有权已移交
        }
    }
    if (!connected && op->pipe != INVALID_HANDLE_VALUE) {
        ::CloseHandle(op->pipe); // 销毁未交付的实例
        op->pipe = INVALID_HANDLE_VALUE;
    }

    // 补充后备实例保持待连接队列不枯竭（close 后不再补充）
    if (listening)
        enqueueSpareInstance();

    auto handler = std::move(op->handler);
    op->handler = nullptr;
    if (handler)
        handler(ec);
}

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
    // 纯系统调用探测，不依赖 io_context：CreateFileW 连接一次即知实例是否存在。
    // 探测连接立即关闭：实例回池等待服务端 ConnectNamedPipe（与 POSIX 版
    // connect+close 探测一致，服务端会接受到一个立即 EOF 的会话）
    const std::wstring pipePath = toWide(localEndpointPath(name));
    HANDLE h = ::CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
        return true;
    }
    const DWORD err = ::GetLastError();
    // 全部实例被占用（ERROR_PIPE_BUSY）= 有活动服务在服务客户端；
    // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND（管道不存在）与其余错误
    //（访问拒绝等）一律按无实例处理——重名兜底由 listen 的
    // FILE_FLAG_FIRST_PIPE_INSTANCE 探测完成（文档化选择）
    return err == ERROR_PIPE_BUSY;
}

bool LocalAcceptor::listen(const std::string &name, std::string &error)
{
    if (m_impl->listening) {
        error = "acceptor is already listening";
        return false;
    }

    if (!m_impl->buildPipeSecurity()) {
        error = "Failed to build pipe security descriptor: " + formatWin32Error(::GetLastError());
        return false;
    }
    m_impl->pipeName = toWide(localEndpointPath(name));

    // 首实例带 FILE_FLAG_FIRST_PIPE_INSTANCE：管道名已被其他 daemon 实例监听时
    // 创建失败，等价于 POSIX 的连接探测重名（IpcServer::listen 已先探测过，
    // 这里兜底并发竞态）
    DWORD err = 0;
    HANDLE first = m_impl->createPipeInstance(true, err);
    if (first == INVALID_HANDLE_VALUE) {
        if (err == ERROR_ACCESS_DENIED || err == ERROR_ALREADY_EXISTS)
            error = "Another qtet-daemon instance is already running on " + name;
        else
            error = "Failed to create named pipe: " + formatWin32Error(err);
        return false;
    }
    m_impl->pendingInstances.push_back(first);

    // 后备实例 ×2：客户端抢连间隙仍有实例可连（失败仅告警，首实例已可用）
    for (int i = 0; i < 2; ++i)
        m_impl->enqueueSpareInstance();

    m_impl->listening = true;
    return true;
}

void LocalAcceptor::asyncAccept(LocalSocket &sock, std::function<void(std::error_code)> handler)
{
    if (!m_impl->listening) {
        // 已关闭/未监听：直接以中止完成，保证 handler 不被静默丢弃
        handler(std::make_error_code(std::errc::operation_canceled));
        return;
    }

    auto op = std::make_shared<Impl::AcceptOperation>();
    op->acceptor = m_impl.get();
    op->acceptorAlive = m_impl->alive;
    // 交付闭包须在本成员函数作用域内创建以获得 LocalSocket 的友元访问权
    op->deliver = [&sock](HANDLE h, std::error_code &outEc) {
        boost::system::error_code ec;
        sock.m_impl->handle.assign(h, ec);
        outEc = std::error_code(ec);
    };
    op->handler = std::move(handler);
    m_impl->inFlight.insert(op);

    // 硬错误有界重试预算：初值 4（listen 预置 ≤3 实例 + 现建 1）
    m_impl->startAccept(op, 4);
}

void LocalAcceptor::close()
{
    // 幂等：先取消在途接受再关闭全部待连接实例。CancelIoEx 使每个
    // ConnectNamedPipe 立即以 ERROR_OPERATION_ABORTED 完成（事件置位 →
    // object_handle 回调在 io 线程触发 → handler 收到 operation_canceled，
    // 绝不静默丢失）；io 已停止的拆序场景由 ~AcceptOperation 兜底释放句柄
    for (const auto &op : m_impl->inFlight) {
        if (op->pipe != INVALID_HANDLE_VALUE)
            ::CancelIoEx(op->pipe, &op->overlapped);
    }
    for (HANDLE h : m_impl->pendingInstances)
        ::CloseHandle(h);
    m_impl->pendingInstances.clear();
    m_impl->listening = false;
}

} // namespace ipc

#endif // _WIN32
