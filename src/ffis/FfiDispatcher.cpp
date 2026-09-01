/// @file FfiDispatcher.cpp
/// @brief FFI 调度层实现

#include "FfiDispatcher.h"
#include "EasyTierFFI.h"
#include "base64.h"
#include "log.h"
#include "tomlpp/toml.hpp"

#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>

namespace {

/// 配置服务器客户端是进程级资源：所有 IPC 会话共享同一个底层客户端。
/// 回调上下文使用进程级单例，避免某个会话 dispatcher 析构后回调仍访问其内存。
class ConfigServerBridge
{
public:
    static ConfigServerBridge& instance()
    {
        static ConfigServerBridge bridge;
        return bridge;
    }

    /// C 回调入口（由 EasyTier 工作线程触发），与 start_config_server_client 一起注册
    static void onEvent(const char* eventJson, void* userData)
    {
        if (eventJson == nullptr || userData == nullptr) return;
        auto* self = static_cast<ConfigServerBridge*>(userData);
        std::lock_guard<std::mutex> lock(self->m_mutex);
        // 队列设上限，超限丢弃最旧事件，避免远端持续推送耗尽 daemon 内存
        if (self->m_events.size() >= kMaxEvents)
            self->m_events.pop_front();
        self->m_events.emplace_back(eventJson);
    }

    /// 取走全部待处理事件（线程安全）
    std::vector<std::string> pollEvents()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> events;
        while (!m_events.empty()) {
            events.push_back(std::move(m_events.front()));
            m_events.pop_front();
        }
        return events;
    }

private:
    static constexpr std::size_t kMaxEvents = 1024;

    std::mutex m_mutex;
    std::deque<std::string> m_events;
};

/// 对齐 Qt QJsonValue::toString() 宽容语义：仅字符串类型返回内容，其余（含缺失）返回空串
std::string paramString(const nlohmann::json& params, const char* key)
{
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

/// 对齐 Qt QJsonValue::toBool(default) 宽容语义：仅布尔类型返回内容，其余返回缺省值
bool paramBool(const nlohmann::json& params, const char* key, bool fallback)
{
    const auto it = params.find(key);
    if (it == params.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

/// 对齐 Qt QJsonValue::toArray() 宽容语义：非数组视为空；元素仅字符串取值
std::vector<std::string> paramStringArray(const nlohmann::json& params, const char* key)
{
    std::vector<std::string> values;
    const auto it = params.find(key);
    if (it == params.end() || !it->is_array()) return values;
    for (const auto& v : *it) {
        if (v.is_string())
            values.push_back(v.get<std::string>());
        else
            values.emplace_back();
    }
    return values;
}

/// 构造失败结果（errorMessage → 响应的 error.message）
FfiDispatcher::DispatchResult makeError(const std::string& msg)
{
    FfiDispatcher::DispatchResult r;
    r.success = false;
    r.errorMessage = msg;
    return r;
}

/// 构造成功结果（data → 响应的 result 负载）
FfiDispatcher::DispatchResult makeSuccess(const nlohmann::json& data = nlohmann::json::object())
{
    FfiDispatcher::DispatchResult r;
    r.success = true;
    r.data = data;
    return r;
}

} // namespace

FfiDispatcher::FfiDispatcher(asio::thread_pool& pool, const std::string& socketName,
                             std::filesystem::path storeBaseDir)
    : m_reconnectStore(socketName, std::move(storeBaseDir))
    , m_pool(pool)
{
}

std::string FfiDispatcher::extractInstanceNameFromConfig(const std::string& cfgToml)
{
    try {
        toml::table config = toml::parse(cfgToml);
        if (auto name = config["instance_name"].value<std::string>())
            return *name;
    } catch (const toml::parse_error&) {
        return std::string();
    }
    return std::string();
}

void FfiDispatcher::reconnectStoredInstances()
{
    std::string error;
    bool enabled = false;
    if (!m_reconnectStore.autoReconnect(&enabled, &error)) {
        LOG_WARNING("Failed to read auto reconnect switch: " + error);
        return;
    }
    if (!enabled) return;

    std::vector<ReconnectStore::StoredInstance> instances;
    if (!m_reconnectStore.loadInstances(&instances, &error)) {
        LOG_WARNING("Failed to load reconnect instances: " + error);
        return;
    }

    std::vector<std::string> failedNames;
    for (const auto& instance : instances) {
        const std::string cfgDecoded = b64decode(instance.cfgBase64);
        const bool ok = EasyTierFFI::runNetworkInstance(cfgDecoded);
        if (ok) {
            LOG_INFO("Auto reconnected network instance: " + instance.instanceName);
        } else {
            failedNames.push_back(instance.instanceName);
            LOG_WARNING("Failed to auto reconnect instance " + instance.instanceName
                        + ": " + EasyTierFFI::getLastErrorMsg() + " (cached entry removed)");
        }
    }

    // 只删除回连失败的缓存项，成功项保留供下次启动继续回连
    if (!failedNames.empty() && !m_reconnectStore.removeInstances(failedNames, &error))
        LOG_WARNING("Failed to remove failed reconnect instances: " + error);
}

std::vector<std::string> FfiDispatcher::pollConfigEvents()
{
    return ConfigServerBridge::instance().pollEvents();
}

FfiDispatcher::DispatchResult FfiDispatcher::dispatch(const std::string& method,
                                                      const nlohmann::json& params)
{
    // 统一异常边界：任何 FFI/STL 异常都转为错误响应，避免穿透异步边界崩溃 daemon
    try {
        return dispatchImpl(method, params);
    } catch (const std::exception& e) {
        return makeError(std::string("dispatch error: ") + e.what());
    } catch (...) {
        return makeError("dispatch unknown error");
    }
}

FfiDispatcher::DispatchResult FfiDispatcher::dispatchImpl(const std::string& method,
                                                          const nlohmann::json& params)
{
    // parse_config：仅校验 TOML 语法合法性（不启动实例）；cfg_str 需 Base64 解码
    if (method == "parse_config") {
        if (!params.contains("cfg_str")) return makeError("missing cfg_str");
        const std::string cfgDecoded = b64decode(paramString(params, "cfg_str"));
        const bool ok = EasyTierFFI::parseConfig(cfgDecoded);
        return ok ? makeSuccess() : makeError(EasyTierFFI::getLastErrorMsg());
    }

    // run_network_instance：启动实例；成功且 TOML 含 instance_name 时写入回连缓存
    //（缓存的是原始 Base64 cfg_str，回连时直接复用）
    if (method == "run_network_instance") {
        if (!params.contains("cfg_str")) return makeError("missing cfg_str");
        const std::string cfgBase64 = paramString(params, "cfg_str");
        const std::string cfgDecoded = b64decode(cfgBase64);
        const bool ok = EasyTierFFI::runNetworkInstance(cfgDecoded);
        if (ok) {
            const std::string instanceName = extractInstanceNameFromConfig(cfgDecoded);
            if (!instanceName.empty()) {
                std::string error;
                if (!m_reconnectStore.saveInstance(instanceName, cfgBase64, &error))
                    LOG_WARNING("Failed to save reconnect instance " + instanceName
                                + ": " + error);
            }
        }
        return ok ? makeSuccess() : makeError(EasyTierFFI::getLastErrorMsg());
    }

    // delete_network_instance：停止实例并同步移除对应回连缓存（未知名称由 FFI 忽略）
    if (method == "delete_network_instance") {
        const std::vector<std::string> names = paramStringArray(params, "inst_names");
        const bool ok = EasyTierFFI::deleteNetworkInstance(names);
        if (ok) {
            std::string error;
            if (!m_reconnectStore.removeInstances(names, &error))
                LOG_WARNING("Failed to remove reconnect instances: " + error);
        }
        return ok ? makeSuccess() : makeError(EasyTierFFI::getLastErrorMsg());
    }

    if (method == "list_instances") {
        std::vector<EasyTierFFI::KVPair> infos;
        const bool ok = EasyTierFFI::listInstances(infos);
        if (!ok) return makeError(EasyTierFFI::getLastErrorMsg());
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& kv : infos) {
            // value 为实例名称/ID 原始字符串，不做 Base64 编码
            arr.push_back(nlohmann::json{{"key", kv.key}, {"value", kv.value}});
        }
        nlohmann::json data;
        data["instances"] = arr;
        return makeSuccess(data);
    }

    if (method == "collect_network_infos") {
        // max_length 严格校验：必须是范围内的整数，拒绝负数/超大值/非整数，
        // 防止转换为 size_t 后触发灾难性内存分配
        constexpr std::int64_t kMaxCollectLength = 1024;
        std::int64_t maxLen = 64; // 默认值，协议兼容
        // 对齐 Qt 版语义：字段缺失或显式 null 时用默认值，其余类型严格校验
        const auto maxLenIt = params.find("max_length");
        if (maxLenIt != params.end() && !maxLenIt->is_null()) {
            const auto& v = *maxLenIt;
            if (!v.is_number()) return makeError("invalid max_length");
            const double d = v.get<double>(); // 整数/无符号/浮点统一取 double
            if (d < 0 || d > static_cast<double>(kMaxCollectLength)
                || static_cast<std::int64_t>(d) != d)
                return makeError("invalid max_length");
            maxLen = static_cast<std::int64_t>(d);
        }

        std::size_t maxLenSize = static_cast<std::size_t>(maxLen);
        std::vector<EasyTierFFI::KVPair> infos;
        const int count = EasyTierFFI::collectNetworkInfos(infos, maxLenSize);
        if (count < 0) return makeError(EasyTierFFI::getLastErrorMsg());
        nlohmann::json arr = nlohmann::json::array();
        for (int i = 0; i < count; ++i) {
            arr.push_back(nlohmann::json{
                {"key", infos[static_cast<std::size_t>(i)].key},
                {"value", b64encode(infos[static_cast<std::size_t>(i)].value)}});
        }
        nlohmann::json data;
        data["instances"] = arr;
        data["count"] = count;
        return makeSuccess(data);
    }

    // call_json_rpc：转发 EasyTier 内部 RPC——service/method/domain 按原文传输，
    // payload 解码后送入、response 编码后返回
    if (method == "call_json_rpc") {
        const std::string service = paramString(params, "service_name");
        const std::string rpcMethod = paramString(params, "method_name");
        const std::string domain = paramString(params, "domain_name");
        const std::string payload = b64decode(paramString(params, "payload"));

        std::string outResponse;
        const bool ok = EasyTierFFI::callJsonRpc(service, rpcMethod, domain, payload, outResponse);

        if (!ok) return makeError(EasyTierFFI::getLastErrorMsg());
        nlohmann::json data;
        data["response"] = b64encode(outResponse);
        return makeSuccess(data);
    }

    if (method == "start_config_server_client") {
        // 这些配置服务器字段在 IPC 协议中约定为 Base64 编码。
        const std::string url = b64decode(paramString(params, "config_server_url"));
        const std::string hostname = b64decode(paramString(params, "hostname"));
        const std::string machineId = b64decode(paramString(params, "machine_id"));
        const bool secure = paramBool(params, "secure_mode", false);

        const bool ok = EasyTierFFI::startConfigServerClient(
            url, hostname, machineId, secure,
            &ConfigServerBridge::onEvent, &ConfigServerBridge::instance());

        return ok ? makeSuccess() : makeError(EasyTierFFI::getLastErrorMsg());
    }

    // stop_config_server_client：断开配置服务器（事件队列中已缓存的事件保留）
    if (method == "stop_config_server_client") {
        const bool ok = EasyTierFFI::stopConfigServerClient();
        return ok ? makeSuccess() : makeError(EasyTierFFI::getLastErrorMsg());
    }

    // is_config_server_client_connected：查询配置服务器连接状态
    if (method == "is_config_server_client_connected") {
        const bool connected = EasyTierFFI::isConfigServerClientConnected();
        nlohmann::json data;
        data["connected"] = connected;
        return makeSuccess(data);
    }

    // get_error_msg：返回 FFI 累积的全部错误（逐条 Base64 编码）
    if (method == "get_error_msg") {
        const auto errors = EasyTierFFI::getErrorMsg();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : errors)
            arr.push_back(b64encode(e));
        nlohmann::json data;
        data["errors"] = arr;
        return makeSuccess(data);
    }

    // get_last_error_msg：仅返回最近一条错误（Base64 编码）
    if (method == "get_last_error_msg") {
        const std::string msg = EasyTierFFI::getLastErrorMsg();
        nlohmann::json data;
        data["message"] = b64encode(msg);
        return makeSuccess(data);
    }

    // poll_config_events：取走配置服务器事件队列（IPC 层附加方法，事件原文返回）
    if (method == "poll_config_events") {
        const std::vector<std::string> events = pollConfigEvents();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : events)
            arr.push_back(e);
        nlohmann::json data;
        data["events"] = arr;
        return makeSuccess(data);
    }

    // set_auto_reconnect / get_auto_reconnect：读写回连总开关（直接落盘）
    if (method == "set_auto_reconnect") {
        std::string error;
        const bool enabled = paramBool(params, "enabled", false);
        if (!m_reconnectStore.setAutoReconnect(enabled, &error))
            return makeError(error);
        nlohmann::json data;
        data["autoReconnect"] = enabled;
        return makeSuccess(data);
    }

    if (method == "get_auto_reconnect") {
        std::string error;
        bool enabled = false;
        if (!m_reconnectStore.autoReconnect(&enabled, &error))
            return makeError(error);
        nlohmann::json data;
        data["autoReconnect"] = enabled;
        return makeSuccess(data);
    }

    return makeError("unknown method: " + method);
}

std::future<FfiDispatcher::DispatchResult> FfiDispatcher::dispatchAsync(
    asio::io_context& io, const std::string& method, const nlohmann::json& params,
    std::function<void(DispatchResult)> completion)
{
    // 生命周期契约见头文件注释：调用方须保证本对象存活至 completion 执行完毕。
    // pool 任务先向 io 投递 completion 再给 promise 置值，
    // 因此 future 就绪 ⇒ completion 已进入 io 队列（happens-before）。
    auto promise = std::make_shared<std::promise<DispatchResult>>();
    std::future<DispatchResult> future = promise->get_future();

    asio::post(m_pool, [this, &io, method, params, completion, promise]() {
        auto result = std::make_shared<DispatchResult>(dispatch(method, params));
        asio::post(io, [result, completion]() {
            if (completion) completion(*result);
        });
        promise->set_value(*result);
    });

    return future;
}
