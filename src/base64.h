/// @file base64.h
/// @brief 标准字母表 Base64 编解码接口（A-Za-z0-9+/，'=' padding）
///
/// IPC 协议中 TOML 配置、状态 JSON、错误消息等文本字段的 Base64 传输
/// 均经由本模块编解码（哪些字段需要 Base64 以 README API 参考为准）。

#ifndef BASE64_H
#define BASE64_H

#include <string>
#include <string_view>

/// 标准字母表（A-Za-z0-9+/，'=' padding）Base64 编码
/// @param input 任意字节序列（UTF-8 文本或二进制数据）
/// @return Base64 编码字符串；长度恒为 4 的倍数，不足 3 字节的尾部以 '=' 补齐
std::string b64encode(std::string_view input);

/// 解码：宽容模式，跳过非字母表字符（对齐 Qt QByteArray::fromBase64 缺省行为）
/// @param input Base64 文本；允许混入空白、换行等非法字符（自动忽略），
///              '=' padding 同样被跳过
/// @return 解码后的原始字节序列；尾部不足 8 bit 的残余比特静默丢弃
std::string b64decode(std::string_view input);

#endif // BASE64_H
