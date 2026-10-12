/// @file tst_integration.cpp
/// @brief IPC 端到端集成测试（移植父工程 Qt 版全部 11 用例）
///
/// 测试基建：
/// - 服务端：IntegrationFixture（doctest 每用例独立实例）——io_context +
///   work_guard 跑在专职服务端线程，thread_pool(2) 供 FFI 调度，
///   IpcServer 以临时目录 storeBaseDir 监听固定裸 socket 名（见 kSocketName）
/// - 客户端：TestClient——独立 io_context + 后台读循环线程，缓冲区累积帧，
///   awaitFrame 以互斥量 + 条件变量等待（先消费已缓冲的完整帧，再等新数据），
///   超时/对端断开返回 nullopt

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "base64.h"
#include "daemon/IpcServer.h"
#include "daemon/Protocol.h"
#include "ipc/LocalEndpoint.h"
#include "log.h"

namespace {

/// 测试 socket 名（固定裸名，不使用 DAEMON_SOCKET_NAME 拼接：macOS 下后者为
/// 绝对路径 /var/run/qtet-daemon.sock，拼接后将试图在 /var/run 中 bind，非 root
/// 会 EPERM 失败。裸名由 localEndpointPath 解析到临时目录，各平台均可写）
constexpr const char* kSocketName = "qtet-daemon-integration-test";

/// ───────────────────────── 测试客户端 ─────────────────────────
/// 独立 io_context + 后台读循环线程：帧到达即入缓冲区并唤醒等待者；
/// 异步写经 post 切到 io 线程（LocalSocket 非线程安全）
class TestClient
{
public:
    TestClient()
        : m_sock(m_io)
    {
    }

    ~TestClient()
    {
        m_io.stop(); // 停止事件循环；在途回调随 io 销毁丢弃（不执行）
        if (m_thread.joinable())
            m_thread.join();
        // io 已停止且线程已 join，此后无回调执行，销毁 socket 安全
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    /// 同步连接测试 socket
    bool connect()
    {
        if (m_connected)
            return true;
        std::string error;
        if (!m_sock.connect(kSocketName, error))
            return false;
        // 先投递读循环再启动线程：connect 后始终有在途读，run() 不空转退出
        asio::post(m_io, [this] { startReadLoop(); });
        m_thread = std::thread([this] { m_io.run(); });
        m_connected = true;
        return true;
    }

    /// 发送原始字节（帧头/畸形数据均用它）
    void sendRaw(const std::string& bytes)
    {
        asio::post(m_io, [this, bytes] {
            m_sock.asyncWrite(bytes, [](std::error_code) {});
        });
    }

    /// 发送一个帧协议请求
    void sendRequest(const std::string& method, const nlohmann::json& params,
                     proto::IpcId id = 1)
    {
        proto::IpcRequest req;
        req.id = id;
        req.method = method;
        req.params = params;
        sendRaw(proto::encodeFrame(proto::serializeRequest(req)));
    }

    /// 连接 + 发送 + 等响应的便捷路径；失败/超时返回 nullopt（已连接则复用连接）
    std::optional<std::string> sendAndReceive(const std::string& method,
                                              const nlohmann::json& params = {},
                                              proto::IpcId id = 1)
    {
        if (!m_connected && !connect())
            return std::nullopt;
        sendRequest(method, params, id);
        return awaitFrame();
    }

    /// 等待下一帧：先消费缓冲区已累积的完整帧，不足时等新数据；
    /// 超时或对端断开返回 nullopt
    std::optional<std::string> awaitFrame(int timeoutMs = 5000)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        std::string payload;
        if (proto::parseFrame(m_buffer, payload) == proto::FrameStatus::Complete)
            return payload;

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (m_peerClosed)
                return std::nullopt;
            if (m_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                // 超时前最后检查一次（数据可能恰在唤醒判定期间到达）
                if (proto::parseFrame(m_buffer, payload) == proto::FrameStatus::Complete)
                    return payload;
                return std::nullopt;
            }
            if (proto::parseFrame(m_buffer, payload) == proto::FrameStatus::Complete)
                return payload;
        }
    }

    /// 对端是否已断开（读到 eof/error）
    bool peerClosed() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_peerClosed;
    }

private:
    /// 后台读循环：数据入缓冲区并通知；eof/error 置断开标志并通知（循环终止）
    void startReadLoop()
    {
        auto chunk = std::make_shared<std::array<char, 65536>>();
        readLoop(std::move(chunk));
    }

    void readLoop(const std::shared_ptr<std::array<char, 65536>>& chunk)
    {
        m_sock.asyncReadSome(asio::buffer(*chunk),
                             [this, chunk](std::error_code ec, std::size_t bytes) {
                                 if (ec || bytes == 0) {
                                     std::lock_guard<std::mutex> lock(m_mutex);
                                     m_peerClosed = true;
                                     m_cv.notify_all();
                                     return; // 不再续挂读，io 耗尽后线程退出
                                 }
                                 {
                                     std::lock_guard<std::mutex> lock(m_mutex);
                                     m_buffer.append(chunk->data(), bytes);
                                 }
                                 m_cv.notify_all();
                                 readLoop(chunk);
                             });
    }

    asio::io_context m_io;
    ipc::LocalSocket m_sock;
    std::thread m_thread; // 本类专用 io 线程（dtor join 后成员销毁）

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_buffer;    // 已接收未消费的字节（含帧头）
    bool m_peerClosed = false;
    bool m_connected = false;
};

/// ───────────────────────── 服务端夹具 ─────────────────────────
/// 每个用例独立：临时回连缓存目录 + io 服务端线程 + FFI 线程池 + IpcServer
struct IntegrationFixture
{
    asio::io_context io;
    asio::thread_pool pool{2};
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::unique_ptr<IpcServer> server;
    std::thread ioThread;
    std::filesystem::path storeDir;
    bool tornDown = false;

    IntegrationFixture()
    {
        std::error_code ec;
        storeDir = std::filesystem::temp_directory_path(ec)
                   / ("qtet-integration-"
                      + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(storeDir, ec);

        work = std::make_unique<
            asio::executor_work_guard<asio::io_context::executor_type>>(
            io.get_executor());
        ioThread = std::thread([this] { io.run(); });

        server = std::make_unique<IpcServer>(io, pool, storeDir);
        std::string error;
        if (!server->listen(kSocketName, error)) {
            teardown();
            FAIL("listen failed: ", error.c_str());
        }
    }

    ~IntegrationFixture()
    {
        teardown();
    }

    IntegrationFixture(const IntegrationFixture&) = delete;
    IntegrationFixture& operator=(const IntegrationFixture&) = delete;

    void teardown()
    {
        if (tornDown)
            return;
        tornDown = true;

        // 对齐 main 的关闭次序：io.stop() → join 服务端线程 → pool.join()，
        // 之后成员逆序析构（server → close()，此时 io 已停、无并发回调）
        io.stop();
        if (ioThread.joinable())
            ioThread.join();
        pool.join();

        std::error_code ec;
        std::filesystem::remove_all(storeDir, ec);
        // 清理残留 socket 文件（LocalAcceptor 不负责 unlink）；路径与监听侧同源推导
        ::unlink(ipc::localEndpointPath(kSocketName).c_str());
    }
};

/// 反序列化响应的便捷封装
proto::IpcResponse parseResponse(const std::string& frame)
{
    return proto::deserializeResponse(frame);
}

} // namespace

// ───────────────────────── 1. 基本请求响应 ─────────────────────────

TEST_CASE_FIXTURE(IntegrationFixture, "list_instances 响应 id=1 且 method 匹配")
{
    TestClient client;
    const auto frame = client.sendAndReceive("list_instances");
    REQUIRE(frame.has_value());

    const proto::IpcResponse resp = parseResponse(*frame);
    CHECK(resp.id == 1);
    CHECK(resp.method == "list_instances");
}

TEST_CASE_FIXTURE(IntegrationFixture, "get_last_error_msg 返回错误或含 message 的结果")
{
    TestClient client;
    const auto frame = client.sendAndReceive("get_last_error_msg");
    REQUIRE(frame.has_value());

    const proto::IpcResponse resp = parseResponse(*frame);
    CHECK(resp.id == 1);
    const bool hasMessage = resp.isError() || resp.result.contains("message");
    CHECK(hasMessage);
}

TEST_CASE_FIXTURE(IntegrationFixture, "未知方法返回错误响应")
{
    TestClient client;
    const auto frame = client.sendAndReceive("nonexistent_method");
    REQUIRE(frame.has_value());

    const proto::IpcResponse resp = parseResponse(*frame);
    CHECK(resp.isError());
}

// ───────────────────────── 2. 多客户端并发 ─────────────────────────

TEST_CASE_FIXTURE(IntegrationFixture, "两个客户端并发各自收到正确响应")
{
    TestClient first;
    TestClient second;
    REQUIRE(first.connect());
    REQUIRE(second.connect());

    first.sendRequest("list_instances", {}, 11);
    second.sendRequest("get_last_error_msg", {}, 22);

    const auto firstFrame = first.awaitFrame();
    const auto secondFrame = second.awaitFrame();
    REQUIRE(firstFrame.has_value());
    REQUIRE(secondFrame.has_value());

    const proto::IpcResponse firstResp = parseResponse(*firstFrame);
    const proto::IpcResponse secondResp = parseResponse(*secondFrame);
    CHECK(firstResp.id == 11);
    CHECK(firstResp.method == "list_instances");
    CHECK(secondResp.id == 22);
    CHECK(secondResp.method == "get_last_error_msg");
}

TEST_CASE_FIXTURE(IntegrationFixture, "两客户端互不阻塞（均 5s 内完成）")
{
    const auto started = std::chrono::steady_clock::now();

    TestClient first;
    TestClient second;
    REQUIRE(first.connect());
    REQUIRE(second.connect());

    first.sendRequest("get_last_error_msg", {}, 31);
    second.sendRequest("list_instances", {}, 32);

    const auto firstFrame = first.awaitFrame();
    const auto secondFrame = second.awaitFrame();
    REQUIRE(firstFrame.has_value());
    REQUIRE(secondFrame.has_value());

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    CHECK(elapsed.count() < 5000);

    CHECK(parseResponse(*firstFrame).id == 31);
    CHECK(parseResponse(*secondFrame).id == 32);
}

TEST_CASE_FIXTURE(IntegrationFixture, "异步调度不阻塞（单请求 1000ms 内完成）")
{
    TestClient client;
    const auto started = std::chrono::steady_clock::now();
    const auto frame = client.sendAndReceive("get_last_error_msg");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    REQUIRE(frame.has_value());
    CHECK(elapsed.count() < 1000);
}

// ───────────────────────── 3. 自动回连开关 ─────────────────────────

TEST_CASE_FIXTURE(IntegrationFixture, "自动回连开关往返且缓存文件落在临时目录")
{
    TestClient client;
    const auto setFrame =
        client.sendAndReceive("set_auto_reconnect", { { "enabled", true } });
    REQUIRE(setFrame.has_value());
    const proto::IpcResponse setResp = parseResponse(*setFrame);
    REQUIRE_FALSE(setResp.isError());
    CHECK(setResp.result.value("autoReconnect", false) == true);

    const auto getFrame = client.sendAndReceive("get_auto_reconnect");
    REQUIRE(getFrame.has_value());
    const proto::IpcResponse getResp = parseResponse(*getFrame);
    REQUIRE_FALSE(getResp.isError());
    CHECK(getResp.result.value("autoReconnect", false) == true);

    // set 成功即已落盘：<storeDir>/<socket 名称>.json
    const std::filesystem::path storeFile =
        storeDir / (std::string(kSocketName) + ".json");
    CHECK(std::filesystem::exists(storeFile));
}

// ───────────────────────── 4. 启停控制台日志 ─────────────────────────

TEST_CASE_FIXTURE(IntegrationFixture, "启停命令输出控制台日志")
{
    std::vector<std::string> captured;
    const auto previous = logging::setSink(
        [&captured](logging::Level level, const std::string& message) {
            if (level == logging::Level::Info || level == logging::Level::Warning)
                captured.push_back(message);
        });

    // 启动失败路径：无效 TOML 配置 → 应输出失败日志
    //（日志在 completion 中先于响应写出发出，收到响应即可断言）
    TestClient runClient;
    const auto runFrame = runClient.sendAndReceive(
        "run_network_instance", { { "cfg_str", b64encode("invalid toml") } });
    REQUIRE(runFrame.has_value());

    // 结束成功路径：未知名称被忽略但仍成功 → 应输出停止日志
    TestClient deleteClient;
    const auto deleteFrame = deleteClient.sendAndReceive(
        "delete_network_instance", { { "inst_names", { "net-x" } } });
    REQUIRE(deleteFrame.has_value());

    logging::setSink(previous); // 恢复默认 sink 后再读取捕获内容（无并发写入）

    bool startLogged = false;
    bool stopLogged = false;
    for (const std::string& line : captured) {
        if (line.find("Failed to start network instance") != std::string::npos)
            startLogged = true;
        if (line.find("Network instance(s) stopped") != std::string::npos)
            stopLogged = true;
    }
    CHECK(startLogged);
    CHECK(stopLogged);
}

// ───────────────────────── 5. 非法帧防护 ─────────────────────────

TEST_CASE_FIXTURE(IntegrationFixture, "超长帧头导致服务端断开")
{
    TestClient client;
    REQUIRE(client.connect());

    // 声明 0xFFFFFFFF 长度：超过单帧上限，daemon 应判定非法并断开连接
    std::string header(4, '\0');
    header[0] = static_cast<char>(0xFF);
    header[1] = static_cast<char>(0xFF);
    header[2] = static_cast<char>(0xFF);
    header[3] = static_cast<char>(0xFF);
    client.sendRaw(header);

    const auto frame = client.awaitFrame(3000);
    CHECK_FALSE(frame.has_value()); // 无响应帧
    CHECK(client.peerClosed());     // 客户端读到 eof/error
}

TEST_CASE_FIXTURE(IntegrationFixture, "零长度帧导致服务端断开")
{
    TestClient client;
    REQUIRE(client.connect());

    // 零长度帧无合法 JSON payload，daemon 应判定非法并断开连接
    const std::string header(4, '\0');
    client.sendRaw(header);

    const auto frame = client.awaitFrame(3000);
    CHECK_FALSE(frame.has_value());
    CHECK(client.peerClosed());
}

// ───────────────────────── 6. 非法请求不阻塞后续 ─────────────────────────

TEST_CASE_FIXTURE(IntegrationFixture, "同批次坏 JSON 帧不阻塞后续合法请求")
{
    TestClient client;
    REQUIRE(client.connect());

    // 同一批次先发非法 JSON 帧，再发合法请求（id=77）
    client.sendRaw(proto::encodeFrame("not json at all"));
    client.sendRequest("list_instances", {}, 77);

    // 第一帧：invalid request 错误响应（id=0、method 为空）
    const auto firstFrame = client.awaitFrame();
    REQUIRE(firstFrame.has_value());
    const proto::IpcResponse errResp = parseResponse(*firstFrame);
    CHECK(errResp.isError());
    CHECK(errResp.id == 0);

    // 随后：id=77 的正常响应到达
    const auto secondFrame = client.awaitFrame();
    REQUIRE(secondFrame.has_value());
    const proto::IpcResponse resp = parseResponse(*secondFrame);
    CHECK(resp.id == 77);
    CHECK(resp.method == "list_instances");
}

// ───────────────────────── 7. 背压下后续帧不丢失 ─────────────────────────

TEST_CASE_FIXTURE(IntegrationFixture, "FFI 在途时到达的后续帧在完成后被处理")
{
    // 背压设计：FFI 调用在途（busy）时服务端不继续读 socket，新数据滞留内核
    // 缓冲，由 completion 空闲后补挂读接收。本用例驱动该路径：第二帧延迟发送，
    // 大概率落在第一帧 dispatch 在途、读循环停摆的窗口内；若补挂读缺失，
    // 第二帧永远不被读取，awaitFrame 超时失败
    TestClient client;
    REQUIRE(client.connect());

    client.sendRequest("list_instances", {}, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.sendRequest("list_instances", {}, 2);

    const auto r1 = client.awaitFrame();
    const auto r2 = client.awaitFrame();
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(parseResponse(*r1).id == 1);
    CHECK(parseResponse(*r2).id == 2);
}
