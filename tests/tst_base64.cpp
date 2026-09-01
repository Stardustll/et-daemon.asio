/// @file tst_base64.cpp
/// @brief base64 编解码单元测试（RFC 4648 向量、二进制往返、宽容解码语义）

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>

#include "base64.h"

TEST_CASE("RFC 4648 测试向量（foobar 三例）")
{
    CHECK(b64encode("fo") == "Zm8=");
    CHECK(b64encode("fooba") == "Zm9vYmE=");
    CHECK(b64encode("foobar") == "Zm9vYmFy");
    CHECK(b64decode("Zm8=") == "fo");
    CHECK(b64decode("Zm9vYmE=") == "fooba");
    CHECK(b64decode("Zm9vYmFy") == "foobar");
}

TEST_CASE("空串编解码")
{
    CHECK(b64encode("") == "");
    CHECK(b64decode("") == "");
}

TEST_CASE("二进制往返（含 \\0 与 0xFF 字节）")
{
    const char raw[] = {'a', '\0', 'b', '\xff', '\x00', '\xfe', '\x7f', (char)0x80, '\0'};
    const std::string data(raw, sizeof(raw));
    CHECK(b64decode(b64encode(data)) == data);
}

TEST_CASE("宽容解码：非法字符剔除后解码")
{
    // "not@@base64!!" 剔除 @ 与 ! 后为 "notbase64"，9 个 6bit 单元 → 6 字节
    const std::string expected("\x9e\x8b\x5b\x6a\xc7\xba", 6);
    CHECK(b64decode("not@@base64!!") == expected);
}

TEST_CASE("缺失 padding 输入")
{
    CHECK(b64decode("Zm8") == "fo");
    CHECK(b64decode("Zm9vYmE") == "fooba");
}

TEST_CASE("非 4 倍数尾部残片")
{
    // 9 个单元 → 6 字节，尾部 1 个单元丢弃
    const std::string expected("\x9e\x8b\x5b\x6a\xc7\xba", 6);
    CHECK(b64decode("notbase64") == expected);
    // 5 个单元 → "foo"，尾部 1 个单元丢弃
    CHECK(b64decode("Zm9vY") == "foo");
    // '=' 任意位置出现均忽略
    CHECK(b64decode("Zm==9vYmFy") == "foobar");
}
