/// @file base64.cpp
/// @brief 标准字母表 Base64 编解码实现（表驱动编码，宽容模式解码）

#include "base64.h"

namespace {

const char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
