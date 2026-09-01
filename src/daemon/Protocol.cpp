/// @file Protocol.cpp
/// @brief IPC 通信协议层实现

#include "daemon/Protocol.h"

#include <cstdint>

namespace proto {
namespace {

/// 解析 JSON 并要求根为对象；解析失败/根非对象返回 null，调用方按默认值处理
nlohmann::json parseObject(const std::string& json)
{
    try {
        nlohmann::json root = nlohmann::json::parse(json);
        if (root.is_object())
            return root;
    } catch (const nlohmann::json::parse_error&) {
        // 畸形 JSON → 视同默认对象，不抛出
    }
    return nlohmann::json();
}

/// 取整数字段；键缺失或非数字返回默认值（对齐 Qt value("id").toInteger(0) 语义）
IpcId numberOr(const nlohmann::json& root, const char* key, IpcId defaultValue)
{
    const auto it = root.find(key);
    if (it != root.end() && it->is_number())
        return it->get<IpcId>();
    return defaultValue;
}

/// 取字符串字段；键缺失或非字符串返回空串（对齐 Qt value("method").toString() 语义）
std::string stringOr(const nlohmann::json& root, const char* key)
{
    const auto it = root.find(key);
    if (it != root.end() && it->is_string())
        return it->get<std::string>();
    return std::string();
}

/// 取对象字段；键缺失或非对象返回空对象（对齐 Qt value("params").toObject() 语义）
nlohmann::json objectOr(const nlohmann::json& root, const char* key)
{
    const auto it = root.find(key);
    if (it != root.end() && it->is_object())
        return *it;
    return nlohmann::json::object();
}

} // namespace

// ──────────────────────────────────────────────
// 请求序列化 / 反序列化
// ──────────────────────────────────────────────

std::string serializeRequest(const IpcRequest& req)
{
    // dump() 缺省 Compact 格式：无换行无缩进，节省带宽
    return nlohmann::json{
        {"id", req.id},
        {"method", req.method},
        {"params", req.params},
    }.dump();
}

IpcRequest deserializeRequest(const std::string& json)
{
    IpcRequest req;

    // JSON 解析失败或根不是对象 → 返回空请求
    const nlohmann::json root = parseObject(json);
    if (root.is_null())
        return req;

    req.id = numberOr(root, "id", 0);
    req.method = stringOr(root, "method");
    req.params = objectOr(root, "params");
    return req;
}

// ──────────────────────────────────────────────
// 响应序列化 / 反序列化
// ──────────────────────────────────────────────

std::string serializeResponse(const IpcResponse& resp)
{
    nlohmann::json root{
        {"id", resp.id},
        {"method", resp.method},
    };

    // 根据 isError() 决定输出 result 或 error 字段（互斥）
    if (resp.isError())
        root["error"] = resp.error;
    else
        root["result"] = resp.result;

    return root.dump();
}

IpcResponse deserializeResponse(const std::string& json)
{
    IpcResponse resp;

    const nlohmann::json root = parseObject(json);
    if (root.is_null())
        return resp;

    resp.id = numberOr(root, "id", 0);
    resp.method = stringOr(root, "method");
    resp.result = objectOr(root, "result");
    resp.error = objectOr(root, "error");
    return resp;
}

// ──────────────────────────────────────────────
// 帧协议：4 字节大端长度前缀 + payload
// ──────────────────────────────────────────────

FrameStatus parseFrame(std::string& buffer, std::string& payload)
{
    // 缓冲区不足 4 字节，连长度头都不完整，等待更多数据
    if (buffer.size() < 4)
        return FrameStatus::Incomplete;

    // 从 4 字节大端头解码 payload 长度
    const std::uint32_t len = (static_cast<unsigned char>(buffer[0]) << 24)
                            | (static_cast<unsigned char>(buffer[1]) << 16)
                            | (static_cast<unsigned char>(buffer[2]) << 8)
                            | static_cast<unsigned char>(buffer[3]);

    // 零长度帧无合法 JSON payload，视为非法
    if (len == 0)
        return FrameStatus::Invalid;

    // 声明长度超过上限：拒绝而不是继续累积，避免内存耗尽
    if (len > kMaxFrameSize)
        return FrameStatus::Invalid;

    // 用 size_t 计算总长度；len 已受上限约束，4 + len 不会回绕
    const std::size_t total = std::size_t(4) + static_cast<std::size_t>(len);
    if (buffer.size() < total)
        return FrameStatus::Incomplete;

    // 取出完整帧 payload，剩余数据保留在缓冲区供下次使用
    payload = buffer.substr(4, len);
    buffer.erase(0, total);
    return FrameStatus::Complete;
}

std::string encodeFrame(const std::string& payload)
{
    // 4 字节大端序编码 payload 长度
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    std::string frame(4, '\0');
    frame[0] = static_cast<char>((len >> 24) & 0xFF);
    frame[1] = static_cast<char>((len >> 16) & 0xFF);
    frame[2] = static_cast<char>((len >> 8) & 0xFF);
    frame[3] = static_cast<char>(len & 0xFF);

    frame += payload; // 再拼接数据体
    return frame;
}

} // namespace proto
