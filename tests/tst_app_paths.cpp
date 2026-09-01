/// @file tst_app_paths.cpp
/// @brief 可执行文件目录定位单元测试（非空存在性、包含自身二进制、与 CWD 无关）

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <filesystem>
#include <system_error>

#include "app_paths.h"

TEST_CASE("exeDirectory 返回非空且存在的目录")
{
    const std::filesystem::path dir = app_paths::exeDirectory();
    INFO("exeDirectory() = ", dir.string());
    REQUIRE_FALSE(dir.empty());

    std::error_code ec;
    CHECK(std::filesystem::exists(dir, ec));
    CHECK(std::filesystem::is_directory(dir, ec));
}

TEST_CASE("exeDirectory 目录中包含本测试可执行文件")
{
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::canonical(app_paths::exeDirectory(), ec);
    if (ec)
        FAIL("canonical(exeDirectory()) 失败: ", ec.message());

    const std::filesystem::path exe = std::filesystem::canonical(dir / "tst_app_paths", ec);
    if (ec)
        FAIL("exeDirectory()/tst_app_paths 不存在或无法 canonical: ", ec.message());

    CHECK(std::filesystem::exists(exe, ec));
    CHECK(exe.parent_path() == dir);
}

TEST_CASE("exeDirectory 结果与当前工作目录无关")
{
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path original = fs::current_path(ec);
    REQUIRE_FALSE(ec);

    // RAII 保证测试结束后恢复原 CWD，避免污染同进程内其他用例
    struct RestoreCwd {
        fs::path saved;
        ~RestoreCwd()
        {
            std::error_code e;
            fs::current_path(saved, e);
        }
    } restore{original};

    const fs::path before = app_paths::exeDirectory();

    const fs::path temp = fs::temp_directory_path(ec);
    REQUIRE_FALSE(ec);
    fs::current_path(temp, ec);
    REQUIRE_FALSE(ec);

    const fs::path after = app_paths::exeDirectory();
    CHECK(after == before);
}
