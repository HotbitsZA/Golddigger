#include "test_env.h"

#include <drogon/drogon.h>
#include <dlib/svm.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iomanip>
#include <stdexcept>
#include <thread>
#include <vector>

#include "MarketData/candle_data_provider.h"
#include "Training/model_types.h"
#include "services/ServiceRegistry.h"

namespace golddiggerui::test
{
  namespace fs = std::filesystem;

  namespace
  {
    std::atomic<int> g_dirCounter{0};
    std::uint16_t g_port = 0;
    TestEnvironment g_environment;
    std::thread g_thread;

    std::uint16_t freePort()
    {
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
        return 0;

      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = 0;

      std::uint16_t port = 0;
      if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
      {
        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0)
          port = ntohs(address.sin_port);
      }
      ::close(fd);
      return port;
    }

    bool waitForPort(std::uint16_t port, std::chrono::milliseconds timeout)
    {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline)
      {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
          return false;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);

        const bool connected =
            ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
        ::close(fd);
        if (connected)
          return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return false;
    }

    drogon::HttpClientPtr client()
    {
      static drogon::HttpClientPtr instance =
          drogon::HttpClient::newHttpClient("127.0.0.1", g_port);
      return instance;
    }

    std::chrono::milliseconds intervalOf(const std::string &timeframe)
    {
      CandleTimeframe parsed{};
      if (!parse_timeframe(timeframe, parsed))
        throw std::invalid_argument("unknown timeframe in test fixture: " + timeframe);
      return candle_interval(parsed);
    }

    /// A deterministic 140 candle series that ends `ageIntervals` intervals
    /// before now. The newest candle is complete, so ageIntervals 1 is fresh.
    std::vector<Candle> makeCandles(const std::string &timeframe, int ageIntervals)
    {
      const auto interval = intervalOf(timeframe);
      const auto intervalMs = static_cast<std::uint64_t>(interval.count());
      const auto now = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());

      std::vector<Candle> candles;
      candles.reserve(140);
      const std::uint64_t newest = now - intervalMs * static_cast<std::uint64_t>(ageIntervals);

      for (int i = 0; i < 140; ++i)
      {
        Candle candle;
        candle.timestamp = newest - intervalMs * static_cast<std::uint64_t>(139 - i);
        const double base = 2000.0 + static_cast<double>(i) * 0.1 +
                            2.0 * std::sin(static_cast<double>(i) * 0.3);
        candle.open = base;
        candle.close = base + 0.6 * std::sin(static_cast<double>(i) * 0.7);
        candle.high = std::max(candle.open, candle.close) + 0.5;
        candle.low = std::min(candle.open, candle.close) - 0.5;
        candle.volume = 100.0 + static_cast<double>(i % 17);
        candles.push_back(candle);
      }
      return candles;
    }

    void writeCandlesFile(const std::string &timeframe, int ageIntervals)
    {
      const auto candles = makeCandles(timeframe, ageIntervals);
      const fs::path path = g_environment.dataDir / ("xauusd-" + timeframe + "-bid.csv");

      std::ofstream out(path, std::ios::trunc);
      if (!out)
        throw std::runtime_error("cannot write the test candle file " + path.string());
      out << "timestamp,open,high,low,close,volume\n";
      out << std::setprecision(15);
      for (const auto &candle : candles)
      {
        out << candle.timestamp << ',' << candle.open << ',' << candle.high << ','
            << candle.low << ',' << candle.close << ',' << candle.volume << '\n';
      }
    }

    /// Serialize a tiny SVR that predicts a constant +1% log return. That makes
    /// the action deterministic: the target price is above the close by more
    /// than the default 0.40 spread, so the signal is BUY.
    void writeModelFile(const fs::path &path)
    {
      std::vector<sample_type> samples;
      std::vector<double> labels;
      samples.reserve(40);
      labels.reserve(40);
      for (int i = 0; i < 40; ++i)
      {
        sample_type sample;
        for (long j = 0; j < sample.size(); ++j)
          sample(j) = std::sin(0.17 * static_cast<double>(i) + 0.41 * static_cast<double>(j));
        samples.push_back(sample);
        labels.push_back(0.01 + 0.0001 * std::sin(static_cast<double>(i)));
      }

      dlib::vector_normalizer<sample_type> normalizer;
      normalizer.train(samples);

      dlib::svr_trainer<kernel_type> trainer;
      trainer.set_kernel(kernel_type(0.2));
      trainer.set_c(10.0);
      trainer.set_epsilon(0.001);
      trainer.set_epsilon_insensitivity(0.001);
      const auto decisionFunction = trainer.train(samples, labels);

      dlib::serialize(path.string()) << decisionFunction << normalizer;
    }

    void writeFixtures()
    {
      for (const char *timeframe : {"m15", "h1", "d1"})
      {
        writeCandlesFile(timeframe, 1);
        writeModelFile(g_environment.modelsDir / (std::string("xauusd-") + timeframe + "-bid.dat"));
      }
    }
  } // namespace

  TempDir::TempDir(const std::string &prefix)
  {
    const int index = ++g_dirCounter;
    m_path = fs::temp_directory_path() /
             (prefix + "-" + std::to_string(::getpid()) + "-" + std::to_string(index));
    std::error_code ec;
    fs::remove_all(m_path, ec);
    fs::create_directories(m_path, ec);
  }

  TempDir::~TempDir()
  {
    std::error_code ec;
    fs::remove_all(m_path, ec);
  }

  std::uint16_t startTestServer(const std::string &root)
  {
    g_environment.root = root;
    g_environment.dataDir = fs::path(root) / "Data";
    g_environment.modelsDir = fs::path(root) / "Models";
    g_environment.stateDir = fs::path(root) / "state";
    g_environment.webDir = fs::path(root) / "web";

    std::error_code ec;
    fs::create_directories(g_environment.dataDir, ec);
    fs::create_directories(g_environment.modelsDir, ec);
    fs::create_directories(g_environment.stateDir, ec);
    fs::create_directories(g_environment.webDir, ec);
    if (ec)
      throw std::runtime_error("cannot create the test directories under " + root);

    // Do not let the developer's environment leak into the defaults.
    ::unsetenv("ALPHAVANTAGE_API_KEY");
    ::unsetenv("GOLDDIGGER_INSTRUMENT");
    ::unsetenv("GOLDDIGGER_MT5_ENDPOINT");
    ::unsetenv("GOLDDIGGER_MT5_CLIENT_ID");
    ::unsetenv("GOLDDIGGER_DATA_DIR");
    ::unsetenv("GOLDDIGGER_MODELS_DIR");
    ::unsetenv("GOLDDIGGER_UI_STATE_DIR");
    ::unsetenv("GOLDDIGGER_UI_WEB_DIR");

    writeFixtures();

    g_environment.paths = buildAppPaths(root,
                                        g_environment.dataDir.string(),
                                        g_environment.modelsDir.string(),
                                        g_environment.webDir.string(),
                                        g_environment.stateDir.string());
    setAppPaths(g_environment.paths);
    ensureStateDir(g_environment.paths);
    initServices(g_environment.paths);

    g_port = freePort();
    if (g_port == 0)
      throw std::runtime_error("cannot find a free port for the test server");

    auto &app = drogon::app();
    app.addListener("127.0.0.1", g_port);
    app.setThreadNum(2);

    auto ready = std::make_shared<std::promise<void>>();
    auto future = ready->get_future();
    g_thread = std::thread([ready]()
                           {
      drogon::app().getLoop()->queueInLoop([ready]() { ready->set_value(); });
      drogon::app().run(); });
    future.get();

    if (!waitForPort(g_port, std::chrono::seconds(5)))
    {
      stopTestServer();
      throw std::runtime_error("the test server did not start listening");
    }
    return g_port;
  }

  void stopTestServer()
  {
    if (drogon::app().getLoop() != nullptr)
      drogon::app().getLoop()->queueInLoop([]() { drogon::app().quit(); });
    if (g_thread.joinable())
      g_thread.join();
  }

  std::uint16_t serverPort()
  {
    return g_port;
  }

  const TestEnvironment &environment()
  {
    return g_environment;
  }

  void resetState()
  {
    std::error_code ec;
    fs::remove_all(g_environment.stateDir, ec);
    fs::create_directories(g_environment.stateDir, ec);
    ensureStateDir(g_environment.paths);
    initServices(g_environment.paths);
  }

  void writeCandles(const std::string &timeframe, int ageIntervals)
  {
    // One extra row changes the file size, which busts the mtime/size cache of
    // MarketDataService even on a file system with a coarse timestamp.
    if (ageIntervals <= 0)
      ageIntervals = 1;
    const auto path = g_environment.dataDir / ("xauusd-" + timeframe + "-bid.csv");
    const auto candles = makeCandles(timeframe, ageIntervals);

    std::ofstream out(path, std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot rewrite the test candle file " + path.string());
    out << "timestamp,open,high,low,close,volume\n";
    out << std::setprecision(15);
    for (const auto &candle : candles)
    {
      out << candle.timestamp << ',' << candle.open << ',' << candle.high << ','
          << candle.low << ',' << candle.close << ',' << candle.volume << '\n';
    }
    // A comment row is not valid candle data and is skipped by the reader. It
    // only exists to guarantee a different file size than the previous write.
    out << "# rewritten by the test fixture\n";
  }

  void writeFreshCandles()
  {
    for (const char *timeframe : {"m15", "h1", "d1"})
    {
      const fs::path path = g_environment.dataDir / (std::string("xauusd-") + timeframe + "-bid.csv");
      const auto candles = makeCandles(timeframe, 1);
      std::ofstream out(path, std::ios::trunc);
      out << "timestamp,open,high,low,close,volume\n";
      out << std::setprecision(15);
      for (const auto &candle : candles)
      {
        out << candle.timestamp << ',' << candle.open << ',' << candle.high << ','
            << candle.low << ',' << candle.close << ',' << candle.volume << '\n';
      }
    }
  }

  HttpReply request(drogon::HttpMethod method,
                    const std::string &path,
                    const std::string &body,
                    const std::string &contentType)
  {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(path);
    if (!body.empty())
    {
      req->setContentTypeString(contentType);
      req->setBody(body);
    }

    HttpReply reply;
    const auto answer = client()->sendRequest(req, 10.0);
    reply.result = answer.first;
    if (answer.second)
    {
      reply.status = answer.second->statusCode();
      reply.body = std::string(answer.second->getBody());
      if (!reply.body.empty())
      {
        try
        {
          reply.json = nlohmann::json::parse(reply.body);
          reply.hasJson = true;
        }
        catch (const std::exception &)
        {
          reply.hasJson = false;
        }
      }
    }
    return reply;
  }

  HttpReply get(const std::string &path)
  {
    return request(drogon::Get, path);
  }

  HttpReply post(const std::string &path, const std::string &body)
  {
    return request(drogon::Post, path, body);
  }

  HttpReply put(const std::string &path, const std::string &body)
  {
    return request(drogon::Put, path, body);
  }
} // namespace golddiggerui::test
