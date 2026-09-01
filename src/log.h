/// @file log.h
/// @brief 轻量日志：全局 sink 可替换，宏分发三级别，线程安全输出

#ifndef LOG_H
#define LOG_H

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

/// 命名空间为 logging 而非计划原文的 log：POSIX math.h 在全局作用域声明
/// ::log 函数（doctest 等经 <cmath> 引入），全局 namespace log 无法与之
/// 共存（任何包含顺序均编译失败），故改名；枚举/函数签名/宏全部保持原文
namespace logging {

enum class Level { Info, Warning, Error };

/// 级别名称（缺省 sink 输出格式使用）
inline const char* levelName(Level level)
{
    switch (level) {
    case Level::Info:    return "INFO";
    case Level::Warning: return "WARNING";
    case Level::Error:   return "ERROR";
    }
    return "UNKNOWN";
}

namespace detail {

/// 缺省 sink：输出到 stderr，格式 "[ISO8601本地时间] [LEVEL] message\n"
/// std::localtime 返回静态缓冲非线程安全，由 write() 持有的同一 mutex
/// 保护（sink 调用发生在锁内）
inline void defaultSink(Level level, const std::string& message)
{
    const std::time_t now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::ostringstream ts;
    ts << std::put_time(std::localtime(&now), "%Y-%m-%dT%H:%M:%S");
    std::fprintf(stderr, "[%s] [%s] %s\n", ts.str().c_str(), levelName(level),
                 message.c_str());
}

/// 全局状态（函数局部 static 避免 ODR 问题）：mutex + 当前 sink
struct LogState {
    std::mutex mutex;
    std::function<void(Level, const std::string&)> sink = &defaultSink;
};

inline LogState& logState()
{
    static LogState state;
    return state;
}

} // namespace detail

/// 替换全局 sink；返回旧 sink。缺省 sink 输出到 stderr："[ISO8601本地时间] [LEVEL] message\n"
/// sink 为空时恢复缺省 stderr 输出
inline std::function<void(Level, const std::string&)>
setSink(std::function<void(Level, const std::string&)> sink)
{
    auto& state = detail::logState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!sink)
        sink = &detail::defaultSink;
    return std::exchange(state.sink, std::move(sink));
}

/// 写一条日志；线程安全（内部 mutex，sink 调用发生在锁内，保证行不交叉）
inline void write(Level level, const std::string& message)
{
    auto& state = detail::logState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.sink(level, message);
}

} // namespace logging

#define LOG_INFO(msg)    ::logging::write(::logging::Level::Info, (msg))
#define LOG_WARNING(msg) ::logging::write(::logging::Level::Warning, (msg))
#define LOG_ERROR(msg)   ::logging::write(::logging::Level::Error, (msg))

#endif // LOG_H
