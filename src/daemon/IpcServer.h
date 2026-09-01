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
///
/// 组合三层能力：LocalAcceptor（连接接受）→ Protocol（帧解析/序列化）→
/// FfiDispatcher（FFI 调度），自身不持有线程——回调串行跑在调用方的
/// io_context 上，FFI 阻塞调用卸载到调用方的线程池。
class IpcServer
{
public:
    /// 构造 IPC 服务端
    /// @param io 单线程事件循环（全部回调在其上串行执行；不拥有）
    /// @param pool FFI 工作线程池（不拥有）
    /// @param storeBaseDir 回连缓存目录；为空 → 各 dispatcher 内部用
    ///        exeDirectory()，集成测试传临时目录
    IpcServer(asio::io_context& io, asio::thread_pool& pool,
              std::filesystem::path storeBaseDir = {});
    ~IpcServer(); // = close()

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// 监听 + 读取缓存回连。失败时 error 有值
    /// @param name socket 名称（单实例探测也基于该名称）
    /// @param[out] error 失败时填写可读原因（含"已有实例运行"）
    /// @return 监听成功与否；失败后对象可安全析构
    bool listen(const std::string& name, std::string& error);

    /// 停止接受新连接；等待全部在途 FFI future 完成后清理会话（对齐 Qt 版 close()）
    void close();

private:
    /// 单个前端会话：独立帧缓冲 + 串行请求调度（对齐 Qt 版 ClientSession）
    struct Session : std::enable_shared_from_this<Session> {
        std::shared_ptr<ipc::LocalSocket> socket;  ///< 会话传输端点（接受时移交独占）
        std::string buffer;                        ///< 帧接收累积缓冲（parseFrame 就地消费）
        bool busy = false;                         ///< 本会话是否有 FFI 调用在途（在途期间停读背压）
        bool disconnected = false;                 ///< 对端断开/传输错误标记：不再写响应
        bool cleanupRequested = false;             ///< 我方请求清理标记（close/断开/非法帧）
        proto::IpcId pendingId = 0;                ///< 在途请求的序号（completion 写响应回填）
        std::string pendingMethod;                 ///< 在途请求的方法名（响应与日志回填）
        nlohmann::json pendingParams;              ///< 在途请求参数（完成日志提取实例名用）
        std::future<FfiDispatcher::DispatchResult> inFlight;  ///< 在途异步 FFI 调用（close 时 wait）
        std::unique_ptr<FfiDispatcher> dispatcher; ///< 每会话独立（对齐 Qt 版）

        /// 已发起（含排队）尚未触发回调的 socket 异步操作数：
        /// LocalSocket 析构契约要求无在途异步操作，retiring 会话据此判定排空时机
        int pendingSocketOps = 0;
    };

    /// 挂起一次异步接受；回调内创建会话（独立 dispatcher）并递归续挂下一接受
    void doAccept();

    /// 挂起 64 KiB 异步读；回调内追加缓冲、驱动帧解析，空闲时续挂读
    ///（busy 期间停读，数据滞留内核缓冲形成背压）
    void startRead(const std::shared_ptr<Session>& session);

    /// 提交一个异步 FFI 调用并置 busy；completion 中写响应、处理缓冲区后续帧
    /// 并补挂读（弱引用守护会话，防止 completion 执行时会话已析构）
    void startDispatch(const std::shared_ptr<Session>& session,
                       proto::IpcId id, const std::string& method, const nlohmann::json& params);

    /// 循环解析缓冲区内的完整帧：Incomplete 即止；Invalid 断连；
    /// method 为空回错误响应后继续；合法帧提交 startDispatch 并让出
    void processNextFrame(const std::shared_ptr<Session>& session);

    /// 会话清理主体（幂等，可安全重入）：从活动集合移入退休集合、关闭 socket；
    /// 最后引用由 maybeReleaseRetired 在全部在途操作排空后释放
    void cleanupSession(const std::shared_ptr<Session>& session);

    /// run/delete_network_instance 的成败控制台日志（从参数中提取实例名）
    void logClientDispatchOutcome(const FfiDispatcher::DispatchResult& result,
                                  const nlohmann::json& params, const std::string& method);

    /// 读/写回调发现对端断开（错误且非我方取消，或 EOF）：置位标志，空闲即清理
    void onDisconnect(const std::shared_ptr<Session>& session);

    /// 序列化并写出一帧响应（含在途操作计数与写回调错误监测）
    void writeResponse(const std::shared_ptr<Session>& session, const std::string& payload);

    /// 已退休会话在全部在途操作排空且不忙碌时释放最后持有引用（io 线程调用）
    void maybeReleaseRetired(const std::shared_ptr<Session>& session);

    asio::io_context& m_io;               ///< 单线程事件循环（不拥有）
    asio::thread_pool& m_pool;            ///< FFI 工作线程池（不拥有）
    std::filesystem::path m_storeBaseDir; ///< 透传给 bootstrap 与各会话 dispatcher
    std::unique_ptr<ipc::LocalAcceptor> m_acceptor;  ///< 监听接受器（listen 成功后有效）
    std::set<std::shared_ptr<Session>> m_sessions;   ///< 活动会话集合

    /// 已从 m_sessions 移除、等待在途 socket 操作以 aborted 排空后销毁的会话
    /// （LocalSocket 析构契约：先 close() 并等回调排空再销毁）
    std::set<std::shared_ptr<Session>> m_retiring;

    std::string m_socketName;             ///< 监听的 socket 名（决定回连缓存文件名）
    std::unique_ptr<FfiDispatcher> m_bootstrapDispatcher;  ///< 仅 listen 期 reconnectStoredInstances 用
    bool m_closed = false;                ///< close() 重入保护
};

#endif // IPCSERVER_H
