/// @file IpcServer.h
/// @brief IPC 服务端：基于 Boost.Asio 的本地 Socket 会话模型（无 Qt 依赖版本）
///
/// 监听指定名称的本地 Socket，支持多个前端客户端会话并行连接。
/// 每个会话独立维护帧缓冲、异步请求与完成回调，避免不同前端之间互相阻塞。
///
/// 线程模型：所有回调均在驱动 io_context 的单线程上串行执行，会话状态无锁。
/// close() 可在 io 线程调用：等待在途 FFI future 期间会短暂阻塞 io 循环
/// （pool 侧任务置值不依赖 io 线程推进，不会死锁），对齐 Qt 版 close() 的
/// 同步等待语义；pool 的 join 由 main 负责，close() 不做。
///
/// 生命周期约定：IpcServer 须存活至 io_context 停止运行且队列中残留回调
/// 排干完毕（main 在 stop 后会 restart 再跑一轮 io 消化 aborted/completion，
/// 之后才析构本对象），销毁后不再有回调执行。

#ifndef IPCSERVER_H
#define IPCSERVER_H

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <filesystem>
#include <future>
#include <memory>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "daemon/Protocol.h"
#include "FfiDispatcher.h"
#include "ipc/LocalAcceptor.h"
#include "ipc/LocalSocket.h"

/// IPC 服务端
class IpcServer
{
public:
    /// io 为单线程事件循环；pool 为 FFI 工作线程池（IpcServer 不拥有二者）
    /// storeBaseDir 为空 → 各 dispatcher 内部用 exeDirectory()；集成测试传临时目录
    IpcServer(asio::io_context& io, asio::thread_pool& pool,
              std::filesystem::path storeBaseDir = {});
    ~IpcServer(); // = close()

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// 监听 + 读取缓存回连。失败时 error 有值
    bool listen(const std::string& name, std::string& error);

    /// 停止接受新连接；等待全部在途 FFI future 完成后清理会话（对齐 Qt 版 close()）
    void close();

private:
    /// 单个前端会话：独立帧缓冲 + 串行请求调度（对齐 Qt 版 ClientSession）
    struct Session : std::enable_shared_from_this<Session> {
        std::shared_ptr<ipc::LocalSocket> socket;
        std::string buffer;
        bool busy = false;
        bool disconnected = false;
        bool cleanupRequested = false;
        proto::IpcId pendingId = 0;
        std::string pendingMethod;
        nlohmann::json pendingParams;
        std::future<FfiDispatcher::DispatchResult> inFlight;
        std::unique_ptr<FfiDispatcher> dispatcher; // 每会话独立（对齐 Qt 版）

        /// 已发起（含排队）尚未触发回调的 socket 异步操作数：
        /// LocalSocket 析构契约要求无在途异步操作，retiring 会话据此判定排空时机
        int pendingSocketOps = 0;
    };

    void doAccept();
    void startRead(const std::shared_ptr<Session>& session);
    void startDispatch(const std::shared_ptr<Session>& session,
                       proto::IpcId id, const std::string& method, const nlohmann::json& params);
    void processNextFrame(const std::shared_ptr<Session>& session);
    void cleanupSession(const std::shared_ptr<Session>& session);
    void logClientDispatchOutcome(const FfiDispatcher::DispatchResult& result,
                                  const nlohmann::json& params, const std::string& method);

    /// 读/写回调发现对端断开（错误且非我方取消，或 EOF）：置位标志，空闲即清理
    void onDisconnect(const std::shared_ptr<Session>& session);

    /// 序列化并写出一帧响应（含在途操作计数与写回调错误监测）
    void writeResponse(const std::shared_ptr<Session>& session, const std::string& payload);

    /// 已退休会话在全部在途操作排空且不忙碌时释放最后持有引用（io 线程调用）
    void maybeReleaseRetired(const std::shared_ptr<Session>& session);

    asio::io_context& m_io;
    asio::thread_pool& m_pool;
    std::filesystem::path m_storeBaseDir; // 透传给 bootstrap 与各会话 dispatcher
    std::unique_ptr<ipc::LocalAcceptor> m_acceptor;
    std::set<std::shared_ptr<Session>> m_sessions;

    /// 已从 m_sessions 移除、等待在途 socket 操作以 aborted 排空后销毁的会话
    /// （LocalSocket 析构契约：先 close() 并等回调排空再销毁）
    std::set<std::shared_ptr<Session>> m_retiring;

    std::string m_socketName;
    std::unique_ptr<FfiDispatcher> m_bootstrapDispatcher; // 仅 reconnectStoredInstances 用
    bool m_closed = false; // close() 重入保护
};

#endif // IPCSERVER_H
