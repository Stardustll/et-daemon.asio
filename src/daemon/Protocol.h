/// @file Protocol.h
/// @brief IPC 通信协议层定义（无 Qt 依赖版本）
///
/// 定义前后端通信的 JSON-RPC 风格消息格式：
/// - 请求：{ "id": N, "method": "...", "params": {...} }
/// - 响应：{ "id": N, "method": "...", "result": {...} }
///         { "id": N, "method": "...", "error": { "message": "..." } }
///
/// 消息帧格式：4 字节大端序长度前缀 + JSON 负载。
/// 部分文本字段使用 Base64 编码传输；具体字段以 IPC API 文档为准。

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

/// IPC 通信协议层命名空间
namespace proto {

/// 请求 ID 类型（与 JSON 整数兼容）
using IpcId = std::int64_t;

/// IPC 请求结构体
/// JSON 格式：{ "id": N, "method": "方法名", "params": {...} }
struct IpcRequest {
    IpcId id = 0;      ///< 请求序号，用于匹配请求与响应
    std::string method;             ///< 方法名，对应 EasyTierFFI 函数名
    nlohmann::json params = nlohmann::json::object();  ///< 方法参数；是否需要 Base64 取决于具体字段
};

/// IPC 响应结构体
/// 成功：result 有内容，error 为空
/// 失败：error 有内容，result 为空
struct IpcResponse {
    IpcId id = 0;
    std::string method;
    nlohmann::json result = nlohmann::json::object();  ///< 成功时的返回数据
    nlohmann::json error = nlohmann::json::object();   ///< 失败时的错误信息，包含 "message" 字段

    /// 判断是否为错误响应
    bool isError() const { return !error.empty(); }
};

/// 将请求对象序列化为 JSON 字符串（Compact 格式，无空白）
std::string serializeRequest(const IpcRequest& req);

/// 将 JSON 字符串反序列化为请求对象（解析失败/根非对象返回默认值，method 为空）
IpcRequest deserializeRequest(const std::string& json);

/// 将响应对象序列化为 JSON 字符串（error 非空输出 "error"，否则输出 "result"）
std::string serializeResponse(const IpcResponse& resp);

/// 将 JSON 字符串反序列化为响应对象
IpcResponse deserializeResponse(const std::string& json);

/// 单帧 payload 最大长度（字节）。
/// 超过该上限的帧被判定为非法，调用方应断开连接，避免恶意客户端撑大接收缓冲区耗尽内存。
constexpr std::size_t kMaxFrameSize = 16 * 1024 * 1024; // 16 MiB

/// 帧读取状态
enum class FrameStatus {
    Incomplete, ///< 数据不足，等待更多数据
    Complete,   ///< 已读取一个完整帧
    Invalid     ///< 非法帧（零长度或超过最大长度），应断开连接
};

/// 纯函数帧解析：从 buffer 头部解析一帧（4 字节长度头 + 数据体）
/// @param buffer 累积缓冲区，成功解析时消费对应字节，剩余数据保留
/// @param payload 输出参数，仅在返回 Complete 时有效
/// @return 解析状态；Invalid 表示帧声明非法（零长度/超长，返回时不消费也不
/// 修改缓冲区），调用方应断开连接
FrameStatus parseFrame(std::string& buffer, std::string& payload);

/// 打包一帧：4 字节大端长度前缀 + payload
std::string encodeFrame(const std::string& payload);

} // namespace proto

#endif // PROTOCOL_H
