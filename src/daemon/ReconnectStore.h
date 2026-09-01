/// @file ReconnectStore.h
/// @brief 自动回连实例缓存的 JSON 持久化存储（无 Qt 依赖版本）
///
/// 磁盘格式（4 空格缩进，对齐 Qt 版 QJsonDocument::Indented）：
/// {
///     "autoReconnect": false,
///     "instances": [ { "instance_name": "...", "cfg_str": "<Base64 配置>" } ]
/// }
///
/// 全部公有方法由类级互斥量串行化；写入采用临时文件 + rename 原子替换。

#ifndef RECONNECTSTORE_H
#define RECONNECTSTORE_H

#include <filesystem>
#include <string>
#include <vector>

/// 自动回连持久化存储
class ReconnectStore
{
public:
    struct StoredInstance
    {
        std::string instanceName;  ///< 实例名（TOML 根域 instance_name）
        std::string cfgBase64;     ///< Base64 编码的完整配置
    };

    /// baseDir 为空 → exeDirectory()；测试传临时目录
    explicit ReconnectStore(const std::string& socketName,
                            std::filesystem::path baseDir = {});

    /// 配置文件完整路径：<baseDir>/<socketName>.json
    std::filesystem::path filePath() const;

    bool autoReconnect(bool* enabled, std::string* error = nullptr) const;
    bool setAutoReconnect(bool enabled, std::string* error = nullptr);

    bool loadInstances(std::vector<StoredInstance>* instances, std::string* error = nullptr) const;
    bool saveInstance(const std::string& instanceName, const std::string& cfgBase64, std::string* error = nullptr);
    bool removeInstances(const std::vector<std::string>& instanceNames, std::string* error = nullptr);

private:
    std::filesystem::path m_baseDir;
    std::string m_socketName;
};

#endif // RECONNECTSTORE_H
