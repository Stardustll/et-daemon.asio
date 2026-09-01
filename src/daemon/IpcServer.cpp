/// @file IpcServer.cpp
/// @brief IPC 服务端实现（无 Qt 依赖版本）
///
/// 会话语义逐项对齐父工程 src/daemon/IpcServer.cpp（Qt 版）：
/// - 每会话请求串行执行（busy 标志），帧在缓冲区累积
/// - 非法帧（零长度/超长）直接断开；method 为空返回错误响应后继续同批后续帧
/// - 断开后在途 FFI 调用照常完成但不写响应，completion 兜底清理会话
/// - 回调全部在 io 单线程执行，状态无锁；回调经 weak_ptr<Session> 守护

#include "daemon/IpcServer.h"
#include "base64.h"
#include "ipc/LocalEndpoint.h"
#include "log.h"

#include <array>
#include <exception>
#include <system_error>
#include <utility>
#include <vector>

IpcServer::IpcServer(asio::io_context& io, asio::thread_pool& pool,
                     std::filesystem::path storeBaseDir)
    : m_io(io)
    , m_pool(pool)
    , m_storeBaseDir(std::move(storeBaseDir))
{
}

IpcServer::~IpcServer()
{
    close();
}

bool IpcServer::listen(const std::string& name, std::string& error)
{
    if (m_closed) {
        error = "IpcServer is closed";
        return false;
    }
    if (m_acceptor) {
        error = "IpcServer is already listening";
        return false;
    }

    // 探测 socket 是否已被其他 daemon 实例监听：能连上说明已有活动实例，
    // 直接失败退出，避免 unlink 破坏正在运行的实例
    if (ipc::LocalAcceptor::hasActiveInstance(name)) {
        error = "Another qtet-daemon instance is already running on " + name;
        LOG_ERROR(error);
        return false;
    }

    m_acceptor = std::make_unique<ipc::LocalAcceptor>(m_io);
    if (!m_acceptor->listen(name, error)) {
        LOG_ERROR("Failed to listen on " + name + ": " + error);
        m_acceptor.reset();
        return false;
    }

    m_socketName = name;
    m_bootstrapDispatcher = std::make_unique<FfiDispatcher>(m_pool, name, m_storeBaseDir);
    m_bootstrapDispatcher->reconnectStoredInstances();
    doAccept();
    LOG_INFO("qtet-daemon listening on " + ipc::localEndpointPath(name));
    return true;
}

void IpcServer::close()
{
    if (m_closed)
        return;
    m_closed = true;

    // 关闭监听端点：在途 asyncAccept 以 operation_aborted 完成，
    // 其回调见 m_closed 直接返回（不重新挂接受）
    if (m_acceptor)
        m_acceptor->close();

    // 等待所有在途 FFI 调用完成，避免 dispatcher 在线程池任务执行期间被析构。
    // 阻塞 io 线程直至 pool 侧任务置值是可接受的（Qt 版 close() 同样等待；
    // pool 任务给 promise 置值不依赖 io 线程推进，不会死锁）。pool 的 join
    // 由 main 负责，此处不做
    for (const auto& session : m_sessions) {
        if (session->inFlight.valid())
            session->inFlight.wait();
    }

    const auto sessions = m_sessions; // 拷贝持有，cleanupSession 会从 m_sessions 移除
    for (const auto& session : sessions) {
        session->disconnected = true;
        session->cleanupRequested = true;
        if (session->socket)
            session->socket->close();
        cleanupSession(session);
    }
    m_sessions.clear();
}

void IpcServer::doAccept()
{
    if (m_closed || !m_acceptor)
        return;

    // pending socket 由回调持有的 shared_ptr 保活至接受完成，随后移交会话独占
    auto pendingSocket = std::make_shared<ipc::LocalSocket>(m_io);
    m_acceptor->asyncAccept(*pendingSocket,
                            [this, pendingSocket](std::error_code ec) mutable {
        if (m_closed)
            return;

        if (ec) {
            LOG_WARNING("Failed to accept client connection: " + ec.message());
        } else {
            auto session = std::make_shared<Session>();
            // 所有权经移动移交会话：shared_ptr 引用计数 1→1，
            // 旧持有者置空不再析构对象（reset(get()) + 置空会立即析构导致悬空）
            session->socket = std::move(pendingSocket);
            session->dispatcher =
                std::make_unique<FfiDispatcher>(m_pool, m_socketName, m_storeBaseDir);
            m_sessions.insert(session);
            LOG_INFO("Client connected");
            startRead(session);
        }

        // 继续挂下一个接受（m_closed 时内部直接返回）
        doAccept();
    });
}

void IpcServer::startRead(const std::shared_ptr<Session>& session)
{
    if (!session || !session->socket || session->disconnected || session->cleanupRequested)
        return;

    // 缓冲区堆持有：asyncReadSome 接受非占有视图，须存活至回调触发
    auto chunk = std::make_shared<std::array<char, 65536>>();
    ++session->pendingSocketOps;
    auto weak = std::weak_ptr<Session>(session);
    session->socket->asyncReadSome(asio::buffer(*chunk),
                                   [this, weak, chunk](std::error_code ec, std::size_t bytes) {
                                       auto session = weak.lock();
                                       if (!session)
                                           return;
                                       --session->pendingSocketOps;

                                       // 我方主动 close 的排空信号（非法帧断开 /
                                       // 会话清理 / 服务关闭）：作为退休会话的
                                       // 最后事件，空闲即清理
                                       if (ec == std::errc::operation_canceled) {
                                           if (!session->busy && session->cleanupRequested)
                                               cleanupSession(session);
                                           else
                                               maybeReleaseRetired(session);
                                           return;
                                       }

                                       if (ec || bytes == 0) {
                                           onDisconnect(session);
                                           return;
                                       }

                                       session->buffer.append(chunk->data(), bytes);
                                       processNextFrame(session);

                                        // busy 时不继续读：FFI 调用在途期间新数据滞留
                                        // 内核 socket 缓冲，由其流控约束发送方，避免用户态
                                        // 帧缓冲无界增长（对齐 Qt 版 busy 即不读）；
                                        // 空闲后的补挂读见 startDispatch 的 completion
                                        if (!session->busy && !session->disconnected
                                            && !session->cleanupRequested) {
                                            startRead(session); // 继续读，busy 时帧在缓冲区累积
                                        } else if (session->cleanupRequested) {
                                            // 非法帧在空闲会话上断开时无在途操作，close()
                                            // 不触发任何回调，须在此直接清理；若仍有在途
                                            // 操作（如同批排队写），cleanupSession 移入退休
                                            // 集合，由排空回调释放（幂等，可安全重入）
                                            cleanupSession(session);
                                        } else {
                                            maybeReleaseRetired(session);
                                        }
                                   });
}

void IpcServer::processNextFrame(const std::shared_ptr<Session>& session)
{
    if (!session || !session->socket)
        return;
    if (session->busy || session->disconnected)
        return;

    // 循环处理缓冲区内所有可用的完整帧，直到数据不足或提交了异步调用
    while (!session->busy) {
        std::string frame;
        const proto::FrameStatus status = proto::parseFrame(session->buffer, frame);

        if (status == proto::FrameStatus::Incomplete)
            break;

        // 非法帧（零长度/超长）：直接断开，避免缓冲区被恶意撑大或会话卡死。
        // 对齐 Qt 版 abort()：置位标志 + close；空闲会话上本 read 回调即最后
        // 事件，close() 不会产生 aborted 排空，清理由回调尾部 cleanupSession 执行
        if (status == proto::FrameStatus::Invalid) {
            LOG_WARNING("Invalid IPC frame from client, disconnecting");
            session->disconnected = true;
            session->cleanupRequested = true;
            session->socket->close();
            break;
        }

        proto::IpcRequest req = proto::deserializeRequest(frame);
        if (req.method.empty()) {
            proto::IpcResponse resp;
            resp.id = req.id;
            resp.method = req.method;
            resp.error = {{ "message", "invalid request" }};
            writeResponse(session, proto::serializeResponse(resp));
            continue; // 继续处理后续帧，不阻塞同批次请求
        }

        startDispatch(session, req.id, req.method, req.params);
        return;
    }
}

void IpcServer::startDispatch(const std::shared_ptr<Session>& session,
                              proto::IpcId id, const std::string& method,
                              const nlohmann::json& params)
{
    // 保存请求标识，标记忙碌，提交到线程池异步执行
    session->pendingId = id;
    session->pendingMethod = method;
    session->pendingParams = params;
    session->busy = true;

    auto weak = session->weak_from_this();
    session->inFlight = session->dispatcher->dispatchAsync(
        m_io, method, params, [this, weak](FfiDispatcher::DispatchResult) {
            auto session = weak.lock();
            if (!session)
                return;

            session->busy = false;

            // FFI 调用结果已在线程池中计算完成，回 io 线程写响应。
            // 防御性地捕获 future 可能携带的异常，避免异常穿透事件回调导致
            // daemon 崩溃。pool 任务先投递 completion 再给 promise 置值，
            // 此处 get() 至多短暂阻塞且不会死锁（置值不依赖 io 线程）
            FfiDispatcher::DispatchResult result;
            try {
                if (session->inFlight.valid()) {
                    result = session->inFlight.get();
                } else {
                    result.success = false;
                    result.errorMessage = "dispatch result unavailable";
                }
            } catch (const std::exception& e) {
                result.success = false;
                result.errorMessage = std::string("dispatch exception: ") + e.what();
            } catch (...) {
                result.success = false;
                result.errorMessage = "dispatch unknown exception";
            }

            if (!session->disconnected && session->socket) {
                proto::IpcResponse resp;
                resp.id = session->pendingId;
                resp.method = session->pendingMethod;
                if (result.success)
                    resp.result = result.data;
                else
                    resp.error = {{ "message", result.errorMessage }};
                writeResponse(session, proto::serializeResponse(resp));
            }

            // 前端启动/停止命令的成败输出控制台日志
            logClientDispatchOutcome(result, session->pendingParams, session->pendingMethod);

            // 检查是否有更多帧在等待
            if (!session->disconnected)
                processNextFrame(session);

            if (session->cleanupRequested) {
                cleanupSession(session);
            } else if (!session->busy && !session->disconnected) {
                // FFI 在途期间读循环停摆（背压，见 startRead 尾部注释）：
                // 缓冲区已无完整帧且会话空闲时补挂读，继续接收后续数据。
                // 若 processNextFrame 提交了新调用（busy），由下一轮 completion 补挂
                startRead(session);
            }
        });
}

void IpcServer::logClientDispatchOutcome(const FfiDispatcher::DispatchResult& result,
                                         const nlohmann::json& params,
                                         const std::string& method)
{
    if (method == "run_network_instance") {
        std::string cfgStr;
        const auto it = params.find("cfg_str");
        if (it != params.end() && it->is_string())
            cfgStr = it->get<std::string>();
        const std::string instanceName =
            FfiDispatcher::extractInstanceNameFromConfig(b64decode(cfgStr));

        if (result.success)
            LOG_INFO("Network instance started: "
                     + (instanceName.empty() ? std::string("(no instance_name in config)")
                                             : instanceName));
        else
            LOG_WARNING("Failed to start network instance: " + result.errorMessage);
    } else if (method == "delete_network_instance") {
        std::vector<std::string> names;
        const auto it = params.find("inst_names");
        if (it != params.end() && it->is_array()) {
            for (const auto& value : *it)
                names.push_back(value.is_string() ? value.get<std::string>() : std::string());
        }

        if (result.success) {
            if (!names.empty()) {
                std::string joined = names.front();
                for (std::size_t i = 1; i < names.size(); ++i)
                    joined += ", " + names[i];
                LOG_INFO("Network instance(s) stopped: " + joined);
            }
        } else {
            LOG_WARNING("Failed to stop network instance(s): " + result.errorMessage);
        }
    }
}

void IpcServer::onDisconnect(const std::shared_ptr<Session>& session)
{
    if (!session)
        return;

    session->disconnected = true;
    session->cleanupRequested = true;
    // busy 会话保留至 completion 兜底清理；否则立即清理
    if (!session->busy)
        cleanupSession(session);
    LOG_INFO("Client disconnected");
}

void IpcServer::writeResponse(const std::shared_ptr<Session>& session, const std::string& payload)
{
    if (!session || !session->socket || session->disconnected)
        return;

    ++session->pendingSocketOps;
    auto weak = std::weak_ptr<Session>(session);
    // 打包帧：4 字节大端长度前缀 + JSON payload（对齐 Qt 版 Protocol::writeFrame）
    session->socket->asyncWrite(proto::encodeFrame(payload), [this, weak](std::error_code ec) {
        auto session = weak.lock();
        if (!session)
            return;
        --session->pendingSocketOps;

        // 我方 close 触发的排队写中止：作为退休会话的排空事件
        if (ec == std::errc::operation_canceled) {
            if (!session->busy && session->cleanupRequested)
                cleanupSession(session);
            else
                maybeReleaseRetired(session);
            return;
        }

        if (ec) {
            onDisconnect(session);
            return;
        }
    });
}

void IpcServer::cleanupSession(const std::shared_ptr<Session>& session)
{
    if (!session)
        return;

    // 仅仍在活动集合时执行主体（幂等）；busy 会话不应到达此处
    // （onDisconnect/completion 均在 !busy 时才调用，close() 则先 wait 再清理）
    if (m_sessions.erase(session) > 0) {
        session->disconnected = true;
        session->cleanupRequested = true;
        // 移入退休集合保持持有：close() 触发在途读/写以 aborted 排空后，
        // 由 maybeReleaseRetired 释放最后引用（LocalSocket 析构契约）
        m_retiring.insert(session);
        if (session->socket)
            session->socket->close();
    }
    maybeReleaseRetired(session);
}

void IpcServer::maybeReleaseRetired(const std::shared_ptr<Session>& session)
{
    if (m_retiring.count(session) == 0)
        return;
    // busy（completion 尚未执行）或在途 socket 操作未排空时保留持有
    if (session->busy || session->pendingSocketOps > 0)
        return;
    m_retiring.erase(session); // 最后持有引用释放 → 会话（含 socket/dispatcher）析构
}
