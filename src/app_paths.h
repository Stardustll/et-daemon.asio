/// @file app_paths.h
/// @brief 应用路径工具：定位可执行文件所在目录（与 CWD 无关）

#ifndef APP_PATHS_H
#define APP_PATHS_H

#include <filesystem>

namespace app_paths {

/// 定位当前进程可执行文件所在目录（与当前工作目录 CWD 无关）
/// @return 可执行文件目录；平台 API 失败时回退 `std::filesystem::current_path()`，
///         连 CWD 也取不到时返回空 path（调用方自行兜底）
std::filesystem::path exeDirectory();

} // namespace app_paths

#endif // APP_PATHS_H
