/// @file tst_log.cpp
/// @brief log 模块单元测试（三级别分发、setSink 交换与恢复、nullptr 恢复缺省 sink）

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <functional>
#include <string>
#include <vector>

#include "log.h"

namespace {

struct Capture {
    std::vector<std::pair<logging::Level, std::string>> entries;

    std::function<void(logging::Level, const std::string&)> sink()
    {
        return [this](logging::Level level, const std::string& msg) {
            entries.emplace_back(level, msg);
        };
    }
};

} // namespace

TEST_CASE("三级别宏分发：Level 与消息原样送达 sink")
{
    Capture cap;
    const std::function<void(logging::Level, const std::string&)> prev =
        logging::setSink(cap.sink());

    LOG_INFO("hello 信息");
    LOG_WARNING("warn 警告 \xff\xfe bytes");
    LOG_ERROR("error 错误");

    REQUIRE(cap.entries.size() == 3);
    CHECK(cap.entries[0].first == logging::Level::Info);
    CHECK(cap.entries[0].second == "hello 信息");
    CHECK(cap.entries[1].first == logging::Level::Warning);
    CHECK(cap.entries[1].second == "warn 警告 \xff\xfe bytes");
    CHECK(cap.entries[2].first == logging::Level::Error);
    CHECK(cap.entries[2].second == "error 错误");

    logging::setSink(prev); // 恢复，避免污染后续用例
}

TEST_CASE("setSink 返回旧 sink，恢复缺省后写日志不崩溃")
{
    Capture cap;
    const std::function<void(logging::Level, const std::string&)> prev =
        logging::setSink(cap.sink());
    REQUIRE(prev != nullptr); // 此前是缺省 stderr sink，非空

    LOG_INFO("captured");
    REQUIRE(cap.entries.size() == 1);
    CHECK(cap.entries[0].second == "captured");

    // 恢复旧 sink（缺省 stderr）：仅验证不崩溃
    logging::setSink(prev);
    LOG_INFO("this goes to stderr");
    LOG_WARNING("so does this");
    LOG_ERROR("and this");
    CHECK(cap.entries.size() == 1); // 不再进入捕获 sink
}

TEST_CASE("恢复缺省后可再次安装捕获 sink")
{
    Capture first;
    const auto prev = logging::setSink(first.sink());
    LOG_INFO("one");
    logging::setSink(prev);
    CHECK(first.entries.size() == 1);

    Capture second;
    logging::setSink(second.sink());
    LOG_WARNING("two");
    LOG_ERROR("three");
    logging::setSink(nullptr); // nullptr 恢复缺省，同样仅验证不崩溃
    LOG_INFO("stderr again");

    REQUIRE(second.entries.size() == 2);
    CHECK(second.entries[0].first == logging::Level::Warning);
    CHECK(second.entries[0].second == "two");
    CHECK(second.entries[1].first == logging::Level::Error);
    CHECK(second.entries[1].second == "three");
}

TEST_CASE("write 直接调用与空消息")
{
    Capture cap;
    const auto prev = logging::setSink(cap.sink());

    logging::write(logging::Level::Error, "");
    LOG_INFO(std::string("拼接: ") + std::to_string(42));

    REQUIRE(cap.entries.size() == 2);
    CHECK(cap.entries[0].first == logging::Level::Error);
    CHECK(cap.entries[0].second.empty());
    CHECK(cap.entries[1].second == "拼接: 42");

    logging::setSink(prev);
}
