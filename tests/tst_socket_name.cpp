/// @file tst_socket_name.cpp
/// @brief 监听 socket 名解析优先级单元测试（命令行 > 环境变量 > 平台默认）

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>

#include "daemon/SocketName.h"

TEST_CASE("resolveSocketName：--socket 取值优先于环境变量与默认")
{
    CHECK(socket_name::resolveSocketName("/run/from-arg.sock", "/env/from-env.sock",
                                    "qtet-daemon.sock")
          == "/run/from-arg.sock");
}

TEST_CASE("resolveSocketName：无 --socket 时取环境变量")
{
    CHECK(socket_name::resolveSocketName("", "/env/from-env.sock", "qtet-daemon.sock")
          == "/env/from-env.sock");
}

TEST_CASE("resolveSocketName：都未提供时取平台默认")
{
    CHECK(socket_name::resolveSocketName("", "", "/var/run/qtet-daemon.sock")
          == "/var/run/qtet-daemon.sock");
}

TEST_CASE("resolveSocketName：空串视为未提供（--socket= 空值不覆盖默认）")
{
    CHECK(socket_name::resolveSocketName("", "", "qtet-daemon.sock") == "qtet-daemon.sock");
}
