/// @file LocalAcceptor.cpp
/// @brief LocalAcceptor 平台分发
///
/// LocalAcceptor 的 Impl 与全部方法定义随平台实现同文件分布
/// （asyncAccept 经友元访问 LocalSocket::Impl，两类 Impl 须同编译单元）：
/// - POSIX：LocalSocketPosix.cpp（#ifndef _WIN32）
/// - Windows：LocalSocketWindows.cpp（#ifdef _WIN32，Task 11 填充）
/// 本文件为分发结构说明锚点，不承载平台相关定义。

#ifdef _WIN32
// Windows 分支：定义见 LocalSocketWindows.cpp（Task 11）
#else
// POSIX 分支：定义见 LocalSocketPosix.cpp
#endif
