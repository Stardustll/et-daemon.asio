# QtEasyTier Daemon

基于 Qt6（Core/Network/Concurrent）的 EasyTier 异地组网后端 daemon 程序，通过本地 Socket 向前端提供 JSON IPC 管理接口。本项目主要用于 QtEasyTier 相关项目的后端守护进程，当在其他非 Qt 项目中使用时，可能会因为引入 Qt 相关动态库而导致程序体积膨胀。

## 项目背景

**EasyTier** 是一款去中心化的异地组网和内网穿透工具。由于 EasyTier 后端（FFI）在执行创建 TUN 网卡等操作时需要高权限（Linux 下 root、Windows 下管理员），而前端 UI 不宜以高权限运行，因此设计了本独立后端 daemon。

**qtet-daemon** 通过 FFI 调用 EasyTier 的 Rust 核心库（Linux 下为 `libeasytier_ffi.so`，Windows 下为 `easytier_ffi.dll`，macOS 下为 `libeasytier_ffi.dylib`），封装并暴露其管理功能，向上层前端提供 JSON IPC 接口。daemon 本身不实现组网逻辑——它仅仅是 EasyTier Rust 核心的管理代理。

Qt 应用组织名为 `qteasytier`，应用名为 `Daemon`。daemon 会在 `QStandardPaths::AppConfigLocation` 指向的系统应用配置目录下维护 `<socket名称>.json`，用于保存自动回连开关和最近成功启动的实例配置。

**职责边界**：本 daemon 专注于**管理层面**（实例生命周期、状态监控、配置管理、前端接口暴露），**不直接操作数据平面**（TCP/UDP 连接、数据读写）。

## 构建

### 依赖

- **CMake** 3.19+
- **Qt6** 6.5+（Core、Network、Concurrent、Test 四个模块）
- **支持 C++17 的编译器**（Linux/macOS 用 GCC/Clang，Windows 用 MSVC 或 MinGW）
- `libeasytier_ffi` 动态库**无需手动安装**——CMake 配置时自动下载对应平台的预编译包（EasyTier FFI v2.6.4）
- 可选：Ninja（不安装则使用 CMake 平台默认生成器）

### 编译

Linux / macOS：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Windows（PowerShell）：

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8    # -j 参数也可省略
```

> **CMake 选项**：
> - `-DDAEMON_SOCKET_NAME=...`：自定义 IPC socket 名称，默认为 `qtet-daemon.sock`
> - `-DDOWNLOAD_FFI_FROM_CNB=OFF`：切换 FFI 下载源，默认从 CNB 下载，关闭后改从 GitHub Releases 下载

生成产物统一输出到 `build/Output/`：

| 目标 | 路径 |
|---|---|
| daemon | `build/Output/qtet-daemon`（Windows 下为 `qtet-daemon.exe`） |
| CLI 客户端 | `build/Output/cli-client`（Windows 下为 `cli-client.exe`） |
| 测试 | `build/Output/tst_*` |

> FFI 动态库会随构建自动复制到 `build/Output/`，daemon 通过 RPATH（构建目录）或 `$ORIGIN`（安装目录）在自身目录下查找，无需手动配置库路径。

### 安装

```bash
cmake --install build --prefix /usr/local
```

安装规则会把 daemon 可执行文件与 FFI 动态库一并安装到同一目录，可直接运行。

## 运行

Linux：

```bash
# 启动 daemon（需要 root 权限，因为要创建 TUN 网卡）
sudo -E ./build/Output/qtet-daemon

# 通过 CLI 客户端调用 daemon（无需 root）
./build/Output/cli-client list_instances
```

Windows：以**管理员身份**运行 `build\Output\qtet-daemon.exe`（创建 TUN 网卡需要管理员权限）；CLI 客户端 `build\Output\cli-client.exe` 调用无需管理员。

> 如果 daemon 在非特权模式下运行，`run_network_instance` 等需要 TUN 设备的操作会失败，但 IPC 通信功能正常。

## Windows 服务管理

Windows 下可将 daemon 注册为系统服务，随系统自动启动（`SERVICE_AUTO_START`），无需手动运行。服务内部名为 `qtet-daemon.sock`（等于 IPC socket 名，自定义构建时同步变化），显示名为 `QtEasyTier Daemon Service`。

```powershell
# 注册服务（服务命令行自动携带 --daemon，防止程序崩溃退出）
qtet-daemon.exe --install

# 启动 / 停止服务
qtet-daemon.exe --start
qtet-daemon.exe --stop

# 卸载服务（运行中会先停止）
qtet-daemon.exe --uninstall

# 查询服务状态
sc query qtet-daemon.sock
```

> **说明**：
> - `--install`/`--start`/`--stop`/`--uninstall` 需要管理员权限；未以管理员身份运行时程序会自动弹出 UAC 提权对话框并重跑自身，无需手动开管理员终端
> - 普通前台模式（无参数运行）与 `--daemon` 模式不做提权判断，以任意权限运行
> - 服务管理命令只操作服务控制管理器，不初始化 FFI、不监听 socket，执行完即退出
> - `--daemon` 由服务控制管理器拉起时以服务模式运行；手动在控制台运行 `qtet-daemon.exe --daemon` 时会回退为前台模式，便于调试
> - 服务模式下日志无控制台可见；服务以 LocalSystem 账户运行，需确保 `easytier_ffi.dll` 与 `qtet-daemon.exe` 同目录

## 项目结构

```
qtet-daemon/
├── CMakeLists.txt                     # 顶层 CMake
├── README.md
├── AGENTS.md                          # 开发上下文文档
│
├── cmake/
│   └── download_ffi.cmake             # EasyTier FFI 自动下载与导入
│
├── script/
│   └── build_daemon.sh                # 一键克隆并构建脚本（Linux/macOS）
│
├── src/
│   ├── main.cpp                       # 应用入口
│   ├── CMakeLists.txt
│   ├── daemon/
│   │   ├── Protocol.h / .cpp          # 通信协议层（JSON/Base64 辅助/帧）
│   │   ├── IpcServer.h / .cpp         # IPC 服务端（QLocalServer）
│   │   └── ReconnectStore.h / .cpp    # 自动回连配置的 JSON 持久化
│   ├── ffis/
│   │   └── FfiDispatcher.h / .cpp     # FFI 调度层（方法→EasyTierFFI）
│   └── windows-register/              # Windows 服务注册与控制（SCM API）
│       └── WindowsService.h / .cpp    # --daemon 服务入口 + 服务管理命令
│
├── tests/
│   ├── CMakeLists.txt
│   ├── tst_protocol.cpp               # 协议层单元测试
│   ├── tst_dispatcher.cpp             # 调度层单元测试
│   ├── tst_reconnect_store.cpp        # 自动回连配置持久化测试
│   └── tst_integration.cpp            # 端到端集成测试
│
├── examples/
│   ├── CMakeLists.txt
│   └── cli-client/
│       ├── CMakeLists.txt
│       └── main.cpp                   # 示例 CLI 前端
│
└── easytier/                          # EasyTier FFI 头文件（上游依赖，不修改）
    ├── bin/                           # 空占位目录（构建未引用，动态库在 configure 时下载到 build 目录）
    └── include/
        ├── easytier_ffi.h             # C FFI 头文件
        └── EasyTierFFI.h              # C++ 封装头文件
```

## IPC 通信协议

本 daemon 使用**三层协议栈**与前端通信：传输层 → 帧协议 → 消息格式。

### 传输层：本地 Socket

| 项 | 值 |
|---|---|
| 传输方式 | QLocalSocket / QLocalServer（Linux/macOS 为 Unix Domain Socket，Windows 为命名管道） |
| Socket 名称 | `qtet-daemon.sock`（Linux 下实际路径为 `/tmp/qtet-daemon.sock`，Windows 下为 `\\.\pipe\qtet-daemon.sock`） |

前端只需使用标准 QLocalSocket 客户端连接名为 `qtet-daemon.sock` 的 socket 即可。在 Qt 中：

```cpp
QLocalSocket sock;
sock.connectToServer("qtet-daemon.sock");
```

在其他语言（Python 示例，仅 Linux）：

```python
import socket
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect("/tmp/qtet-daemon.sock")
```

> 若 daemon 以自定义 socket 名称构建（`-DDAEMON_SOCKET_NAME=...`），上述路径需同步替换。

### 帧协议：长度前缀分帧

TCP 是流式协议，不保证消息边界。本协议使用**4 字节大端长度前缀**进行帧分隔：

```
┌──────────────────────────────────────────────┐
│  Payload Length (4 bytes, Big-Endian uint32) │
├──────────────────────────────────────────────┤
│  Payload (JSON bytes, UTF-8)                 │
└──────────────────────────────────────────────┘
```

**编码规则（写帧）**：

```
1. 获取 JSON 字节数 N（uint32）
2. 构造 4 字节大端长度头：byte[0]=(N>>24)&0xFF, byte[1]=(N>>16)&0xFF,
                         byte[2]=(N>>8)&0xFF,   byte[3]=N&0xFF
3. 写入 socket：长度头 + JSON 字节
4. flush socket
```

**解码规则（读帧）**：

```
1. 从 socket 读取所有可用数据，追加到接收缓冲区
2. 如果缓冲区 < 4 字节，等待更多数据
3. 从前 4 字节解码 payload 长度 N（大端序）
4. 如果缓冲区 < 4+N 字节，等待更多数据
5. 取出缓冲区 [4, 4+N) 作为完整帧 payload
6. 从缓冲区移除 [0, 4+N)
```

**伪代码实现**：

```
function writeFrame(sock, payload):
    len = payload.length
    header = bytes([
        (len >> 24) & 0xFF,
        (len >> 16) & 0xFF,
        (len >> 8)  & 0xFF,
        len         & 0xFF
    ])
    sock.sendall(header + payload)

function readFrame(sock, buffer):
    buffer += sock.recv(65536)
    if len(buffer) < 4:
        return null
    len = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3]
    if len(buffer) < 4 + len:
        return null
    payload = buffer[4 : 4+len]
    buffer = buffer[4+len : ]
    return payload
```

### 消息格式：JSON-RPC 风格

每帧 payload 是一个完整的 JSON 文档（UTF-8 编码，Compact 格式无换行）。

**请求格式**：

```json
{
    "id": 1,
    "method": "方法名",
    "params": {
        "参数1": "值1",
        "参数2": "值2"
    }
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | integer | 请求序号，前端自行维护，响应中会原样返回 |
| `method` | string | 方法名，对应 EasyTierFFI 函数名（snake_case） |
| `params` | object | 方法参数；是否需要 Base64 取决于具体字段，见下方 API 参考 |

**成功响应**：

```json
{
    "id": 1,
    "method": "方法名",
    "result": {
        "字段1": "值1"
    }
}
```

**错误响应**：

```json
{
    "id": 1,
    "method": "方法名",
    "error": {
        "message": "错误描述"
    }
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | integer | 与请求相同 |
| `method` | string | 与请求相同 |
| `result` | object | 成功时的返回数据——与 `error` 互斥 |
| `error` | object | 失败时的错误信息，含 `message` 字段——与 `result` 互斥 |

### Base64 编码规则

Base64 只用于当前接口中明确标注的字段，主要是 TOML 配置、JSON payload、状态 JSON 和错误消息等可能包含复杂文本内容的字段。其他普通标识符、实例名称、整数、布尔值和数组按原始 JSON 类型传输。

调用方应以本节和 API 参考中的字段说明为准，不要简单按 C++ 层 `std::string` 类型推断是否需要 Base64。

**需要 Base64 编码的参数**：

| 方法 | 参数字段 |
|---|---|
| `parse_config` | `cfg_str` |
| `run_network_instance` | `cfg_str` |
| `call_json_rpc` | `payload` |
| `start_config_server_client` | `config_server_url`, `hostname`, `machine_id` |

**需要 Base64 编码的返回值字段**：

| 方法 | 返回字段 |
|---|---|
| `collect_network_infos` | `instances[].value` |
| `call_json_rpc` | `response` |
| `get_error_msg` | `errors[]` |
| `get_last_error_msg` | `message` |

**编码与解码**：

```
发送：Base64Encode(原始UTF-8字符串)
     例如：echo -n "Hello" | base64 → "SGVsbG8="

接收：Base64Decode(接收到的字符串)
     例如：echo -n "SGVsbG8=" | base64 -d → "Hello"
```

**当前不需要 Base64 编码的字段**：

| 方法 | 字段 |
|---|---|
| `delete_network_instance` | `inst_names[]` |
| `list_instances` | `instances[].key`, `instances[].value` |
| `call_json_rpc` | `service_name`, `method_name`, `domain_name` |
| `set_auto_reconnect` | `enabled` |
| `get_auto_reconnect` | 无参数 |
| `poll_config_events` | `events[]` |
| 所有方法 | `id`, `method`, 整数参数、布尔参数 |

---

## API 参考

> **约定**：以下示例中，所有 `<base64>` 标记的值表示实际传输时需替换为 Base64 编码后的字符串。

### parse_config — 解析 TOML 配置

验证 TOML 配置格式合法性，不启动实例。

**请求**：

```json
{
    "id": 1,
    "method": "parse_config",
    "params": {
        "cfg_str": "<Base64编码的TOML内容>"
    }
}
```

**成功响应**：

```json
{
    "id": 1,
    "method": "parse_config",
    "result": {}
}
```

**失败响应**：

```json
{
    "id": 1,
    "method": "parse_config",
    "error": {
        "message": "Invalid TOML syntax at line 3"
    }
}
```

### run_network_instance — 启动网络实例

启动一个 EasyTier 网络实例。同名实例会被覆盖，实例在后台线程运行。启动成功后，daemon 会尝试从 TOML 根域读取 `instance_name`；如果读取成功，会把传入的 Base64 `cfg_str` 写入自动回连缓存。同名缓存会被更新。

**请求**：

```json
{
    "id": 2,
    "method": "run_network_instance",
    "params": {
        "cfg_str": "<Base64编码的TOML配置>"
    }
}
```

**成功响应**：

```json
{
    "id": 2,
    "method": "run_network_instance",
    "result": {}
}
```

> **示例 TOML 配置**（Base64 编码前）：
> ```toml
> instance_name = "my-net"
>
> [instance]
> hostname = "node1"
>
> [[peer]]
> uri = "tcp://example.com:11010"
> ```

### delete_network_instance — 删除指定实例

删除指定的网络实例。空数组不执行任何操作，未知名称会被忽略。删除成功后，daemon 会从自动回连缓存中移除 `inst_names[]` 对应的实例。

**请求**：

```json
{
    "id": 3,
    "method": "delete_network_instance",
    "params": {
        "inst_names": ["my-net", "old-net"]
    }
}
```

**成功响应**：

```json
{
    "id": 3,
    "method": "delete_network_instance",
    "result": {}
}
```

| 参数 | 类型 | 说明 |
|---|---|---|
| `inst_names` | string[] | 要删除的实例名称列表 |

### list_instances — 列出运行中实例

仅返回实例名称和 ID，不含详细状态 JSON。最多返回 64 个实例。

**请求**：

```json
{
    "id": 4,
    "method": "list_instances",
    "params": {}
}
```

**成功响应**：

```json
{
    "id": 4,
    "method": "list_instances",
    "result": {
        "instances": [
            {
                "key": "my-net",
                "value": "abc123def"
            }
        ]
    }
}
```

| 返回字段 | 说明 |
|---|---|
| `instances[].key` | 实例名称 |
| `instances[].value` | 实例 ID |

### collect_network_infos — 收集实例详细状态

返回实例名称和详细状态信息。`instances[].key` 为原始实例名称；`instances[].value` 是 JSON 状态字符串，**Base64 编码后返回**。

**请求**：

```json
{
    "id": 5,
    "method": "collect_network_infos",
    "params": {
        "max_length": 64
    }
}
```

| 参数 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `max_length` | integer | 64 | 最大查询实例数 |

**成功响应**：

```json
{
    "id": 5,
    "method": "collect_network_infos",
    "result": {
        "count": 1,
        "instances": [
            {
                "key": "my-net",
                "value": "<Base64编码的JSON状态信息>"
            }
        ]
    }
}
```

> `value` 字段解码后为 EasyTier 的原生 JSON 状态对象，包含节点列表、连接状态、延迟等信息。

### call_json_rpc — 调用 EasyTier 内部 JSON RPC

调用 EasyTier 核心内部的 JSON RPC 服务。`service_name`、`method_name`、`domain_name` 按普通 JSON 字符串传输；`payload` 需 Base64 编码，`response` 为 Base64 编码返回。

**请求**：

```json
{
    "id": 6,
    "method": "call_json_rpc",
    "params": {
        "service_name": "service",
        "method_name": "method",
        "domain_name": "",
        "payload": "<Base64编码的请求JSON>"
    }
}
```

| 参数 | 类型 | 说明 |
|---|---|---|
| `service_name` | string | 目标 RPC 服务名 |
| `method_name` | string | 目标 RPC 方法名 |
| `domain_name` | string | 域名，可为空字符串 |
| `payload` | string | 请求 JSON 字符串（Base64） |

**成功响应**：

```json
{
    "id": 6,
    "method": "call_json_rpc",
    "result": {
        "response": "<Base64编码的响应JSON>"
    }
}
```

### start_config_server_client — 连接配置服务器

连接到远程配置服务器，接收远程下发的实例启停事件。事件通过 [poll_config_events](#poll_config_events--获取配置服务器事件) 轮询获取。

**请求**：

```json
{
    "id": 7,
    "method": "start_config_server_client",
    "params": {
        "config_server_url": "<Base64编码的URL>",
        "hostname": "<Base64编码的主机名>",
        "machine_id": "<Base64编码的机器ID>",
        "secure_mode": false
    }
}
```

| 参数 | 类型 | 说明 |
|---|---|---|
| `config_server_url` | string | 配置服务器 URL（Base64） |
| `hostname` | string | 主机名，可为空（Base64） |
| `machine_id` | string | 机器唯一标识（Base64） |
| `secure_mode` | boolean | 是否使用安全模式 |

### stop_config_server_client — 断开配置服务器

断开与远程配置服务器的连接。

**请求**：

```json
{
    "id": 8,
    "method": "stop_config_server_client",
    "params": {}
}
```

### is_config_server_client_connected — 查询连接状态

查询是否已连接到配置服务器。

**请求**：

```json
{
    "id": 9,
    "method": "is_config_server_client_connected",
    "params": {}
}
```

**成功响应**：

```json
{
    "id": 9,
    "method": "is_config_server_client_connected",
    "result": {
        "connected": false
    }
}
```

### get_error_msg — 获取所有错误信息

返回 EasyTier FFI 中累积的错误信息列表。每条错误消息为 **Base64 编码**。

**请求**：

```json
{
    "id": 10,
    "method": "get_error_msg",
    "params": {}
}
```

**成功响应**：

```json
{
    "id": 10,
    "method": "get_error_msg",
    "result": {
        "errors": [
            "<Base64编码的错误消息1>",
            "<Base64编码的错误消息2>"
        ]
    }
}
```

### get_last_error_msg — 获取最近一条错误

返回最近一条错误信息（**Base64 编码**）。无错误时返回 `"Unknown error"` 的 Base64。

**请求**：

```json
{
    "id": 11,
    "method": "get_last_error_msg",
    "params": {}
}
```

**成功响应**：

```json
{
    "id": 11,
    "method": "get_last_error_msg",
    "result": {
        "message": "<Base64编码的错误消息>"
    }
}
```

### poll_config_events — 获取配置服务器事件

> **注意**：此方法是 IPC 层的附加方法，用于读取配置服务器回调暂存的事件队列。

轮询获取远程配置服务器推送的待处理事件。调用 `start_config_server_client` 后，远程下发的事件会缓存在内部队列中，需通过此方法取出。

**请求**：

```json
{
    "id": 13,
    "method": "poll_config_events",
    "params": {}
}
```

**成功响应**：

```json
{
    "id": 13,
    "method": "poll_config_events",
    "result": {
        "events": [
            "{...事件JSON...}",
            "{...事件JSON...}"
        ]
    }
}
```

> 如果无待处理事件，`events` 为空数组。

### set_auto_reconnect — 设置自动回连开关

设置 daemon 下次启动时是否自动回连缓存实例。开关保存在 `<socket名称>.json` 的根域 `autoReconnect` 字段中，默认值为 `false`。

**请求**：

```json
{
    "id": 14,
    "method": "set_auto_reconnect",
    "params": {
        "enabled": true
    }
}
```

**成功响应**：

```json
{
    "id": 14,
    "method": "set_auto_reconnect",
    "result": {
        "autoReconnect": true
    }
}
```

### get_auto_reconnect — 查询自动回连开关

查询当前是否启用自动回连。

**请求**：

```json
{
    "id": 15,
    "method": "get_auto_reconnect",
    "params": {}
}
```

**成功响应**：

```json
{
    "id": 15,
    "method": "get_auto_reconnect",
    "result": {
        "autoReconnect": true
    }
}
```

### 自动回连配置文件

配置文件位于 `QStandardPaths::AppConfigLocation` 对应的目录，文件名为 `<socket名称>.json`。默认 socket 名为 `qtet-daemon.sock` 时，文件名为 `qtet-daemon.sock.json`。

```json
{
    "autoReconnect": false,
    "instances": [
        {
            "instance_name": "my-net",
            "cfg_str": "<Base64编码的TOML配置>"
        }
    ]
}
```

当 `autoReconnect` 为 `true` 时，daemon 每次启动都会读取 `instances` 并逐个尝试回连缓存配置：**回连成功的实例保留缓存项**（下次启动继续回连），**回连失败的实例删除对应缓存项**（避免反复重试坏配置）。`autoReconnect` 开关本身不会被自动关闭。

---

## 限制与安全

- **仅支持单客户端**：daemon 同一时刻只接受一个客户端连接，第二个连接会被拒绝，客户端断开后自动解锁。
- **单实例运行**：若 socket 已被其他 daemon 实例监听，新启动的实例会检测到并直接退出（不会移除或覆盖正在运行的实例的 socket）。
- **请求串行执行**：同一时刻只有一个 FFI 调用在线程池中执行。调用期间到达的帧会累积在接收缓冲区，当前调用完成后按序处理（不会丢弃）。
- **无鉴权**：daemon 设置了 `QLocalServer::WorldAccessOption`，允许非 root 前端连接 root 运行的 daemon。socket 上没有任何认证或加密，**任何本地用户都可发起请求**，请仅在本机可信环境运行。
- **事件为内存队列**：配置服务器推送的事件缓存在进程内队列中，daemon 重启后丢失，需通过 `poll_config_events` 及时轮询取出。

## CLI 客户端示例

`examples/cli-client/` 提供了一个最小命令行前端，演示如何通过 IPC 协议与 daemon 通信。

```bash
# 无参数运行：列出所有可用方法
./build/Output/cli-client

# 查看命令行用法
./build/Output/cli-client --help

# 列出运行中的实例
./build/Output/cli-client list_instances

# 查询配置服务器连接状态
./build/Output/cli-client is_config_server_client_connected

# 获取最近一条错误
./build/Output/cli-client get_last_error_msg

# 传递 JSON 参数
./build/Output/cli-client delete_network_instance '{"inst_names":["my-net"]}'
```

## 测试

```bash
# 运行所有测试
ctest --test-dir build --output-on-failure

# 单独运行某个测试
ctest --test-dir build -R tst_protocol --output-on-failure
```

| 测试 | 覆盖内容 |
|---|---|
| `tst_protocol` | JSON 序列化/反序列化、Base64 编解码、帧协议分片、畸形输入 |
| `tst_dispatcher` | 14 个已知方法全路径覆盖、未知方法错误、参数缺失、事件队列 |
| `tst_reconnect_store` | 自动回连配置 JSON 持久化：开关读写、实例 upsert/删除、读取不改写缓存 |
| `tst_integration` | QLocalSocket 端到端 IPC、并发客户端、请求 ID 匹配 |
