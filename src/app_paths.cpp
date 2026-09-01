/// @file app_paths.cpp
/// @brief 应用路径工具实现：按平台定位可执行文件目录，任何失败回退 CWD

#include "app_paths.h"

#include <string>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif

namespace app_paths {

namespace {

/// 定位失败时的回退：CWD；连 CWD 也取不到则返回空 path
std::filesystem::path fallbackDirectory()
{
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec)
        return {};
    return cwd;
}

} // namespace

std::filesystem::path exeDirectory()
{
#if defined(_WIN32)
    // Windows：GetModuleFileNameW 直接取完整路径（截断到 MAX_PATH 即视为失败）
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return fallbackDirectory();
    return std::filesystem::path(buffer, buffer + length).parent_path();
#elif defined(__APPLE__)
    // macOS：_NSGetExecutablePath 两段式取路径（先探长度再取内容），
    // 再经 realpath 解析符号链接与 ".." 得到规范绝对路径
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != 0 && size == 0)
        return fallbackDirectory();
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return fallbackDirectory();
    char resolved[PATH_MAX] = {};
    if (realpath(buffer.data(), resolved) == nullptr)
        return fallbackDirectory();
    return std::filesystem::path(resolved).parent_path();
#else
    // Linux：/proc/self/exe 符号链接恒指向当前可执行文件（已是规范路径）
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || self.empty())
        return fallbackDirectory();
    return self.parent_path();
#endif
}

} // namespace app_paths
