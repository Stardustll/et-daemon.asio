/// @file tst_dispatcher.cpp
/// @brief FfiDispatcher 调度层单元测试（移植父工程 Qt 版用例）
///
/// 测试覆盖：14 个已知方法调度、未知方法错误、参数缺失、事件轮询、
/// max_length 严格校验、自动回连开关往返、TOML 根域实例名提取、
/// 回连缓存清理（禁用不动缓存 / 失败项删除）、dispatchAsync 异步分发

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "FfiDispatcher.h"
#include "daemon/ReconnectStore.h"

namespace {

/// 每个用例独立的临时基准目录：
/// <temp>/qtet-dispatcher-<runId>-<counter>/，析构时递归清理
class TempBaseDir
{
public:
    TempBaseDir()
    {
        static const auto runId =
            std::chrono::steady_clock::now().time_since_epoch().count();
        static int counter = 0;

        std::error_code ec;
        m_dir = std::filesystem::temp_directory_path(ec) /
                ("qtet-dispatcher-" + std::to_string(runId) + "-" +
                 std::to_string(++counter));
        std::filesystem::create_directories(m_dir, ec);
    }

    ~TempBaseDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    TempBaseDir(const TempBaseDir&) = delete;
    TempBaseDir& operator=(const TempBaseDir&) = delete;

    const std::filesystem::path& path() const { return m_dir; }

private:
    std::filesystem::path m_dir;
};

} // namespace

TEST_CASE("unknown method 返回 unknown 错误")
{
    asio::thread_pool pool(1);
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-unknown", dir.path());

    const FfiDispatcher::DispatchResult r =
        dispatcher.dispatch("nonexistent_method", nlohmann::json::object());
    CHECK_FALSE(r.success);
    CHECK(r.errorMessage.find("unknown") != std::string::npos);
}

TEST_CASE("14 个已知方法均返回结构化 DispatchResult")
{
    asio::thread_pool pool(1);
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-all-methods", dir.path());

    const std::vector<std::string> methods = {
        "parse_config", "run_network_instance", "delete_network_instance",
        "list_instances", "collect_network_infos", "call_json_rpc",
        "start_config_server_client", "stop_config_server_client",
        "is_config_server_client_connected", "get_error_msg",
        "get_last_error_msg", "poll_config_events",
        "set_auto_reconnect", "get_auto_reconnect"
    };

    for (const auto& m : methods) {
        nlohmann::json params = nlohmann::json::object();
        if (m == "set_auto_reconnect")
            params["enabled"] = true;

        INFO("method: ", m);
        const FfiDispatcher::DispatchResult r = dispatcher.dispatch(m, params);
        const bool hasOutcome = r.success || !r.errorMessage.empty();
        CHECK(hasOutcome);
    }
}

TEST_CASE("parse_config 缺少 cfg_str 参数时报错")
{
    asio::thread_pool pool(1);
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-missing-param", dir.path());

    const FfiDispatcher::DispatchResult r =
        dispatcher.dispatch("parse_config", nlohmann::json::object());
    CHECK_FALSE(r.success);
    CHECK(r.errorMessage == "missing cfg_str");
}

TEST_CASE("pollConfigEvents 初始为空")
{
    CHECK(FfiDispatcher::pollConfigEvents().empty());
}

TEST_CASE("collect_network_infos 缺省 max_length 成功且 count >= 0")
{
    asio::thread_pool pool(1);
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-collect-default", dir.path());

    const FfiDispatcher::DispatchResult r =
        dispatcher.dispatch("collect_network_infos", nlohmann::json::object());
    REQUIRE(r.success);
    CHECK(r.data.at("count").get<int>() >= 0);
}

TEST_CASE("collect_network_infos 拒绝非法 max_length")
{
    asio::thread_pool pool(1);
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-collect-invalid", dir.path());

    const std::vector<nlohmann::json> invalidValues = {
        -1,            // 负数
        1000000000,    // 超过上限 1024
        2.5            // 非整数
    };

    for (const auto& value : invalidValues) {
        INFO("max_length: ", value.dump());
        const FfiDispatcher::DispatchResult r = dispatcher.dispatch(
            "collect_network_infos", {{"max_length", value}});
        CHECK_FALSE(r.success);
        CHECK(r.errorMessage.find("max_length") != std::string::npos);
    }
}

TEST_CASE("set_auto_reconnect / get_auto_reconnect 往返")
{
    asio::thread_pool pool(1);
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-auto-reconnect", dir.path());

    const FfiDispatcher::DispatchResult set =
        dispatcher.dispatch("set_auto_reconnect", {{"enabled", true}});
    REQUIRE(set.success);
    CHECK(set.data.at("autoReconnect").get<bool>());

    const FfiDispatcher::DispatchResult get =
        dispatcher.dispatch("get_auto_reconnect", nlohmann::json::object());
    REQUIRE(get.success);
    CHECK(get.data.at("autoReconnect").get<bool>());
}

TEST_CASE("extractInstanceNameFromConfig 仅取 TOML 根域 instance_name")
{
    const std::string toml = R"(instance_name = "net-root"

[instance]
instance_name = "nested-ignored"
)";

    CHECK(FfiDispatcher::extractInstanceNameFromConfig(toml) == "net-root");
}

TEST_CASE("extractInstanceNameFromConfig 根域缺失时返回空串")
{
    const std::string toml = R"([instance]
instance_name = "nested-only"
)";

    CHECK(FfiDispatcher::extractInstanceNameFromConfig(toml).empty());
}

TEST_CASE("reconnectStoredInstances 开关禁用时不动缓存")
{
    asio::thread_pool pool(1);
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-disabled", dir.path());
    ReconnectStore store("sock-disabled", dir.path());

    REQUIRE(store.saveInstance("net-a", "aW52YWxpZCB0b21s"));

    dispatcher.reconnectStoredInstances();

    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances));
    CHECK(instances.size() == 1);
}

TEST_CASE("reconnectStoredInstances 回连失败的缓存项被删除")
{
    asio::thread_pool pool(1);
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-fail", dir.path());
    ReconnectStore store("sock-fail", dir.path());

    REQUIRE(store.setAutoReconnect(true));
    REQUIRE(store.saveInstance("net-a", "aW52YWxpZCB0b21s"));

    dispatcher.reconnectStoredInstances();

    // 回连失败（配置无效）的缓存项应被删除，避免下次启动反复重试坏配置
    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances));
    CHECK(instances.empty());
}

TEST_CASE("dispatchAsync 在 io 上下文线程回调 completion 并回传结果")
{
    asio::thread_pool pool(1);
    asio::io_context io;
    TempBaseDir dir;
    FfiDispatcher dispatcher(pool, "sock-async", dir.path());

    REQUIRE(dispatcher.dispatch("set_auto_reconnect", {{"enabled", true}}).success);

    std::atomic<bool> done{false};
    bool cbSuccess = false;
    bool cbAutoReconnect = false;
    std::thread::id callbackThread{};

    std::future<FfiDispatcher::DispatchResult> future = dispatcher.dispatchAsync(
        io, "get_auto_reconnect", nlohmann::json::object(),
        [&](FfiDispatcher::DispatchResult r) {
            callbackThread = std::this_thread::get_id();
            cbSuccess = r.success;
            if (r.success)
                cbAutoReconnect = r.data.value("autoReconnect", false);
            done = true;
        });

    // future 就绪 ⇒ pool 任务已完成且 completion 已投递进 io 队列
    REQUIRE(future.get().success);
    io.run();
    REQUIRE(done.load());
    CHECK(cbSuccess);
    CHECK(cbAutoReconnect);
    // completion 必须在运行 io 上下文的线程执行（此处为主线程驱动 io.run()）
    CHECK(callbackThread == std::this_thread::get_id());
}
