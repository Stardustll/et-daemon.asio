/// @file FfiDispatcher.h
/// @brief FFI 调度层：将 IPC 方法名映射到 EasyTierFFI 函数调用（无 Qt 依赖版本）
///
/// 负责：
/// 1. 14 个方法的名称 ⇒ EasyTierFFI 函数的分发
/// 2. 参数从 JSON 到 C++ 的转换（按字段处理 Base64 解码）
/// 3. 返回值从 C++ 到 JSON 的转换（按字段处理 Base64 编码）
/// 4. 配置服务器事件回调桥接（C 回调 → 进程级事件队列）
/// 5. 提供异步调度接口，将阻塞式 FFI 调用移至线程池执行

#ifndef FFIDISPATCHER_H
#define FFIDISPATCHER_H

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

#include <filesystem>
#include <future>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "daemon/ReconnectStore.h"

namespace asio = boost::asio;

/// FFI 调度器类
class FfiDispatcher
{
public:
    /// 调度结果
    struct DispatchResult
    {
        bool success = false;
        nlohmann::json data = nlohmann::json::object();
        std::string errorMessage;
    };

    /// pool 供 dispatchAsync 投递；同步 dispatch() 不使用 pool（测试直接调用）
    /// socketName 决定自动回连缓存文件名；storeBaseDir 为空 → exeDirectory()
    FfiDispatcher(asio::thread_pool& pool, const std::string& socketName,
                  std::filesystem::path storeBaseDir = {});

    /// 同步调度：在当前线程直接调用 FFI 函数（用于测试），含统一异常边界
    DispatchResult dispatch(const std::string& method, const nlohmann::json& params);

    /// 异步调度：asio::post(pool) 执行 → promise 置值 → asio::post(io) 调 completion
    /// （completion 在驱动 io 的线程执行）
    ///
    /// 生命周期约定：调用方须保证 dispatcher 存活至 completion 执行完毕；
    /// 会话侧的 std::weak_ptr 守护由 IpcServer 注入（见父工程约定）。
    /// 若调用方放弃返回的 future（销毁），不会产生危害。
    std::future<DispatchResult> dispatchAsync(asio::io_context& io, const std::string& method,
                                              const nlohmann::json& params,
                                              std::function<void(DispatchResult)> completion);

    /// 获取配置服务器推送的待处理事件列表（进程级单例，线程安全）
    static std::vector<std::string> pollConfigEvents();

    /// 从 TOML 根域提取 instance_name；解析失败或字段不存在时返回空字符串
    static std::string extractInstanceNameFromConfig(const std::string& cfgToml);

    /// 启动时读取缓存实例并逐个尝试回连；成功保留缓存项，失败删除对应缓存项
    void reconnectStoredInstances();

private:
    /// 内部实现（不含异常边界），由 dispatch() 包一层捕获
    DispatchResult dispatchImpl(const std::string& method, const nlohmann::json& params);

    ReconnectStore m_reconnectStore;
    asio::thread_pool& m_pool;
};

#endif // FFIDISPATCHER_H
