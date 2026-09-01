/// @file tst_protocol.cpp
/// @brief Protocol 层单元测试（proto 命名空间：JSON 序列化 + 帧解析纯函数）
///
/// 测试覆盖：请求/响应序列化往返、Compact 无空白、畸形 JSON 容错、
/// result/error 互斥、parseFrame 分片/边界/非法帧、encodeFrame 大端头正确性

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>

#include <nlohmann/json.hpp>

#include "daemon/Protocol.h"

namespace {

/// 以字节列表构造二进制字符串（避免 "\x05x" 这类十六进制转义贪婪吞并问题）
std::string bytes(std::initializer_list<unsigned char> bs)
{
    return std::string(bs.begin(), bs.end());
}

} // namespace

// ──────────────────────────────────────────────
// JSON 序列化 / 反序列化
// ──────────────────────────────────────────────

TEST_CASE("请求序列化/反序列化往返（含 params 对象）")
{
    proto::IpcRequest req;
    req.id = 1;
    req.method = "test";
    req.params = nlohmann::json{{"key", "value"}};

    const std::string json = proto::serializeRequest(req);
    CHECK(!json.empty());

    const proto::IpcRequest parsed = proto::deserializeRequest(json);
    CHECK(parsed.id == req.id);
    CHECK(parsed.method == req.method);
    CHECK(parsed.params == req.params);
}

TEST_CASE("请求序列化输出为 Compact 格式（无空白、键序确定）")
{
    proto::IpcRequest req;
    req.id = 7;
    req.method = "m";
    req.params = nlohmann::json{{"k", "v"}};

    const std::string json = proto::serializeRequest(req);
    CHECK(json == R"({"id":7,"method":"m","params":{"k":"v"}})");
}

TEST_CASE("响应序列化 result 路径往返")
{
    proto::IpcResponse resp;
    resp.id = 42;
    resp.method = "test";
    resp.result = nlohmann::json{{"success", true}};

    const std::string json = proto::serializeResponse(resp);
    CHECK(json == R"({"id":42,"method":"test","result":{"success":true}})");

    const proto::IpcResponse parsed = proto::deserializeResponse(json);
    CHECK(parsed.id == resp.id);
    CHECK(parsed.method == resp.method);
    CHECK_FALSE(parsed.isError());
    CHECK(parsed.result == resp.result);
    CHECK(parsed.error.empty());
}

TEST_CASE("响应序列化 error 路径往返（error 非空时优先于 result，二者互斥）")
{
    proto::IpcResponse resp;
    resp.id = 99;
    resp.method = "test";
    resp.error = nlohmann::json{{"message", "something went wrong"}};
    resp.result = nlohmann::json{{"ignored", true}}; // error 非空时 result 不得输出

    const std::string json = proto::serializeResponse(resp);
    // 精确串比较已隐式验证 Compact 无空白（error 值内部空格不影响键值分隔符判定）
    CHECK(json == R"({"error":{"message":"something went wrong"},"id":99,"method":"test"})");
    CHECK(json.find("\"result\"") == std::string::npos); // 互斥

    const proto::IpcResponse parsed = proto::deserializeResponse(json);
    CHECK(parsed.isError());
    CHECK(parsed.error["message"] == "something went wrong");
}

TEST_CASE("deserializeRequest 畸形 JSON / 非对象根返回默认请求")
{
    CHECK(proto::deserializeRequest("not json at all").method.empty());
    CHECK(proto::deserializeRequest("[1,2,3]").method.empty());
    CHECK(proto::deserializeRequest("42").method.empty());
}

TEST_CASE("deserializeRequest 空对象与字段类型容错")
{
    const proto::IpcRequest req = proto::deserializeRequest("{}");
    CHECK(req.method.empty());
    CHECK(req.id == 0);
    CHECK(req.params.empty());
    CHECK(req.params.is_object());

    // id 非数字 → 0；method 非字符串 → ""；params 非对象 → 空对象
    const proto::IpcRequest odd = proto::deserializeRequest(R"({"id":"abc","method":123,"params":[1,2]})");
    CHECK(odd.id == 0);
    CHECK(odd.method.empty());
    CHECK(odd.params.empty());
    CHECK(odd.params.is_object());
}

TEST_CASE("deserializeResponse 字段缺失/类型容错")
{
    const proto::IpcResponse resp = proto::deserializeResponse("{}");
    CHECK(resp.id == 0);
    CHECK(resp.method.empty());
    CHECK(resp.result.empty());
    CHECK(resp.result.is_object());
    CHECK(resp.error.empty());
    CHECK(resp.error.is_object());
    CHECK_FALSE(resp.isError());

    // result/error 非对象 → 各自回退为空对象
    const proto::IpcResponse odd = proto::deserializeResponse(R"({"id":"x","method":true,"result":"nope","error":7})");
    CHECK(odd.id == 0);
    CHECK(odd.method.empty());
    CHECK(odd.result.empty());
    CHECK(odd.result.is_object());
    CHECK(odd.error.empty());
    CHECK(odd.error.is_object());
}

// ──────────────────────────────────────────────
// 帧协议：4 字节大端长度前缀 + payload
// ──────────────────────────────────────────────

TEST_CASE("parseFrame：空缓冲区 / 不足 4 字节 / 仅有长度头 → Incomplete")
{
    std::string payload;

    std::string empty;
    CHECK(proto::parseFrame(empty, payload) == proto::FrameStatus::Incomplete);

    std::string two = bytes({0x00, 0x00});
    CHECK(proto::parseFrame(two, payload) == proto::FrameStatus::Incomplete);

    std::string headerOnly = bytes({0x00, 0x00, 0x00, 0x05});
    CHECK(proto::parseFrame(headerOnly, payload) == proto::FrameStatus::Incomplete);
}

TEST_CASE("parseFrame：恰好一个完整帧 → Complete 且缓冲区清空")
{
    std::string buffer = proto::encodeFrame("hello");
    std::string payload;
    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Complete);
    CHECK(payload == "hello");
    CHECK(buffer.empty());
}

TEST_CASE("parseFrame：一帧半数据 → 先 Complete 后 Incomplete，剩余字节保留")
{
    std::string buffer = proto::encodeFrame("ab") + bytes({0x00, 0x00, 0x00, 0x05, 'x'});
    std::string payload;

    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Complete);
    CHECK(payload == "ab");
    CHECK(buffer == bytes({0x00, 0x00, 0x00, 0x05, 'x'}));

    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Incomplete);
    CHECK(buffer == bytes({0x00, 0x00, 0x00, 0x05, 'x'})); // 数据不足时不得消费缓冲区
}

TEST_CASE("parseFrame：两帧背靠背 → 顺序解析互不干扰")
{
    std::string buffer = proto::encodeFrame("first") + proto::encodeFrame("second");
    std::string payload;

    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Complete);
    CHECK(payload == "first");
    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Complete);
    CHECK(payload == "second");
    CHECK(buffer.empty());
}

TEST_CASE("parseFrame：零长度帧 → Invalid")
{
    // 语义约定：Invalid 之后缓冲区状态未定义（父工程直接断开连接），此处仅断言状态
    std::string buffer = bytes({0x00, 0x00, 0x00, 0x00});
    std::string payload;
    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Invalid);
}

TEST_CASE("parseFrame：0xFFFFFFFF 超长帧 → Invalid")
{
    std::string buffer = bytes({0xFF, 0xFF, 0xFF, 0xFF});
    std::string payload;
    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Invalid);
}

TEST_CASE("parseFrame：kMaxFrameSize 边界")
{
    std::string payload;

    // 恰好 kMaxFrameSize（0x01000000）但数据不足 → Incomplete（不是 Invalid）
    std::string atMax = bytes({0x01, 0x00, 0x00, 0x00});
    CHECK(proto::parseFrame(atMax, payload) == proto::FrameStatus::Incomplete);

    // kMaxFrameSize + 1 → Invalid
    std::string overMax = bytes({0x01, 0x00, 0x00, 0x01});
    CHECK(proto::parseFrame(overMax, payload) == proto::FrameStatus::Invalid);
}

TEST_CASE("encodeFrame：4 字节大端长度头正确")
{
    const std::string frame = proto::encodeFrame("hello");
    CHECK(frame == bytes({0x00, 0x00, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o'}));
}

TEST_CASE("encodeFrame：空 payload 仅输出 4 字节零头（parseFrame 判其为 Invalid）")
{
    std::string buffer = proto::encodeFrame("");
    CHECK(buffer == bytes({0x00, 0x00, 0x00, 0x00}));
    std::string payload;
    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Invalid);
}

TEST_CASE("encodeFrame/parseFrame 往返（含二进制安全字节）")
{
    const std::string data = bytes({0x00, 0x01, 0xFF, 'a', 0x80, 0x7F});
    std::string buffer = proto::encodeFrame(data);
    std::string payload;
    CHECK(proto::parseFrame(buffer, payload) == proto::FrameStatus::Complete);
    CHECK(payload == data);
    CHECK(buffer.empty());
}
