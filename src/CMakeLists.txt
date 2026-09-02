# src/ 子目录构建配置
# qtet-daemon-core：STATIC 库，封装 Protocol 与基础设施（后续任务追加 IpcServer/FfiDispatcher 等）
# qtet-daemon：守护进程可执行文件，链接 qtet-daemon-core

# ── 核心静态库 ──
add_library(qtet-daemon-core STATIC
    base64.h
    base64.cpp
    app_paths.h
    app_paths.cpp
    daemon/Protocol.h
    daemon/Protocol.cpp
    daemon/ReconnectStore.h
    daemon/ReconnectStore.cpp
    daemon/IpcServer.h
    daemon/IpcServer.cpp
    ffis/FfiDispatcher.h
    ffis/FfiDispatcher.cpp
    ipc/LocalEndpoint.h
    ipc/LocalSocket.h
    ipc/LocalSocket.cpp
    ipc/LocalSocketPosix.cpp
    ipc/LocalSocketWindows.cpp
    ipc/LocalAcceptor.h
    ipc/LocalAcceptor.cpp
)

# include 路径：third_party 单头文件库（nlohmann/json、doctest、toml++）
# 与 src/ 自身（daemon/xxx.h 直接引用），随库 PUBLIC 传播给可执行文件与测试；
# ffis/ 单独加入使父工程风格的 #include "FfiDispatcher.h" 写法可用；
# ipc/ 同理使 #include "LocalSocket.h" 写法可用
target_include_directories(qtet-daemon-core PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/ffis"
    "${CMAKE_CURRENT_SOURCE_DIR}/ipc"
)

# easytier_ffi、Boost（FfiDispatcher.h 公开头引入 boost/asio）均随库 PUBLIC 传播
target_link_libraries(qtet-daemon-core
    PUBLIC
        easytier_ffi
        Boost::headers
)

# Windows 服务注册/控制（父工程移植）：仅 Windows 平台参与编译
if(WIN32)
    target_sources(qtet-daemon-core PRIVATE
        windows-register/WindowsService.h
        windows-register/WindowsService.cpp
    )
    # SCM/安全 API（advapi32）、ShellExecuteExW UAC 提权（shell32）、
    # MessageBoxW（user32）：经 core 的 INTERFACE 传播给 daemon 与测试
    #（LocalSocketWindows.cpp 的 DACL/命名管道同样依赖 advapi32）
    # ws2_32：Boost.Asio 在 Windows 的 winsock_init 静态对象引用 WSAStartup/WSACleanup，
    # 所有包含 asio 头文件的 TU（daemon、测试、cli-client）都需要它，经 INTERFACE 传播
    target_link_libraries(qtet-daemon-core INTERFACE advapi32 shell32 user32 ws2_32)
endif()

# ── 守护进程可执行文件 ──
add_executable(qtet-daemon main.cpp)

target_link_libraries(qtet-daemon
    PRIVATE
        qtet-daemon-core
        Boost::headers
)

if(WIN32)
    # Windows 服务控制管理器 API（CreateService/StartServiceCtrlDispatcher 等）
    target_link_libraries(qtet-daemon PRIVATE advapi32)
endif()

add_dependencies(qtet-daemon copy_easytier_ffi_runtime)

# RPATH 设置
# 构建时：CMake 自动将 easytier_ffi 导入库所在目录加入 RPATH
# 安装时：$ORIGIN 使二进制在其自身目录下查找 libeasytier_ffi.so
set_target_properties(qtet-daemon PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH FALSE
)
