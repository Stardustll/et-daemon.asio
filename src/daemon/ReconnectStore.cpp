/// @file ReconnectStore.cpp
/// @brief 自动回连实例缓存的 JSON 持久化存储实现

#include "daemon/ReconnectStore.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <mutex>
#include <system_error>

#include <nlohmann/json.hpp>

#include "app_paths.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

/// 类级互斥量：所有实例共享，串行化全部公有方法（对齐 Qt 版全局 QMutex）
std::mutex& reconnectStoreMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// 将已写入文件的数据强制落盘（对齐 Qt 版 QSaveFile::commit 的刷盘语义），
/// 避免 rename 提交了仅存在于页缓存的内容——崩溃/断电窗口内丢数据
bool syncFileToDisk(const std::filesystem::path& file)
{
#if defined(_WIN32)
    const int fd = ::_open(file.string().c_str(), _O_WRONLY);
    if (fd < 0)
        return false;
    const int ok = ::_commit(fd);
    ::_close(fd);
    return ok == 0;
#else
    const int fd = ::open(file.c_str(), O_WRONLY);
    if (fd < 0)
        return false;
    const int ok = ::fsync(fd);
    ::close(fd);
    return ok == 0;
#endif
}

/// 填写可选错误输出（error 为空指针时丢弃消息，调用侧无需判空）
void setError(std::string* error, const std::string& message)
{
    if (error != nullptr) *error = message;
}

/// errno 转可读消息（ifstream 打开/写入失败时补充细节）
std::string errnoMessage()
{
    return std::error_code(errno, std::generic_category()).message();
}

/// 缺省根对象：autoReconnect 关闭 + 空 instances（文件不存在时的初始状态）
nlohmann::json defaultRoot()
{
    return nlohmann::json{
        {"autoReconnect", false},
        {"instances", nlohmann::json::array()},
    };
}

/// 从对象中取字符串字段；不存在或非字符串 → 空串（对齐 QJsonValue::toString）
std::string stringValue(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

/// 读取并解析根对象；文件缺失 → 缺省根；解析失败/根非对象 → 报错。
/// 读入后补缺字段：autoReconnect 缺省 false，instances 缺省或非数组 → 空数组。
bool readRoot(const std::filesystem::path& file, nlohmann::json* root, std::string* error)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        *root = defaultRoot();
        return true;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        setError(error, "failed to open reconnect config: " + errnoMessage());
        return false;
    }

    const std::string content{std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>{}};

    try {
        *root = nlohmann::json::parse(content);
    } catch (const nlohmann::json::parse_error& e) {
        setError(error, std::string("invalid reconnect config json: ") + e.what());
        return false;
    }
    if (!root->is_object()) {
        setError(error, "invalid reconnect config json: root is not an object");
        return false;
    }

    if (!root->contains("autoReconnect")) (*root)["autoReconnect"] = false;
    if (!root->contains("instances") || !(*root)["instances"].is_array())
        (*root)["instances"] = nlohmann::json::array();
    return true;
}

/// 原子写入：先写 <file>.tmp，成功后 rename 覆盖目标文件
/// （对齐 Qt 版 QSaveFile 语义，避免崩溃/磁盘满留下半截 JSON）。
/// 任何写入失败 → 报错、best-effort 清理 tmp 且不执行 rename。
bool writeRoot(const std::filesystem::path& file, const nlohmann::json& root, std::string* error)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        setError(error, "failed to create reconnect config directory");
        return false;
    }

    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            setError(error, "failed to write reconnect config: " + errnoMessage());
            return false;
        }

        const std::string data = root.dump(4);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            setError(error, "failed to write reconnect config: " + errnoMessage());
            std::error_code removeEc;
            std::filesystem::remove(tmp, removeEc);
            return false;
        }
    }

    // 落盘后再 rename，保证提交的文件内容已到达存储（QSaveFile 语义）
    if (!syncFileToDisk(tmp)) {
        setError(error, "failed to sync reconnect config to disk");
        std::error_code removeEc;
        std::filesystem::remove(tmp, removeEc);
        return false;
    }

    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        setError(error, "failed to commit reconnect config: " + ec.message());
        std::error_code removeEc;
        std::filesystem::remove(tmp, removeEc);
        return false;
    }
    return true;
}

} // namespace

ReconnectStore::ReconnectStore(const std::string& socketName, std::filesystem::path baseDir)
    : m_baseDir(baseDir.empty() ? app_paths::exeDirectory() : std::move(baseDir))
    , m_socketName(socketName)
{
}

std::filesystem::path ReconnectStore::filePath() const
{
    // socket 名可能是绝对路径（macOS 下 /var/run/qtet-daemon.sock）：若整体拼接，
    // std::filesystem::path::operator/ 遇绝对右操作数会丢弃 m_baseDir，使缓存落到
    // /var/run 等目录。这里只取文件名段（裸名如 "qtet-daemon.sock" 时 filename()
    // 与原值相同，行为不变），保证缓存始终位于 m_baseDir 下。
    const std::filesystem::path name(m_socketName);
    const std::filesystem::path leaf = name.filename();
    return m_baseDir / ((leaf.empty() ? name : leaf).string() + ".json");
}

bool ReconnectStore::autoReconnect(bool* enabled, std::string* error) const
{
    std::lock_guard<std::mutex> locker(reconnectStoreMutex());

    if (enabled == nullptr) {
        setError(error, "enabled output is null");
        return false;
    }

    nlohmann::json root;
    if (!readRoot(filePath(), &root, error)) return false;

    const auto it = root.find("autoReconnect");
    *enabled = (it != root.end() && it->is_boolean()) ? it->get<bool>() : false;
    return true;
}

bool ReconnectStore::setAutoReconnect(bool enabled, std::string* error)
{
    std::lock_guard<std::mutex> locker(reconnectStoreMutex());

    nlohmann::json root;
    if (!readRoot(filePath(), &root, error)) return false;

    root["autoReconnect"] = enabled;
    return writeRoot(filePath(), root, error);
}

bool ReconnectStore::loadInstances(std::vector<StoredInstance>* instances, std::string* error) const
{
    std::lock_guard<std::mutex> locker(reconnectStoreMutex());

    if (instances == nullptr) {
        setError(error, "instances output is null");
        return false;
    }

    nlohmann::json root;
    if (!readRoot(filePath(), &root, error)) return false;

    instances->clear();
    for (const auto& value : root["instances"]) {
        StoredInstance instance;
        if (value.is_object()) {
            instance.instanceName = stringValue(value, "instance_name");
            instance.cfgBase64 = stringValue(value, "cfg_str");
        }
        if (!instance.instanceName.empty() && !instance.cfgBase64.empty())
            instances->push_back(instance);
    }
    return true;
}

bool ReconnectStore::saveInstance(const std::string& instanceName, const std::string& cfgBase64, std::string* error)
{
    std::lock_guard<std::mutex> locker(reconnectStoreMutex());

    if (instanceName.empty()) return true;

    nlohmann::json root;
    if (!readRoot(filePath(), &root, error)) return false;

    nlohmann::json arr = root["instances"];
    bool updated = false;
    for (auto& element : arr) {
        if (element.is_object() && stringValue(element, "instance_name") == instanceName) {
            element["cfg_str"] = cfgBase64;
            updated = true;
            break;
        }
    }

    if (!updated) {
        arr.push_back(nlohmann::json{
            {"instance_name", instanceName},
            {"cfg_str", cfgBase64},
        });
    }

    root["instances"] = arr;
    return writeRoot(filePath(), root, error);
}

bool ReconnectStore::removeInstances(const std::vector<std::string>& instanceNames, std::string* error)
{
    std::lock_guard<std::mutex> locker(reconnectStoreMutex());

    if (instanceNames.empty()) return true;

    nlohmann::json root;
    if (!readRoot(filePath(), &root, error)) return false;

    nlohmann::json kept = nlohmann::json::array();
    for (const auto& value : root["instances"]) {
        const bool listed = value.is_object() &&
                            std::find(instanceNames.begin(), instanceNames.end(),
                                      stringValue(value, "instance_name")) != instanceNames.end();
        if (!listed) kept.push_back(value);
    }

    root["instances"] = kept;
    return writeRoot(filePath(), root, error);
}
