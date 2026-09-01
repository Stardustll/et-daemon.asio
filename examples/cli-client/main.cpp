/// @file main.cpp
/// @brief qtet-daemon-asio 示例 CLI 前端客户端
///
/// 通过 ipc::LocalSocket 连接 qtet-daemon 后端，发送帧协议格式的 JSON 请求。
/// 用法：cli-client <方法名> [JSON参数]
/// 示例：cli-client list_instances
///       cli-client run_network_instance '{"cfg_str":"..."}'
///
/// 帧协议编解码在本文件内联实现（不使用 proto/FfiDispatcher），
/// 仅复用 qtet-daemon-core 的 ipc::LocalSocket 传输层。

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "ipc/LocalSocket.h"

namespace {

/// 打印用法与可用方法清单（对齐 Qt 版 cli-client 的 14 个方法）
void printUsage(std::FILE* out)
{
    std::fprintf(out, "Usage: cli-client <method> [json_params]\n");
    std::fprintf(out, "       cli-client --method <method> [json_params]\n");
    std::fprintf(out, "\nAvailable methods:\n");
    for (const char* method : {
             "parse_config", "run_network_instance", "delete_network_instance",
             "list_instances", "collect_network_infos", "call_json_rpc",
             "start_config_server_client", "stop_config_server_client",
             "is_config_server_client_connected", "get_error_msg",
             "get_last_error_msg", "poll_config_events",
             "set_auto_reconnect", "get_auto_reconnect",
         })
        std::fprintf(out, "  %s\n", method);
}

/// 内联帧打包：4 字节大端长度前缀 + payload（与 daemon 端协议一致）
std::string encodeFrameInline(const std::string& payload)
{
    const auto len = static_cast<std::uint32_t>(payload.size());
    std::string frame(4, '\0');
    frame[0] = static_cast<char>((len >> 24) & 0xFF);
    frame[1] = static_cast<char>((len >> 16) & 0xFF);
    frame[2] = static_cast<char>((len >> 8) & 0xFF);
    frame[3] = static_cast<char>(len & 0xFF);
    frame += payload;
    return frame;
}

/// 等待响应的共享状态：读循环 + 超时定时器，任一先完成（shared_ptr 保活）
struct AwaitState : std::enable_shared_from_this<AwaitState> {
    asio::io_context& io;
    ipc::LocalSocket& sock;
    asio::steady_timer timer;
    std::array<char, 65536> chunk{};
    std::string buffer;
    std::promise<std::optional<std::string>> result;
    bool done = false;

    AwaitState(asio::io_context& ioContext, ipc::LocalSocket& socket)
        : io(ioContext)
        , sock(socket)
        , timer(ioContext)
    {
    }

    void start()
    {
        timer.expires_after(std::chrono::milliseconds(5000));
        timer.async_wait([self = shared_from_this()](const std::error_code& ec) {
            if (ec)
                return; // 我方取消（已收到响应），忽略
            self->finish(std::nullopt); // 超时
        });
        read();
    }

    void read()
    {
        sock.asyncReadSome(asio::buffer(chunk),
                           [self = shared_from_this()](std::error_code ec, std::size_t bytes) {
                               if (ec || bytes == 0) {
                                   self->finish(std::nullopt); // 连接关闭/出错
                                   return;
                               }
                               self->buffer.append(self->chunk.data(), bytes);
                               if (self->buffer.size() >= 4) {
                                   // 内联帧解析：长度头完整且数据体到齐则取出一帧
                                   const auto len =
                                       (static_cast<std::uint8_t>(self->buffer[0]) << 24)
                                       | (static_cast<std::uint8_t>(self->buffer[1]) << 16)
                                       | (static_cast<std::uint8_t>(self->buffer[2]) << 8)
                                       | static_cast<std::uint8_t>(self->buffer[3]);
                                   if (self->buffer.size() >= 4 + len) {
                                       self->finish(self->buffer.substr(4, len));
                                       return;
                                   }
                               }
                               self->read();
                           });
    }

    void finish(std::optional<std::string> payload)
    {
        if (done)
            return;
        done = true;
        timer.cancel();
        sock.close(); // 排空在途操作，使 io.run() 尽快返回
        result.set_value(std::move(payload));
    }
};

} // namespace

int main(int argc, char* argv[])
{
    std::string method;
    std::string optionMethod; // --method 显式指定（对齐 Qt 版 QCommandLineParser 优先级）
    std::string paramsArg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            printUsage(stdout);
            return 0;
        }
        if (arg == "--method") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--method requires a value\n");
                return 1;
            }
            optionMethod = argv[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printUsage(stderr);
            return 1;
        }
        if (method.empty())
            method = arg;
        else if (paramsArg.empty())
            paramsArg = arg;
        else {
            std::fprintf(stderr, "Too many arguments\n");
            printUsage(stderr);
            return 1;
        }
    }
    // --method 选项优先于位置参数（对齐 Qt 版）
    if (!optionMethod.empty())
        method = optionMethod;

    // 无方法名 → 打印使用说明与方法清单
    if (method.empty()) {
        printUsage(stdout);
        return 0;
    }

    // 位置参数 2 为 JSON 对象则用之，否则空对象
    nlohmann::json params = nlohmann::json::object();
    if (!paramsArg.empty()) {
        try {
            nlohmann::json parsed = nlohmann::json::parse(paramsArg);
            if (parsed.is_object())
                params = std::move(parsed);
        } catch (const nlohmann::json::parse_error&) {
            // 非法 JSON → 空对象
        }
    }

    // 连接 daemon
    asio::io_context io;
    ipc::LocalSocket sock(io);
    std::string connectError;
    if (!sock.connect(DAEMON_SOCKET_NAME, connectError)) {
        std::fprintf(stderr, "Failed to connect to daemon: %s\n", connectError.c_str());
        return 1;
    }

    // 构建 JSON 请求：{ "id": 1, "method": "...", "params": {...} }
    nlohmann::json request;
    request["id"] = 1;
    request["method"] = method;
    request["params"] = params;

    sock.asyncWrite(encodeFrameInline(request.dump()), [](std::error_code) {});

    // 等待响应：读循环与 5 秒超时定时器任一先完成
    auto await = std::make_shared<AwaitState>(io, sock);
    await->start();
    io.run();

    const std::optional<std::string> payload = await->result.get_future().get();
    if (!payload.has_value() || payload->empty()) {
        std::fprintf(stderr, "No response from daemon\n");
        return 1;
    }

    // 解析并打印 JSON 响应（美化输出；error 优先）
    nlohmann::json response;
    try {
        response = nlohmann::json::parse(*payload);
    } catch (const nlohmann::json::parse_error&) {
        std::fprintf(stderr, "Invalid response JSON\n");
        return 1;
    }
    if (!response.is_object()) {
        std::fprintf(stderr, "Invalid response JSON\n");
        return 1;
    }

    const char* key = response.contains("error") ? "error" : "result";
    std::printf("%s\n", response[key].dump(4).c_str());
    return 0;
}
