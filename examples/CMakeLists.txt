# examples/ 目录：示例客户端
# cli-client：命令行前端示例
# 帧协议编解码在 main.cpp 内联实现（不使用 proto/FfiDispatcher），
# 仅复用 qtet-daemon-core 的 ipc::LocalSocket 传输层（nlohmann 经 core PUBLIC 传播）
add_executable(cli-client cli-client/main.cpp)

target_link_libraries(cli-client
    PRIVATE
        qtet-daemon-core
        Boost::headers
)

# 经 core 传递链接 libeasytier_ffi.so：确保运行时文件已复制到输出目录
add_dependencies(cli-client copy_easytier_ffi_runtime)

# RPATH 与 daemon 一致：安装后在自身目录查找 FFI 运行库
set_target_properties(cli-client PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH FALSE
)
