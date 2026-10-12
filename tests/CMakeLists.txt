# tests/ 子目录构建配置
# 每个 tst_*.cpp 自带 DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN，独立链接为可执行文件
# 统一链接 qtet-daemon-core（include 路径由 core PUBLIC 传播，不再直接编译 ../src/*.cpp）

add_executable(tst_base64 tst_base64.cpp)
target_link_libraries(tst_base64 PRIVATE qtet-daemon-core)
add_test(NAME tst_base64 COMMAND tst_base64)

# log.h 为 header-only，仅需 core 传播的 include 路径
add_executable(tst_log tst_log.cpp)
target_link_libraries(tst_log PRIVATE qtet-daemon-core)
add_test(NAME tst_log COMMAND tst_log)

add_executable(tst_app_paths tst_app_paths.cpp)
target_link_libraries(tst_app_paths PRIVATE qtet-daemon-core)
add_test(NAME tst_app_paths COMMAND tst_app_paths)

# SocketName.h 为 header-only，仅需 core 传播的 include 路径
add_executable(tst_socket_name tst_socket_name.cpp)
target_link_libraries(tst_socket_name PRIVATE qtet-daemon-core)
add_test(NAME tst_socket_name COMMAND tst_socket_name)

add_executable(tst_protocol tst_protocol.cpp)
target_link_libraries(tst_protocol PRIVATE qtet-daemon-core)
add_test(NAME tst_protocol COMMAND tst_protocol)

add_executable(tst_reconnect_store tst_reconnect_store.cpp)
target_link_libraries(tst_reconnect_store PRIVATE qtet-daemon-core)
add_test(NAME tst_reconnect_store COMMAND tst_reconnect_store)
# /dev/full 失败注入用例在源内 #ifdef _WIN32 运行时跳过，可到处构建

# ── tst_dispatcher：FFI 调度层单元测试（14 个方法分发、参数校验、回连缓存清理）──
# 依赖 libeasytier_ffi.so 在运行时可加载：FFI 运行时复制到 Output/ + 构建树 RPATH
add_executable(tst_dispatcher tst_dispatcher.cpp)
target_link_libraries(tst_dispatcher PRIVATE qtet-daemon-core)
add_dependencies(tst_dispatcher copy_easytier_ffi_runtime)
add_test(NAME tst_dispatcher COMMAND tst_dispatcher)

# ── tst_integration：IPC 端到端集成测试（同进程 IpcServer + TestClient 收发，11 用例）──
# POSIX-only（unistd.h ::unlink + unix socket 路径）：待 Windows CI 落地后（Task 11
# 运行时验证未竟）再移植，当前仅非 Windows 构建
if(NOT WIN32)
    add_executable(tst_integration tst_integration.cpp)
    target_link_libraries(tst_integration PRIVATE qtet-daemon-core)
    add_dependencies(tst_integration copy_easytier_ffi_runtime)
    add_test(NAME tst_integration COMMAND tst_integration)
endif()
