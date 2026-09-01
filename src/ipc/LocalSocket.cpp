/// @file LocalSocket.cpp
/// @brief LocalSocket 平台分发
///
/// LocalSocket 的 Impl 与全部方法定义按平台互斥编译门分布：
/// - POSIX：LocalSocketPosix.cpp（#ifndef _WIN32）
/// - Windows：LocalSocketWindows.cpp（#ifdef _WIN32，Task 11 填充）
/// 两个平台文件经互斥门保证任意平台恰好提供一份符号；本文件为分发
/// 结构说明锚点，不承载平台相关定义。

#ifdef _WIN32
// Windows 分支：定义见 LocalSocketWindows.cpp（Task 11）
#else
// POSIX 分支：定义见 LocalSocketPosix.cpp
#endif
