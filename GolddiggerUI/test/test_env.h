#pragma once

/**
  Shared helpers for the GolddiggerUI tests.

  The tests talk to a real drogon server that this binary starts on a free
  loop back port. Every file the server reads or writes lives in a throw away
  directory, so the real `Data/`, `Models/` and `state/` of the project are
  never touched.

  The candle files and the serialized dlib models are generated at start up:

  * `Data/xauusd-<timeframe>-bid.csv` with 140 synthetic but valid candles,
  * `Models/xauusd-<timeframe>-bid.dat` with a tiny SVR whose prediction is a
    constant +1% move, so the signal is deterministic and actionable.
*/

#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

#include "services/AppPaths.h"

namespace golddiggerui::test
{
  /// Create a unique temporary directory and remove it on destruction.
  class TempDir
  {
  public:
    explicit TempDir(const std::string &prefix);
    ~TempDir();

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    const std::filesystem::path &path() const { return m_path; }

  private:
    std::filesystem::path m_path;
  };

  /// The directories the test server uses.
  struct TestEnvironment
  {
    std::filesystem::path root;
    std::filesystem::path dataDir;
    std::filesystem::path modelsDir;
    std::filesystem::path stateDir;
    std::filesystem::path webDir;
    AppPaths paths;
  };

  /// Create the temporary directories, write the fixtures, initialise the
  /// services and start the server. `root` has to exist already.
  std::uint16_t startTestServer(const std::string &root);

  /// Stop the server again.
  void stopTestServer();

  std::uint16_t serverPort();

  const TestEnvironment &environment();

  /// Remove `state/` (settings and orders), recreate it and rebuild the
  /// services. Call this at the start of a test that changes settings or
  /// places orders.
  void resetState();

  /// Rewrite the candle file of `timeframe` with candles whose newest entry is
  /// `ageIntervals` intervals in the past. `ageIntervals` 1 produces a fresh
  /// file.
  void writeCandles(const std::string &timeframe, int ageIntervals = 1);

  /// Rewrite every candle file with fresh data.
  void writeFreshCandles();

  /// The answer of a request to the test server.
  struct HttpReply
  {
    drogon::ReqResult result = drogon::ReqResult::Ok;
    drogon::HttpStatusCode status = drogon::k200OK;
    std::string body;
    nlohmann::json json;
    bool hasJson = false;
  };

  HttpReply request(drogon::HttpMethod method,
                    const std::string &path,
                    const std::string &body = std::string(),
                    const std::string &contentType = "application/json");

  HttpReply get(const std::string &path);
  HttpReply post(const std::string &path, const std::string &body);
  HttpReply put(const std::string &path, const std::string &body);
} // namespace golddiggerui::test
