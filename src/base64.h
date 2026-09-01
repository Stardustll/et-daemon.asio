/// @file base64.h
/// @brief 标准字母表 Base64 编解码接口（A-Za-z0-9+/，'=' padding）

#ifndef BASE64_H
#define BASE64_H

#include <string>
#include <string_view>

/// 标准字母表（A-Za-z0-9+/，'=' padding）Base64 编码
std::string b64encode(std::string_view input);

/// 解码：宽容模式，跳过非字母表字符（对齐 Qt QByteArray::fromBase64 缺省行为）
std::string b64decode(std::string_view input);

#endif // BASE64_H
