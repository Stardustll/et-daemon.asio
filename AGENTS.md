# AGENTS.md — qtet-daemon.asio 开发上下文

QtEasyTier 的后端 daemon（**无 Qt 依赖**的 Boost.Asio/C++17 版本）：通过 FFI 调用 EasyTier Rust 核心（`libeasytier_ffi`），经本地 Socket（Unix socket / Windows 命名管道）向前端暴露 JSON-RPC 风格管理接口。仅管理面（实例生命周期/状态/配置），不碰数据面。完整 IPC 协议与 API 字段说明见 `README.md`——修改协议相关代码前必读。

## 常用命令

```bash
# 构建（CMake 3.19+，产物统一输出 build/Output/）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 全部测试 / 单个测试（doctest，每个 tst_*.cpp 是独立可执行文件）
ctest --test-dir build --output-on-failure
ctest --test-dir build -R tst_protocol --output-on-failure
```

- 配置时自动从 CNB 下载 EasyTier FFI 预编译包（v2.6.4，逻辑在 `cmake/download_ffi.cmake`），需联网；可 `-DDOWNLOAD_FFI_FROM_CNB=OFF` 切到 GitHub 源。
- CMake 选项：`-DDAEMON_SOCKET_NAME=...`（默认 `qtet-daemon.sock`；macOS 默认 `/var/run/qtet-daemon.sock`，因 root daemon 与普通用户前端 `TMPDIR` 不同。运行时 `--socket` / `QTET_DAEMON_SOCKET` 可覆盖，见 `src/daemon/SocketName.h`。该值同时影响 Windows 服务名与回连缓存文件名）。
- macOS：默认 Universal（`arm64;x86_64`）+ 最低 13.0；FFI 预编译包按架构分开发布，`cmake/download_ffi.cmake` 在 Universal 构建时下载双架构并 lipo 合成，同时把 install name 修为 `@rpath/libeasytier_ffi.dylib` 并重新 ad-hoc 签名（daemon 侧 RPATH 为 `@loader_path`）。
- 编译器：GCC/Clang、Windows 用 MinGW/LLVM-MinGW，**不支持 MSVC**。

## 架构分层

```
examples/cli-client/   示例 CLI 前端（演示 IPC 协议用法）
src/main.cpp           入口：参数解析、io_context + thread_pool 组装
src/daemon/Protocol    帧协议（4 字节大端长度前缀 + JSON）与请求/响应序列化
src/daemon/IpcServer   多客户端会话模型：每会话独立帧缓冲与 dispatcher，
                       会话内串行调度 FFI，跨会话并行
src/daemon/ReconnectStore  自动回连配置的 JSON 持久化（<socket名>.json）
src/ffis/FfiDispatcher 方法名 → EasyTierFFI 调用，含按字段的 Base64 编解码
src/ipc/               本地 Socket 平台抽象（LocalSocket/LocalAcceptor/LocalEndpoint，
                       Posix/Windows 两个实现文件）
src/windows-register/  Windows 服务注册（SCM），仅 WIN32 参与编译
easytier/include/      EasyTier FFI 上游头文件——不修改
third_party/           单头库：nlohmann/json、doctest、toml++——不修改
```

**线程模型**（改动前先理解，见 `IpcServer.h` 头注释）：所有回调在驱动 `io_context` 的单线程上串行执行，会话状态无锁；阻塞式 FFI 调用经 `FfiDispatcher::dispatchAsync` 投递到 `asio::thread_pool`（main.cpp 固定 4 线程），完成后 post 回 io 线程执行 completion。`IpcServer` 生命周期须延续到 io 停止且残留回调排干（main 中有对应的 restart-再跑一轮逻辑）。

## 编码约定

- 代码注释、文档、CMake 注释均用中文；文件头带 `@file`/`@brief` Doxygen 注释，重要设计决策写在头文件注释里。
- 日志用 `src/log.h` 的 `LOG_INFO/LOG_WARNING/LOG_ERROR` 宏；命名空间是 `logging` 而非 `log`（避开 POSIX `::log` 冲突，勿改回）。
- include 风格：`#include "daemon/Protocol.h"`（src/ 在 include 路径）；`ffis/`、`ipc/` 目录本身也在 include 路径上，故 `#include "FfiDispatcher.h"`、`#include "LocalSocket.h"` 直接可用。新源文件须同时登记进 `src/CMakeLists.txt` 的 `qtet-daemon-core` 静态库（测试与可执行文件只链 core，不直接编译 src/*.cpp）。
- Base64 只用于 README API 参考中明确标注的字段（TOML 配置、状态 JSON、错误消息等），不要按 C++ `std::string` 类型推断。
- 本工程是 Qt 版 daemon 的移植（"父工程"），多处注释以 Qt 版行为为对齐基准；改动语义时保持对齐或在注释中说明差异。

## 已知坑

- 帧上限 `kMaxFrameSize = 16 MiB`（`Protocol.h`），零长度/超长帧直接断开客户端，不要放宽该上限。
- `tst_integration` 仅非 Windows 构建（依赖 POSIX unlink/unix socket）；`tst_dispatcher` 运行时需能加载 `libeasytier_ffi.so`（构建时已复制到 `build/Output/` 并设好 RPATH）。
- daemon 以 root/管理员运行（TUN 操作需要），socket 无鉴权、任何本地用户可连——不要往 IPC 里加需保密的数据。
- `.cnb.yml`：CNB CI 仅做 push 镜像到 GitHub（push 触发 + 分支详情页「同步到 GitHub」手动按钮，按钮定义在 `.cnb/web_trigger.yml`），无构建/测试流水线；验证靠本地 ctest。
- `build/` 与 `cmake-build-debug/` 均为构建目录，不要提交其中内容。
