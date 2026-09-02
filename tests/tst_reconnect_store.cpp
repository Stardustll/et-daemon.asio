/// @file tst_reconnect_store.cpp
/// @brief ReconnectStore 自动回连配置持久化测试（移植父工程 Qt 版用例 + 新增原子写/路径用例）

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "daemon/ReconnectStore.h"
#include "app_paths.h"

namespace {

/// 每个用例独立的临时基准目录：
/// <temp>/qtet-reconnect-<runId>-<counter>/，析构时递归清理
class TempBaseDir
{
public:
    TempBaseDir()
    {
        static const auto runId =
            std::chrono::steady_clock::now().time_since_epoch().count();
        static int counter = 0;

        std::error_code ec;
        m_dir = std::filesystem::temp_directory_path(ec) /
                ("qtet-reconnect-" + std::to_string(runId) + "-" +
                 std::to_string(++counter));
        std::filesystem::create_directories(m_dir, ec);
    }

    ~TempBaseDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    TempBaseDir(const TempBaseDir&) = delete;
    TempBaseDir& operator=(const TempBaseDir&) = delete;

    const std::filesystem::path& path() const { return m_dir; }

private:
    std::filesystem::path m_dir;
};

/// 读取整个文件内容（二进制）
std::string readFile(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// 直接向磁盘写入原始字节（构造损坏文件等场景）
void writeFile(const std::filesystem::path& file, const std::string& content)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << content;
}

} // namespace

TEST_CASE("filePath 为 baseDir/<socketName>.json")
{
    TempBaseDir dir;
    ReconnectStore store("sock-path", dir.path());
    CHECK(store.filePath() == dir.path() / "sock-path.json");

    // baseDir 缺省 → exeDirectory()（只验证路径计算，不落盘）
    ReconnectStore defaultBase("sock-path");
    CHECK(defaultBase.filePath() == app_paths::exeDirectory() / "sock-path.json");
}

TEST_CASE("文件缺失时返回缺省值：autoReconnect 为 false，instances 为空")
{
    TempBaseDir dir;
    ReconnectStore store("sock-default", dir.path());

    std::string error;
    bool enabled = true;
    REQUIRE(store.autoReconnect(&enabled, &error));
    CHECK(error.empty());
    CHECK_FALSE(enabled);

    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances, &error));
    CHECK(error.empty());
    CHECK(instances.empty());
}

TEST_CASE("setAutoReconnect 落盘并可回读切换")
{
    TempBaseDir dir;
    ReconnectStore store("sock-switch", dir.path());

    std::string error;
    REQUIRE(store.setAutoReconnect(true, &error));
    CHECK(error.empty());
    CHECK(std::filesystem::exists(store.filePath()));

    bool enabled = false;
    REQUIRE(store.autoReconnect(&enabled, &error));
    CHECK(enabled);

    REQUIRE(store.setAutoReconnect(false, &error));
    REQUIRE(store.autoReconnect(&enabled, &error));
    CHECK_FALSE(enabled);
}

TEST_CASE("saveInstance 新增后出现在 loadInstances，重复读取缓存原样保留")
{
    TempBaseDir dir;
    ReconnectStore store("sock-add", dir.path());

    std::string error;
    REQUIRE(store.saveInstance("net-a", "Y2ZnMQ==", &error));

    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances, &error));
    REQUIRE(instances.size() == 1);
    CHECK(instances[0].instanceName == "net-a");
    CHECK(instances[0].cfgBase64 == "Y2ZnMQ==");

    // 读取用于回连后缓存必须原样保留，供下次启动重试（对齐父工程用例）
    std::vector<ReconnectStore::StoredInstance> again;
    REQUIRE(store.loadInstances(&again, &error));
    CHECK(again.size() == 1);
    CHECK(again[0].instanceName == "net-a");
}

TEST_CASE("saveInstance 同名不同 cfg 执行 upsert")
{
    TempBaseDir dir;
    ReconnectStore store("sock-upsert", dir.path());

    std::string error;
    REQUIRE(store.saveInstance("net-a", "Y2ZnMQ==", &error));
    REQUIRE(store.saveInstance("net-a", "Y2ZnMg==", &error));

    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances, &error));
    REQUIRE(instances.size() == 1);
    CHECK(instances[0].cfgBase64 == "Y2ZnMg==");
}

TEST_CASE("saveInstance 第二个实例名后共 2 条记录")
{
    TempBaseDir dir;
    ReconnectStore store("sock-two", dir.path());

    std::string error;
    REQUIRE(store.saveInstance("net-a", "YQ==", &error));
    REQUIRE(store.saveInstance("net-b", "Yg==", &error));

    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances, &error));
    REQUIRE(instances.size() == 2);
}

TEST_CASE("removeInstances 按名移除，空名单为 no-op")
{
    TempBaseDir dir;
    ReconnectStore store("sock-remove", dir.path());

    std::string error;
    REQUIRE(store.saveInstance("net-a", "YQ==", &error));
    REQUIRE(store.saveInstance("net-b", "Yg==", &error));

    std::vector<std::string> emptyList;
    REQUIRE(store.removeInstances(emptyList, &error));
    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances, &error));
    CHECK(instances.size() == 2);

    REQUIRE(store.removeInstances({"net-a"}, &error));
    REQUIRE(store.loadInstances(&instances, &error));
    REQUIRE(instances.size() == 1);
    CHECK(instances[0].instanceName == "net-b");
}

TEST_CASE("saveInstance 空实例名为 no-op，不落盘")
{
    TempBaseDir dir;
    ReconnectStore store("sock-empty-name", dir.path());

    std::string error;
    REQUIRE(store.saveInstance("", "YQ==", &error));
    CHECK(error.empty());
    CHECK_FALSE(std::filesystem::exists(store.filePath()));

    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances, &error));
    CHECK(instances.empty());
}

TEST_CASE("损坏文件导致读取失败并报 invalid reconnect config json")
{
    TempBaseDir dir;
    ReconnectStore store("sock-corrupt", dir.path());

    writeFile(store.filePath(), "not-json-garbage{{{");

    std::string error;
    bool enabled = false;
    CHECK_FALSE(store.autoReconnect(&enabled, &error));
    CHECK(error.find("invalid reconnect config json") != std::string::npos);

    error.clear();
    std::vector<ReconnectStore::StoredInstance> instances;
    CHECK_FALSE(store.loadInstances(&instances, &error));
    CHECK(error.find("invalid reconnect config json") != std::string::npos);
}

TEST_CASE("读入后补缺字段：缺 instances 视为空，缺 autoReconnect 视为 false")
{
    TempBaseDir dir;
    ReconnectStore store("sock-partial", dir.path());

    writeFile(store.filePath(), "{\"autoReconnect\": true}");

    std::string error;
    bool enabled = false;
    REQUIRE(store.autoReconnect(&enabled, &error));
    CHECK(enabled);

    writeFile(store.filePath(), "{}");
    REQUIRE(store.autoReconnect(&enabled, &error));
    CHECK_FALSE(enabled);

    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(store.loadInstances(&instances, &error));
    CHECK(instances.empty());
}

TEST_CASE("成功保存后 .tmp 临时文件不残留")
{
    TempBaseDir dir;
    ReconnectStore store("sock-tmp", dir.path());

    std::string error;
    REQUIRE(store.saveInstance("net-a", "YQ==", &error));
    REQUIRE(std::filesystem::exists(store.filePath()));
    CHECK_FALSE(std::filesystem::exists(store.filePath().string() + ".tmp"));

    REQUIRE(store.setAutoReconnect(true, &error));
    CHECK_FALSE(std::filesystem::exists(store.filePath().string() + ".tmp"));
}

TEST_CASE("保存失败（写入中途失败）后 .tmp 临时文件被清理")
{
#ifdef _WIN32
    // /dev/full 注入为 POSIX 专属：Windows 无该设备且 create_symlink 需要特权
    // 注意：doctest 无 SUCCEED 宏（Catch2 专属），等价用法为 MESSAGE（仅记录不判定）
    MESSAGE("/dev/full injection is POSIX-only, skipped on Windows");
    return;
#endif

    // 注入方式说明：评审建议的“最终路径预置为非空目录 → rename 失败”在本平台
    // 不可行——libstdc++ 的 ifstream 读目录会抛 underflow 异常，saveInstance 在
    // readRoot 阶段即抛出，到不了 rename。改为把 .tmp 预置为指向 /dev/full 的
    // 符号链接：ofstream 打开成功（tmp 条目已创建），write/flush 恒返回 ENOSPC，
    // 确定性走 writeRoot 写入中途失败分支。
    TempBaseDir dir;
    ReconnectStore store("sock-fail-write", dir.path());

    const std::filesystem::path tmp = store.filePath().string() + ".tmp";
    std::error_code ec;
    std::filesystem::create_symlink("/dev/full", tmp, ec);
    REQUIRE(std::filesystem::exists(tmp));

    std::string error;
    CHECK_FALSE(store.saveInstance("net-a", "YQ==", &error));
    CHECK(error.find("failed to write reconnect config") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(tmp));

    // 清理失败注入 fixture 后同一 store 保存恢复正常
    REQUIRE(store.saveInstance("net-a", "YQ==", &error));
    REQUIRE(std::filesystem::exists(store.filePath()));
    CHECK_FALSE(std::filesystem::exists(tmp));
}

TEST_CASE("落盘为 dump(4) 缩进格式且无末尾换行，字段名对齐 Qt 版")
{
    TempBaseDir dir;
    ReconnectStore store("sock-format", dir.path());

    std::string error;
    REQUIRE(store.saveInstance("net-a", "Y2ZnMg==", &error));
    REQUIRE(store.setAutoReconnect(true, &error));

    const std::string content = readFile(store.filePath());
    REQUIRE_FALSE(content.empty());
    CHECK(content.back() != '\n');

    const nlohmann::json root = nlohmann::json::parse(content);
    CHECK(root.at("autoReconnect") == true);
    REQUIRE(root.at("instances").size() == 1);
    CHECK(root.at("instances").at(0).at("instance_name") == "net-a");
    CHECK(root.at("instances").at(0).at("cfg_str") == "Y2ZnMg==");

    // 内容与 dump(4) 完全一致 → 4 空格缩进、无末尾换行
    CHECK(content == root.dump(4));
}

TEST_CASE("跨 ReconnectStore 对象持久化：写入者与读取者各自独立构造")
{
    TempBaseDir dir;

    std::string error;
    {
        ReconnectStore writer("sock-across", dir.path());
        REQUIRE(writer.setAutoReconnect(true, &error));
        REQUIRE(writer.saveInstance("net-a", "YQ==", &error));
    }

    ReconnectStore reader("sock-across", dir.path());
    bool enabled = false;
    REQUIRE(reader.autoReconnect(&enabled, &error));
    CHECK(enabled);

    std::vector<ReconnectStore::StoredInstance> instances;
    REQUIRE(reader.loadInstances(&instances, &error));
    REQUIRE(instances.size() == 1);
    CHECK(instances[0].instanceName == "net-a");
    CHECK(instances[0].cfgBase64 == "YQ==");
}

TEST_CASE("空指针输出参数被拒绝并返回错误信息")
{
    TempBaseDir dir;
    ReconnectStore store("sock-null", dir.path());

    std::string error;
    CHECK_FALSE(store.autoReconnect(nullptr));
    CHECK_FALSE(store.autoReconnect(nullptr, &error));
    CHECK(error == "enabled output is null");

    error.clear();
    CHECK_FALSE(store.loadInstances(nullptr));
    CHECK_FALSE(store.loadInstances(nullptr, &error));
    CHECK(error == "instances output is null");
}
