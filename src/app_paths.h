/// @file app_paths.h
/// @brief 应用路径工具：定位可执行文件所在目录（与 CWD 无关）

#ifndef APP_PATHS_H
#define APP_PATHS_H

#include <filesystem>

namespace app_paths {

/// 可执行文件所在目录（不是 CWD）。失败时回退 `std::filesystem::current_path()`
std::filesystem::path exeDirectory();

} // namespace app_paths

#endif // APP_PATHS_H
