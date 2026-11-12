#define DROGON_TEST_MAIN
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>

#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "test_env.h"

DROGON_TEST(BasicTest)
{
  // The server is started by main() before the tests run; there is nothing to
  // do here. The test only proves that the harness itself is wired up.
  CHECK(true);
}

int main(int argc, char **argv)
{
  using namespace drogon;

  // Everything happens in a throw away directory. The real Data/, Models/ and
  // state/ of the project are never read or written by the tests.
  const auto root = std::filesystem::temp_directory_path() /
                    ("golddiggerui-apitest-" + std::to_string(::getpid()));

  const int status = [&]()
  {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    try
    {
      golddiggerui::test::startTestServer(root.string());
    }
    catch (const std::exception &ex)
    {
      std::cerr << "GolddiggerUI test server failed to start: " << ex.what() << '\n';
      return 1;
    }

    const int result = test::run(argc, argv);

    golddiggerui::test::stopTestServer();
    std::filesystem::remove_all(root, ec);
    return result;
  }();

  return status;
}
