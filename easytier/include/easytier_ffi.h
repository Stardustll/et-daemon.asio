/**
 * @file easytier_ffi.h
 * @brief EasyTier FFI C 接口头文件
 * @author Myqfeng
 * @version 0.2.0
 *
 * 本头文件提供了 EasyTier Rust 核心库的 C FFI 调用接口，
 * 允许外部程序通过 FFI 方式创建和管理 EasyTier 网络实例。
 *
 * @note 所有字符串参数使用 UTF-8 编码，调用者需确保字符串有效性。
 * @note 返回的字符串指针需要调用 free_string() 释放内存，否则会导致内存泄漏。
 * @note 所有函数均为 C 接口，C++ 代码需用 extern "C" 包裹或包含本头文件（已内置）。
 */

#ifndef EASYTIER_FFI_H
#define EASYTIER_FFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 键值对结构体
 *
 * 用于存储网络实例名称与实例信息的映射关系。
 * - key:   实例名称字符串指针（C 字符串，需调用 free_string 释放）
 * - value: JSON 格式的实例信息字符串指针（需调用 free_string 释放）
 */
typedef struct KeyValuePair {
    const char* key;   /** 实例名称（C 字符串，需调用 free_string 释放） */
    const char* value; /** 实例信息 JSON 字符串（需调用 free_string 释放） */
} KeyValuePair;

/**
 * @brief 配置服务器事件回调函数类型
 *
 * 由 start_config_server_client 注册，当远程配置服务器触发运行或删除实例事件时调用。
 * - event_json: 事件 JSON 字符串指针，仅在回调执行期间有效
 * - user_data:  调用者传入的自定义上下文指针
 */
typedef void (*ConfigServerEventCallback)(const char* event_json, void* user_data);

/**
 * @brief 解析配置字符串（TOML 格式）
 * @param cfg_str TOML 格式的配置字符串
 * @return 0 表示成功，-1 表示失败
 */
int parse_config(const char* cfg_str);

/**
 * @brief 运行网络实例
 * @param cfg_str TOML 格式的配置字符串
 * @return 0 表示成功，-1 表示失败
 *
 * 同名实例会被新配置覆盖。实例在后台线程中运行，
 * 需通过 collect_network_infos() 查询状态。
 */
int run_network_instance(const char* cfg_str);

/**
 * @brief 保留指定的网络实例，移除其他所有实例
 * @param inst_names 保留的实例名称数组（C 字符串指针数组）
 * @param length     数组长度
 * @return 0 表示成功，-1 表示失败
 *
 * 传 NULL 和 0 表示停止所有实例。
 */
int retain_network_instance(const char** inst_names, size_t length);

/**
 * @brief 删除指定的网络实例
 * @param inst_names 要删除的实例名称数组（C 字符串指针数组）
 * @param length     数组长度
 * @return 0 表示成功，-1 表示失败
 *
 * 传 0 表示不执行任何操作。未知名称会被忽略。
 */
int delete_network_instance(const char** inst_names, size_t length);

/**
 * @brief 列出所有运行中网络实例的名称和 ID
 * @param infos       输出缓冲区，由调用者分配
 * @param max_length  缓冲区大小（KeyValuePair 数量）
 * @return >=0 表示实际返回的实例数量，-1 表示失败
 *
 * @warning 返回的 KeyValuePair.key 和 KeyValuePair.value 均为堆分配内存，
 *          必须使用 free_string() 释放。
 */
int list_instance(KeyValuePair* infos, size_t max_length);

/**
 * @brief 收集所有运行中网络实例的信息
 * @param infos       输出缓冲区，由调用者分配
 * @param max_length  缓冲区大小（KeyValuePair 数量）
 * @return >=0 表示实际返回的实例数量，-1 表示失败
 *
 * @warning 返回的 KeyValuePair.key 和 KeyValuePair.value 均为堆分配内存，
 *          必须使用 free_string() 释放。
 */
int collect_network_infos(KeyValuePair* infos, size_t max_length);

/**
 * @brief 设置 TUN 文件描述符
 * @param inst_name 目标实例名称
 * @param fd        TUN 设备的文件描述符
 * @return 0 表示成功，-1 表示失败
 *
 * 绑定后，Rust 核心会通过该 fd 读写 VPN 数据包。
 */
int set_tun_fd(const char* inst_name, int fd);

/**
 * @brief 调用 EasyTier JSON RPC 服务
 * @param service_name        目标 RPC 服务名
 * @param method_name         目标 RPC 方法名
 * @param domain_name         域名，可为 NULL 或空字符串
 * @param payload_json        请求 JSON 字符串
 * @param out_response_json   输出参数，指向响应 JSON 字符串指针
 * @return 0 表示成功，-1 表示失败
 *
 * @warning *out_response_json 指向的内存需要调用 free_string() 释放。
 */
int call_json_rpc(
    const char* service_name,
    const char* method_name,
    const char* domain_name,
    const char* payload_json,
    const char** out_response_json
);

/**
 * @brief 启动配置服务器客户端
 * @param config_server_url 配置服务器 URL
 * @param hostname          主机名，可为 NULL
 * @param machine_id        机器 ID
 * @param secure_mode       是否使用安全模式
 * @param callback          配置服务器事件回调函数
 * @param user_data         传递给回调函数的自定义上下文指针
 * @return 0 表示成功，-1 表示失败
 */
int start_config_server_client(
    const char* config_server_url,
    const char* hostname,
    const char* machine_id,
    bool secure_mode,
    ConfigServerEventCallback callback,
    void* user_data
);

/**
 * @brief 停止配置服务器客户端
 * @return 0 表示成功，-1 表示失败
 */
int stop_config_server_client(void);

/**
 * @brief 查询配置服务器客户端是否已连接
 * @return 已连接返回 1，否则返回 0
 */
int is_config_server_client_connected(void);

/**
 * @brief 通过数据平面建立 TCP 出站连接
 * @param inst_name      实例名称
 * @param dst_ip         目标 IP 字符串
 * @param dst_port       目标端口
 * @param timeout_ms     超时时间（毫秒）
 * @param out_local_ip   输出参数，指向本地 IP 字符串指针
 * @param out_local_port 输出参数，指向本地端口
 * @return 非零 TCP 流句柄表示成功，0 表示失败
 *
 * @warning *out_local_ip 指向的内存需要调用 free_string() 释放。
 */
uint64_t data_plane_tcp_connect(
    const char* inst_name,
    const char* dst_ip,
    unsigned short dst_port,
    uint64_t timeout_ms,
    const char** out_local_ip,
    unsigned short* out_local_port
);

/**
 * @brief 在数据平面绑定 TCP 监听端口
 * @param inst_name      实例名称
 * @param local_port     本地端口，传 0 表示由系统分配
 * @param timeout_ms     超时时间（毫秒）
 * @param out_local_ip   输出参数，指向本地 IP 字符串指针
 * @param out_local_port 输出参数，指向本地端口
 * @return 非零 TCP 监听句柄表示成功，0 表示失败
 *
 * @warning *out_local_ip 指向的内存需要调用 free_string() 释放。
 */
uint64_t data_plane_tcp_bind(
    const char* inst_name,
    unsigned short local_port,
    uint64_t timeout_ms,
    const char** out_local_ip,
    unsigned short* out_local_port
);

/**
 * @brief 接受一个数据平面 TCP 连接
 * @param handle           TCP 监听句柄
 * @param timeout_ms       超时时间（毫秒）
 * @param out_local_ip     输出参数，指向本地 IP 字符串指针
 * @param out_local_port   输出参数，指向本地端口
 * @param out_peer_ip      输出参数，指向对端 IP 字符串指针
 * @param out_peer_port    输出参数，指向对端端口
 * @return 非零 TCP 流句柄表示成功，0 表示失败
 *
 * @warning *out_local_ip 和 *out_peer_ip 指向的内存需要调用 free_string() 释放。
 */
uint64_t data_plane_tcp_accept(
    uint64_t handle,
    uint64_t timeout_ms,
    const char** out_local_ip,
    unsigned short* out_local_port,
    const char** out_peer_ip,
    unsigned short* out_peer_port
);

/**
 * @brief 从 TCP 数据平面流读取数据
 * @param handle     TCP 流句柄
 * @param buf        写入缓冲区
 * @param len        缓冲区长度
 * @param timeout_ms 超时时间（毫秒）
 * @return >=0 表示读取的字节数，-1 表示失败
 */
int data_plane_tcp_read(
    uint64_t handle,
    unsigned char* buf,
    uint32_t len,
    uint64_t timeout_ms
);

/**
 * @brief 向 TCP 数据平面流写入数据
 * @param handle     TCP 流句柄
 * @param buf        读取缓冲区
 * @param len        要写入的字节数
 * @param timeout_ms 超时时间（毫秒）
 * @return len 表示成功，-1 表示失败
 */
int data_plane_tcp_write(
    uint64_t handle,
    const unsigned char* buf,
    uint32_t len,
    uint64_t timeout_ms
);

/**
 * @brief 关闭 TCP 数据平面流
 * @param handle TCP 流句柄
 * @return 0 表示成功，-1 表示失败
 */
int data_plane_tcp_close(uint64_t handle);

/**
 * @brief 关闭 TCP 数据平面监听器
 * @param handle TCP 监听句柄
 * @return 0 表示成功，-1 表示失败
 */
int data_plane_tcp_listener_close(uint64_t handle);

/**
 * @brief 在数据平面绑定 UDP 套接字
 * @param inst_name      实例名称
 * @param local_port     本地端口，传 0 表示由系统分配
 * @param timeout_ms     超时时间（毫秒）
 * @param out_local_ip   输出参数，指向本地 IP 字符串指针
 * @param out_local_port 输出参数，指向本地端口
 * @return 非零 UDP 套接字句柄表示成功，0 表示失败
 *
 * @warning *out_local_ip 指向的内存需要调用 free_string() 释放。
 */
uint64_t data_plane_udp_bind(
    const char* inst_name,
    unsigned short local_port,
    uint64_t timeout_ms,
    const char** out_local_ip,
    unsigned short* out_local_port
);

/**
 * @brief 通过 UDP 数据平面套接字发送数据报
 * @param handle     UDP 套接字句柄
 * @param dst_ip     目标 IP 字符串
 * @param dst_port   目标端口
 * @param buf        数据缓冲区
 * @param len        数据长度
 * @param timeout_ms 超时时间（毫秒）
 * @return >=0 表示发送的字节数，-1 表示失败
 */
int data_plane_udp_send_to(
    uint64_t handle,
    const char* dst_ip,
    unsigned short dst_port,
    const unsigned char* buf,
    uint32_t len,
    uint64_t timeout_ms
);

/**
 * @brief 从 UDP 数据平面套接字接收数据报
 * @param handle     UDP 套接字句柄
 * @param buf        写入缓冲区
 * @param len        缓冲区长度
 * @param out_ip     输出参数，指向对端 IP 字符串指针
 * @param out_port   输出参数，指向对端端口
 * @param timeout_ms 超时时间（毫秒）
 * @return >=0 表示接收的字节数，-1 表示失败
 *
 * @warning *out_ip 指向的内存需要调用 free_string() 释放。
 */
int data_plane_udp_recv_from(
    uint64_t handle,
    unsigned char* buf,
    uint32_t len,
    const char** out_ip,
    unsigned short* out_port,
    uint64_t timeout_ms
);

/**
 * @brief 关闭 UDP 数据平面套接字
 * @param handle UDP 套接字句柄
 * @return 0 表示成功，-1 表示失败
 */
int data_plane_udp_close(uint64_t handle);

/**
 * @brief 查询异步数据平面操作状态
 * @param handle 异步操作句柄
 * @return 状态码
 */
int data_plane_async_op_status(uint64_t handle);

/**
 * @brief 等待异步数据平面操作完成
 * @param handle     异步操作句柄
 * @param timeout_ms 超时时间（毫秒）
 * @return 状态码
 */
int data_plane_async_op_wait(uint64_t handle, uint64_t timeout_ms);

/**
 * @brief 取消异步数据平面操作
 * @param handle 异步操作句柄
 * @return 0 表示成功，-1 表示失败
 */
int data_plane_async_op_cancel(uint64_t handle);

/**
 * @brief 释放异步数据平面操作句柄
 * @param handle 异步操作句柄
 * @return 0 表示成功，-1 表示失败
 */
int data_plane_async_op_free(uint64_t handle);

/**
 * @brief 释放由数据平面异步操作返回的字节缓冲区
 * @param ptr 缓冲区指针
 * @param len 缓冲区长度
 */
void data_plane_free_bytes(const unsigned char* ptr, uint32_t len);

/**
 * @brief 启动异步 TCP 数据平面连接
 * @param inst_name  实例名称
 * @param dst_ip     目标 IP 字符串
 * @param dst_port   目标端口
 * @param timeout_ms 超时时间（毫秒）
 * @return 非零异步操作句柄表示成功，0 表示失败
 */
uint64_t data_plane_tcp_connect_start(
    const char* inst_name,
    const char* dst_ip,
    unsigned short dst_port,
    uint64_t timeout_ms
);

/**
 * @brief 完成异步 TCP 数据平面连接
 * @param op_handle      异步操作句柄
 * @param out_local_ip   输出参数，指向本地 IP 字符串指针
 * @param out_local_port 输出参数，指向本地端口
 * @return 非零 TCP 流句柄表示成功，0 表示失败
 *
 * @warning *out_local_ip 指向的内存需要调用 free_string() 释放。
 */
uint64_t data_plane_tcp_connect_finish(
    uint64_t op_handle,
    const char** out_local_ip,
    unsigned short* out_local_port
);

/**
 * @brief 启动异步 TCP 数据平面绑定
 * @param inst_name  实例名称
 * @param local_port 本地端口，传 0 表示由系统分配
 * @param timeout_ms 超时时间（毫秒）
 * @return 非零异步操作句柄表示成功，0 表示失败
 */
uint64_t data_plane_tcp_bind_start(
    const char* inst_name,
    unsigned short local_port,
    uint64_t timeout_ms
);

/**
 * @brief 完成异步 TCP 数据平面绑定
 * @param op_handle      异步操作句柄
 * @param out_local_ip   输出参数，指向本地 IP 字符串指针
 * @param out_local_port 输出参数，指向本地端口
 * @return 非零 TCP 监听句柄表示成功，0 表示失败
 *
 * @warning *out_local_ip 指向的内存需要调用 free_string() 释放。
 */
uint64_t data_plane_tcp_bind_finish(
    uint64_t op_handle,
    const char** out_local_ip,
    unsigned short* out_local_port
);

/**
 * @brief 启动异步 TCP 数据平面接受连接
 * @param handle     TCP 监听句柄
 * @param timeout_ms 超时时间（毫秒）
 * @return 非零异步操作句柄表示成功，0 表示失败
 */
uint64_t data_plane_tcp_accept_start(uint64_t handle, uint64_t timeout_ms);

/**
 * @brief 完成异步 TCP 数据平面接受连接
 * @param op_handle      异步操作句柄
 * @param out_local_ip   输出参数，指向本地 IP 字符串指针
 * @param out_local_port 输出参数，指向本地端口
 * @param out_peer_ip    输出参数，指向对端 IP 字符串指针
 * @param out_peer_port  输出参数，指向对端端口
 * @return 非零 TCP 流句柄表示成功，0 表示失败
 *
 * @warning *out_local_ip 和 *out_peer_ip 指向的内存需要调用 free_string() 释放。
 */
uint64_t data_plane_tcp_accept_finish(
    uint64_t op_handle,
    const char** out_local_ip,
    unsigned short* out_local_port,
    const char** out_peer_ip,
    unsigned short* out_peer_port
);

/**
 * @brief 启动异步 TCP 数据平面读取
 * @param handle   TCP 流句柄
 * @param max_len  最大读取长度
 * @param timeout_ms 超时时间（毫秒）
 * @return 非零异步操作句柄表示成功，0 表示失败
 */
uint64_t data_plane_tcp_read_start(
    uint64_t handle,
    uint32_t max_len,
    uint64_t timeout_ms
);

/**
 * @brief 完成异步 TCP 数据平面读取
 * @param op_handle 异步操作句柄
 * @param out_buf   输出参数，指向读取缓冲区指针
 * @param out_len   输出参数，指向读取长度
 * @return >=0 表示读取的字节数，-1 表示失败
 *
 * @warning *out_buf 指向的内存需要调用 data_plane_free_bytes() 释放。
 */
int data_plane_tcp_read_finish(
    uint64_t op_handle,
    const unsigned char** out_buf,
    uint32_t* out_len
);

/**
 * @brief 启动异步 TCP 数据平面写入
 * @param handle     TCP 流句柄
 * @param buf        数据缓冲区
 * @param len        数据长度
 * @param timeout_ms 超时时间（毫秒）
 * @return 非零异步操作句柄表示成功，0 表示失败
 */
uint64_t data_plane_tcp_write_start(
    uint64_t handle,
    const unsigned char* buf,
    uint32_t len,
    uint64_t timeout_ms
);

/**
 * @brief 完成异步 TCP 数据平面写入
 * @param op_handle 异步操作句柄
 * @return 0 表示成功，-1 表示失败
 */
int data_plane_tcp_write_finish(uint64_t op_handle);

/**
 * @brief 启动异步 UDP 数据平面绑定
 * @param inst_name  实例名称
 * @param local_port 本地端口，传 0 表示由系统分配
 * @param timeout_ms 超时时间（毫秒）
 * @return 非零异步操作句柄表示成功，0 表示失败
 */
uint64_t data_plane_udp_bind_start(
    const char* inst_name,
    unsigned short local_port,
    uint64_t timeout_ms
);

/**
 * @brief 完成异步 UDP 数据平面绑定
 * @param op_handle      异步操作句柄
 * @param out_local_ip   输出参数，指向本地 IP 字符串指针
 * @param out_local_port 输出参数，指向本地端口
 * @return 非零 UDP 套接字句柄表示成功，0 表示失败
 *
 * @warning *out_local_ip 指向的内存需要调用 free_string() 释放。
 */
uint64_t data_plane_udp_bind_finish(
    uint64_t op_handle,
    const char** out_local_ip,
    unsigned short* out_local_port
);

/**
 * @brief 启动异步 UDP 数据平面发送
 * @param handle     UDP 套接字句柄
 * @param dst_ip     目标 IP 字符串
 * @param dst_port   目标端口
 * @param buf        数据缓冲区
 * @param len        数据长度
 * @param timeout_ms 超时时间（毫秒）
 * @return 非零异步操作句柄表示成功，0 表示失败
 */
uint64_t data_plane_udp_send_to_start(
    uint64_t handle,
    const char* dst_ip,
    unsigned short dst_port,
    const unsigned char* buf,
    uint32_t len,
    uint64_t timeout_ms
);

/**
 * @brief 完成异步 UDP 数据平面发送
 * @param op_handle 异步操作句柄
 * @return 0 表示成功，-1 表示失败
 */
int data_plane_udp_send_to_finish(uint64_t op_handle);

/**
 * @brief 启动异步 UDP 数据平面接收
 * @param handle     UDP 套接字句柄
 * @param max_len    最大接收长度
 * @param timeout_ms 超时时间（毫秒）
 * @return 非零异步操作句柄表示成功，0 表示失败
 */
uint64_t data_plane_udp_recv_from_start(
    uint64_t handle,
    uint32_t max_len,
    uint64_t timeout_ms
);

/**
 * @brief 完成异步 UDP 数据平面接收
 * @param op_handle 异步操作句柄
 * @param out_buf   输出参数，指向接收缓冲区指针
 * @param out_len   输出参数，指向接收长度
 * @param out_ip    输出参数，指向对端 IP 字符串指针
 * @param out_port  输出参数，指向对端端口
 * @return >=0 表示接收的字节数，-1 表示失败
 *
 * @warning *out_buf 指向的内存需要调用 data_plane_free_bytes() 释放；
 *          *out_ip 指向的内存需要调用 free_string() 释放。
 */
int data_plane_udp_recv_from_finish(
    uint64_t op_handle,
    const unsigned char** out_buf,
    uint32_t* out_len,
    const char** out_ip,
    unsigned short* out_port
);

/**
 * @brief 获取最近的错误信息
 * @param out 输出参数，指向错误字符串指针
 *
 * @warning *out 指向的内存需要调用 free_string() 释放。
 */
void get_error_msg(const char** out);

/**
 * @brief 释放由 FFI 分配的字符串内存
 * @param s 要释放的字符串指针
 *
 * 所有从 FFI 函数返回的字符串（collect_network_infos 的 key/value、
 * get_error_msg 的 out、数据平面地址输出参数等）都必须通过此函数释放。
 */
void free_string(const char* s);

#ifdef __cplusplus
}
#endif

#endif /* EASYTIER_FFI_H */
