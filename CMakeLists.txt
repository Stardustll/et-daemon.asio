# qtet-daemon-asio 顶层 CMake 构建配置
# 依赖：Boost（header-only）、C++17、CMake 3.19+

cmake_minimum_required(VERSION 3.19)

# ── macOS：默认构建 Universal（arm64 + x86_64）二进制，最低支持 macOS 13 ──
# 父工程（QtEasyTier）配置时经 -DCMAKE_OSX_ARCHITECTURES / -DCMAKE_OSX_DEPLOYMENT_TARGET
# 透传，与 .app 架构保持严格一致；此处仅提供默认值（CACHE 无 FORCE，-D 传入值优先）。
# 须在 project() 之前设置，令编译器 ABI 探测即按目标架构进行。
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS 构建架构（默认 Universal）")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS 最低支持版本")
endif()

project(qtet-daemon-asio VERSION 1.0.0 LANGUAGES CXX)
set(ET_FFI_VERSION 2.6.4)

# ── C++ 语言标准 ──
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── IPC socket 名称/路径（可通过 cmake -DDAEMON_SOCKET_NAME=... 自定义）──
# macOS 默认用绝对路径 /var/run/qtet-daemon.sock：root daemon 与普通用户前端的 TMPDIR
# 不同（/tmp vs /var/folders/…/T），裸名会被 ipc::localEndpointPath 解析成不同文件，
# 导致前端连不上。运行时可用 --socket 或 QTET_DAEMON_SOCKET 覆盖（见 SocketName.h）。
if(APPLE)
    set(ET_DAEMON_SOCKET_NAME_DEFAULT "/var/run/qtet-daemon.sock")
else()
    set(ET_DAEMON_SOCKET_NAME_DEFAULT "qtet-daemon.sock")
endif()
set(DAEMON_SOCKET_NAME "${ET_DAEMON_SOCKET_NAME_DEFAULT}" CACHE STRING "守护进程 IPC 本地 socket 名称/路径")
add_compile_definitions(DAEMON_SOCKET_NAME="${DAEMON_SOCKET_NAME}")

# ── EasyTier FFI 下载源（默认从 CNB 下载，关闭后改从 GitHub 下载）──
option(DOWNLOAD_FFI_FROM_CNB "从 CNB 下载 EasyTier FFI 预编译包（关闭时从 GitHub 下载）" ON)

# ── 构建产物统一输出到 build/Output ──
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Output")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Output")

# ── 安装目录约定（bin/lib，供 install 规则使用）──
include(GNUInstallDirs)

# ── Boost 依赖（仅 header-only 使用，要求 config 模式提供 Boost::headers）──
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost REQUIRED)

# ── EasyTier FFI 导入库（上游依赖，构建时自动下载，仅链接不构建）──
include(cmake/download_ffi.cmake)

# ── 向程序暴露版本号（--version 输出使用）──
add_compile_definitions(
    PROJECT_VERSION="${PROJECT_VERSION}"
    ET_FFI_VERSION="${ET_FFI_VERSION}"
)

set(ET_FFI_COPY_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

foreach(ET_FFI_EXCLUDED_RUNTIME_BASENAME IN LISTS ET_FFI_EXCLUDED_RUNTIME_BASENAMES)
    list(APPEND ET_FFI_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E remove -f
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${ET_FFI_EXCLUDED_RUNTIME_BASENAME}"
    )
endforeach()

foreach(ET_FFI_RUNTIME_FILE IN LISTS ET_FFI_RUNTIME_FILES)
    get_filename_component(ET_FFI_RUNTIME_FILE_NAME "${ET_FFI_RUNTIME_FILE}" NAME)
    list(APPEND ET_FFI_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ET_FFI_RUNTIME_FILE}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${ET_FFI_RUNTIME_FILE_NAME}"
    )
endforeach()

add_custom_target(copy_easytier_ffi_runtime ALL
    ${ET_FFI_COPY_COMMANDS}
    COMMENT "复制 EasyTier FFI 运行时文件到输出目录"
)

# ── 子目录 ──
add_subdirectory(src)
add_subdirectory(examples)

# ── 测试 ──
enable_testing()
add_subdirectory(tests)

# ── 安装规则 ──
install(TARGETS qtet-daemon
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
# 将 FFI 运行时文件一并安装到可执行文件同目录
install(FILES ${ET_FFI_RUNTIME_FILES}
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
