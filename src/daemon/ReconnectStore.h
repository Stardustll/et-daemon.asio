/// @file ReconnectStore.h
/// @brief 自动回连实例缓存的 JSON 持久化存储（无 Qt 依赖版本）
///
/// 磁盘格式（4 空格缩进，对齐 Qt 版 QJsonDocument::Indented）：
/// {
///     "autoReconnect": false,
///     "instances": [ { "instance_name": "...", "cfg_str": "<Base64 配置>" } ]
/// }
///
/// 全部公有方法由类级互斥量串行化（io 线程与 FFI 线程池可并发调用）；
/// 写入采用临时文件 + fsync + rename 原子替换。

#ifndef RECONNECTSTORE_H
#define RECONNECTSTORE_H

#include <filesystem>
#include <string>
#include <vector>

/// 自动回连持久化存储：维护 <baseDir>/<socketName>.json 的读写
///
/// 承载两类数据：autoReconnect 总开关（daemon 启动时决定是否回连）与
/// 最近成功启动的实例配置缓存（run 成功写入、delete/回连失败移除）。
class ReconnectStore
{
public:
    /// 单条缓存实例
    struct StoredInstance
    {
        std::string instanceName;  ///< 实例名（TOML 根域 instance_name）
        std::string cfgBase64;     ///< Base64 编码的完整配置
    };

    /// @param socketName IPC socket 名称，决定缓存文件名（取文件名段：<baseName>.json；
    ///        绝对路径 socket 名同样只取文件名段，避免 operator/ 覆盖 baseDir）
    /// @param baseDir 缓存文件所在目录；为空 → 可执行文件目录 exeDirectory()，
    ///        测试传临时目录以隔离
    explicit ReconnectStore(const std::string& socketName,
                            std::filesystem::path baseDir = {});

    /// 配置文件完整路径
    /// @return <baseDir>/<socketName>.json 拼接结果
    std::filesystem::path filePath() const;

    /// 读取自动回连总开关
    /// @param[out] enabled 输出开关当前值（文件缺失或字段缺失/非布尔时为 false）
    /// @param[out] error 可选，失败时填写可读原因
    /// @return 读取成功与否；enabled 为空指针时直接失败
    bool autoReconnect(bool* enabled, std::string* error = nullptr) const;

    /// 写入自动回连总开关（读改写：保留 instances 列表不变）
    /// @param enabled 目标开关值
    /// @param[out] error 可选，失败时填写可读原因
    /// @return 写入成功与否
    bool setAutoReconnect(bool enabled, std::string* error = nullptr);

    /// 加载全部缓存实例
    /// @param[out] instances 输出缓存列表（先清空；instance_name/cfg_str 任一
    ///             为空或非字符串的条目跳过）
    /// @param[out] error 可选，失败时填写可读原因
    /// @return 读取成功与否；instances 为空指针时直接失败
    bool loadInstances(std::vector<StoredInstance>* instances, std::string* error = nullptr) const;

    /// 保存/更新一条实例缓存：同名条目覆盖 cfg_str，否则追加到列表末尾。
    /// instanceName 为空时静默成功（TOML 未解析出 instance_name 则不缓存）。
    /// @param instanceName 实例名（缓存键）
    /// @param cfgBase64 Base64 编码的完整 TOML 配置
    /// @param[out] error 可选，失败时填写可读原因
    /// @return 写入成功与否
    bool saveInstance(const std::string& instanceName, const std::string& cfgBase64, std::string* error = nullptr);

    /// 从缓存移除指定实例（按 instance_name 匹配，未列出的条目保留）。
    /// 空列表静默成功；无匹配项时文件内容不变。
    /// @param instanceNames 要移除的实例名列表
    /// @param[out] error 可选，失败时填写可读原因
    /// @return 写入成功与否
    bool removeInstances(const std::vector<std::string>& instanceNames, std::string* error = nullptr);

private:
    std::filesystem::path m_baseDir;   ///< 缓存文件所在目录
    std::string m_socketName;          ///< socket 名称（决定缓存文件名）
};

#endif // RECONNECTSTORE_H
