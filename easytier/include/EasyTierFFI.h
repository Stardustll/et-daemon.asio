/**
 * @file EasyTierFFI.h
 * @brief EasyTier FFI C++ 封装层
 *
 * 本文件提供对底层 C FFI（easytier_ffi.h）的 C++ 友好封装：
 * - 使用 std::string 替代裸 char*，避免手动内存管理错误
 * - 使用 std::vector 替代 C 数组，支持动态大小
 * - 自动调用 free_string() 释放 FFI 返回的字符串内存
 *
 * @warning 所有返回字符串的 FFI 函数都会分配堆内存，必须通过 free_string() 释放。
 *          本封装层已正确处理内存释放，上层无需关心。
 */

#ifndef EASYTIERFFI_H
#define EASYTIERFFI_H

#include "easytier_ffi.h"
#include <string>
#include <vector>
#include <memory>

namespace EasyTierFFI {

/**
 * @brief 键值对结构体（C++ 风格）
 *
 * 对应 C 层的 KeyValuePair，但使用 std::string 管理内存。
 * key:   网络实例名称
 * value: JSON 格式的实例状态信息
 */
struct KVPair
{
    std::string key;
    std::string value;
};

namespace detail {

/// FFI 返回字符串的 RAII 包装：析构时自动调用 free_string()，
/// 保证 std::string 拷贝抛异常时也不会泄漏 FFI 分配的内存
using FfiString = std::unique_ptr<const char, decltype(&free_string)>;

inline FfiString takeFfiString(const char* s)
{
    return FfiString(s, &free_string);
}

/// 释放一个 C KeyValuePair 中的两个指针并置空，供防御性清理使用
inline void freeKeyValuePair(KeyValuePair& kv)
{
    if (kv.key) { free_string(kv.key); kv.key = nullptr; }
    if (kv.value) { free_string(kv.value); kv.value = nullptr; }
}

} // namespace detail

/**
 * @brief 解析配置字符串（TOML 格式）
 * @param cfgStr TOML 配置字符串
 * @return true 表示解析成功
 */
inline bool parseConfig(const std::string& cfgStr)
{
    if (cfgStr.empty()) return false;
    return parse_config(cfgStr.c_str()) == 0;
}

/**
 * @brief 运行网络实例
 * @param cfgStr TOML 配置字符串
 * @return true 表示启动成功
 *
 * @note 每个实例通过 instance_name 唯一标识，同名实例会被覆盖。
 */
inline bool runNetworkInstance(const std::string& cfgStr)
{
    if (cfgStr.empty()) return false;
    return run_network_instance(cfgStr.c_str()) == 0;
}

/**
 * @brief 删除指定的网络实例
 * @param instNames 要删除的实例名称列表
 * @return true 表示操作成功
 *
 * 传空列表表示不执行任何操作。未知名称会被忽略。
 * 比 retain_network_instance 更直观：直接删除指定实例，无需构建保留列表。
 */
inline bool deleteNetworkInstance(const std::vector<std::string>& instNames)
{
    if (instNames.empty()) return true;

    std::vector<const char*> cStrArray;
    cStrArray.reserve(instNames.size());
    for (const auto& name : instNames)
        cStrArray.push_back(name.c_str());

    return delete_network_instance(cStrArray.data(), cStrArray.size()) == 0;
}

/**
 * @brief 列出所有运行中网络实例的名称和 ID
 * @param infos 输出参数，存放实例名称和 ID
 * @return true 表示操作成功
 *
 * 与 collectNetworkInfos 不同，本函数仅返回名称和 ID，不包含详细状态 JSON。
 */
inline bool listInstances(std::vector<KVPair>& infos)
{
    constexpr size_t kMaxInstances = 64;
    std::vector<KeyValuePair> cInfos(kMaxInstances);
    const int count = list_instance(cInfos.data(), kMaxInstances);

    if (count < 0) return false;

    // 防御：FFI 返回数量超过容量属于违反契约，不得越界读取/释放
    if (static_cast<size_t>(count) > kMaxInstances) {
        for (size_t i = 0; i < kMaxInstances; ++i)
            detail::freeKeyValuePair(cInfos[i]);
        return false;
    }

    infos.clear();
    infos.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        // 先接管所有权，拷贝失败时 RAII 仍会释放，避免异常安全缺口
        detail::FfiString key = detail::takeFfiString(cInfos[i].key);
        detail::FfiString value = detail::takeFfiString(cInfos[i].value);
        KVPair pair;
        if (key)
            pair.key = std::string(key.get());
        if (value)
            pair.value = std::string(value.get());
        infos.push_back(std::move(pair));
    }
    return true;
}

/**
 * @brief 调用 EasyTier JSON RPC 服务
 * @param serviceName  目标 RPC 服务名
 * @param methodName   目标 RPC 方法名
 * @param domainName   域名，可为空
 * @param payloadJson  请求 JSON 字符串
 * @param outResponse  输出参数，响应 JSON 字符串
 * @return true 表示调用成功
 */
inline bool callJsonRpc(
    const std::string& serviceName,
    const std::string& methodName,
    const std::string& domainName,
    const std::string& payloadJson,
    std::string& outResponse)
{
    const char* domainPtr = domainName.empty() ? nullptr : domainName.c_str();
    const char* rawResponse = nullptr;
    int ret = call_json_rpc(
        serviceName.c_str(), methodName.c_str(),
        domainPtr, payloadJson.c_str(), &rawResponse);
    if (ret != 0) return false;
    if (rawResponse != nullptr) {
        detail::FfiString guard = detail::takeFfiString(rawResponse);
        outResponse = std::string(guard.get());
    }
    return true;
}

/**
 * @brief 启动配置服务器客户端
 * @param configServerUrl 配置服务器 URL
 * @param hostname        主机名，可为空
 * @param machineId       机器 ID
 * @param secureMode      是否使用安全模式
 * @param callback        配置服务器事件回调
 * @param userData        传递给回调的自定义上下文指针
 * @return true 表示启动成功
 */
inline bool startConfigServerClient(
    const std::string& configServerUrl,
    const std::string& hostname,
    const std::string& machineId,
    bool secureMode,
    ConfigServerEventCallback callback,
    void* userData)
{
    const char* hostnamePtr = hostname.empty() ? nullptr : hostname.c_str();
    return start_config_server_client(
        configServerUrl.c_str(), hostnamePtr, machineId.c_str(),
        secureMode, callback, userData) == 0;
}

/**
 * @brief 停止配置服务器客户端
 * @return true 表示停止成功
 */
inline bool stopConfigServerClient()
{
    return stop_config_server_client() == 0;
}

/**
 * @brief 查询配置服务器客户端是否已连接
 * @return true 表示已连接
 */
inline bool isConfigServerClientConnected()
{
    return is_config_server_client_connected() != 0;
}

/**
 * @brief 收集所有运行中网络实例的状态信息
 * @param infos      输出参数，存放实例名称和状态 JSON
 * @param maxLength  输入：infos 的最大容量；输出：实际返回的数量
 * @return >=0 表示实例数量，<0 表示错误
 *
 * @warning 返回的 KeyValuePair 中的 key 和 value 指针需要调用 free_string() 释放。
 *          本函数已自动处理释放，并将内容拷贝到 std::string 中。
 */
inline int collectNetworkInfos(std::vector<KVPair>& infos, size_t& maxLength)
{
    std::vector<KeyValuePair> cInfos(maxLength);
    const int count = collect_network_infos(cInfos.data(), maxLength);

    if (count < 0) {
        maxLength = 0;
        return -1;
    }

    // 防御：FFI 返回数量超过容量属于违反契约，不得越界读取/释放
    if (static_cast<size_t>(count) > maxLength) {
        for (size_t i = 0; i < maxLength; ++i)
            detail::freeKeyValuePair(cInfos[i]);
        maxLength = 0;
        return -1;
    }

    infos.clear();
    infos.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        detail::FfiString key = detail::takeFfiString(cInfos[i].key);
        detail::FfiString value = detail::takeFfiString(cInfos[i].value);
        KVPair pair;
        if (key)
            pair.key = std::string(key.get());
        if (value)
            pair.value = std::string(value.get());
        infos.push_back(std::move(pair));
    }
    maxLength = static_cast<size_t>(count);
    return count;
}

/**
 * @brief 获取所有错误信息
 * @return 错误字符串列表
 *
 * @note 当前实现通常只返回最近一条错误信息。
 */
inline std::vector<std::string> getErrorMsg()
{
    const char* errorMsg = nullptr;
    get_error_msg(&errorMsg);
    std::vector<std::string> result;
    if (errorMsg != nullptr) {
        detail::FfiString guard = detail::takeFfiString(errorMsg);
        result.emplace_back(guard.get());
    }
    return result;
}

/**
 * @brief 获取最近一条错误信息
 * @return 错误描述字符串，若无错误则返回 "Unknown error"
 */
inline std::string getLastErrorMsg()
{
    auto errors = getErrorMsg();
    if (errors.empty()) return "Unknown error";
    return errors[0];
}

/**
 * @brief 将 TUN 文件描述符绑定到指定实例
 * @param instName 实例名称
 * @param fd       TUN 设备的文件描述符（由 Android VpnService 创建）
 * @return true 表示绑定成功
 *
 * Rust 核心获得 fd 后，会通过该 fd 读写 IP 数据包，实现 VPN 功能。
 */
inline bool setTunFd(const std::string& instName, int fd)
{
    if (instName.empty()) return false;
    return set_tun_fd(instName.c_str(), fd) == 0;
}

} // namespace EasyTierFFI

#endif // EASYTIERFFI_H
