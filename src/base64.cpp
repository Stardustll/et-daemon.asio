/// @file base64.cpp
/// @brief 标准字母表 Base64 编解码实现（表驱动编码，宽容模式解码）

#include "base64.h"

namespace {

/// 编码字母表：下标 0-63 依次对应 A-Z、a-z、0-9、'+'、'/'（RFC 4648 标准字母表）
const char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// 单字符解码：字母表字符 → 6 bit 码值
/// @param ch 待解码字符
/// @return 0-63 的码值；非字母表字符（含 '=' padding、空白等）返回 -1
int decodeDigit(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    if (ch == '+')
        return 62;
    if (ch == '/')
        return 63;
    return -1;
}

} // namespace

std::string b64encode(std::string_view input)
{
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);

    std::size_t i = 0;
    // 完整 3 字节组：24 bit 重组后切成 4 个 6 bit 码值查表输出
    while (i + 3 <= input.size()) {
        const unsigned v = (static_cast<unsigned char>(input[i]) << 16)
            | (static_cast<unsigned char>(input[i + 1]) << 8)
            | static_cast<unsigned char>(input[i + 2]);
        out += kEncodeTable[(v >> 18) & 0x3F];
        out += kEncodeTable[(v >> 12) & 0x3F];
        out += kEncodeTable[(v >> 6) & 0x3F];
        out += kEncodeTable[v & 0x3F];
        i += 3;
    }

    const std::size_t rem = input.size() - i;
    // 尾部不足 3 字节：低位补零组出 2/3 个字符，剩余位以 '=' 补满 4 字符组
    if (rem == 1) {
        const unsigned v = static_cast<unsigned char>(input[i]) << 16;
        out += kEncodeTable[(v >> 18) & 0x3F];
        out += kEncodeTable[(v >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        const unsigned v = (static_cast<unsigned char>(input[i]) << 16)
            | (static_cast<unsigned char>(input[i + 1]) << 8);
        out += kEncodeTable[(v >> 18) & 0x3F];
        out += kEncodeTable[(v >> 12) & 0x3F];
        out += kEncodeTable[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

std::string b64decode(std::string_view input)
{
    std::string out;
    out.reserve(input.size() / 4 * 3);

    // 比特累加器：每消费一个字母表字符累积 6 bit，攒够 8 bit 输出一字节；
    // 非字母表字符（含 '=' 与空白）直接跳过，实现宽容解码
    unsigned buf = 0;
    int nbits = 0;
    for (char ch : input) {
        const int d = decodeDigit(ch);
        if (d < 0)
            continue;
        buf = (buf << 6) | static_cast<unsigned>(d);
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out += static_cast<char>(buf >> nbits);
            buf &= (1u << nbits) - 1;
        }
    }
    return out;
}
