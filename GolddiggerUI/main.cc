/**
  GolddiggerUI - the web cockpit of the Golddigger XAUUSD predictor.

  The server exposes a JSON API (see controllers/GolddiggerController.h) and
  serves the static single page application from the web directory. Candles,
  models, settings and orders are files; there is no database.

  A small background thread runs the auto trader: when the execution mode is
  "auto" and the auto trader is armed it evaluates the configured timeframes
  every `pollSeconds` and places at most one order per candle.

  Usage:
      GolddiggerUI [--port N] [--data DIR] [--models DIR] [--web DIR]
                   [--state DIR] [--routes]

  Every directory can also be set with an environment variable, see
  services/AppPaths.h.
*/

#include <drogon/drogon.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "services/AppPaths.h"
#include "services/ServiceRegistry.h"
#include "services/Settings.h"

namespace
{
  namespace fs = std::filesystem;

  std::string contentTypeFor(const fs::path &path)
  {
    const std::string extension = path.extension().string();
    if (extension == ".html" || extension == ".htm")
      return "text/html; charset=utf-8";
    if (extension == ".css")
      return "text/css; charset=utf-8";
    if (extension == ".js")
      return "application/javascript; charset=utf-8";
    if (extension == ".json")
      return "application/json; charset=utf-8";
    if (extension == ".svg")
      return "image/svg+xml";
    if (extension == ".png")
      return "image/png";
    if (extension == ".jpg" || extension == ".jpeg")
      return "image/jpeg";
    if (extension == ".ico")
      return "image/x-icon";
    if (extension == ".woff2")
      return "font/woff2";
    if (extension == ".txt")
      return "text/plain; charset=utf-8";
    return "application/octet-stream";
  }

  /// Serve a file below `webDir`. Paths that try to escape the directory are
  /// rejected.
  drogon::HttpResponsePtr serveFile(const std::string &webDir, const std::string &relative)
  {
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(fs::path(webDir), ec);
    if (ec)
      return nullptr;

    // Resolve symlinks and `..` before the containment check. A plain string
    // prefix test would accept a sibling such as "/srv/web2" for "/srv/web",
    // hence the explicit path component comparison below.
    const fs::path target = fs::weakly_canonical(base / relative, ec);
    if (ec || target.empty())
      return nullptr;

    auto baseIt = base.begin();
    auto targetIt = target.begin();
    for (; baseIt != base.end(); ++baseIt, ++targetIt)
    {
      if (targetIt == target.end() || *targetIt != *baseIt)
        return nullptr;
    }

    if (!fs::is_regular_file(target, ec))
      return nullptr;

    std::ifstream in(target, std::ios::binary);
    if (!in)
      return nullptr;
    std::ostringstream buffer;
    buffer << in.rdbuf();

    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeString(contentTypeFor(target));
    response->setBody(buffer.str());
    response->addHeader("Cache-Control", "no-cache");
    return response;
  }

  drogon::HttpResponsePtr notFound()
  {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k404NotFound);
    response->setContentTypeString("text/plain; charset=utf-8");
    response->setBody("not found\n");
    return response;
  }
} // namespace

int main(int argc, char *argv[])
{
  golddiggerui::initAppPaths(argc > 0 ? argv[0] : "");

  int port = 5556;
  if (const char *env = std::getenv("GOLDDIGGER_UI_PORT"))
    port = std::atoi(env);

  bool showRoutes = std::getenv("GOLDDIGGER_UI_SHOW_ROUTES") != nullptr;

  golddiggerui::AppPaths paths = golddiggerui::appPaths();

  for (int i = 1; i < argc; ++i)
  {
    const std::string argument = argv[i];
    const bool hasValue = (i + 1) < argc;
    if (argument == "--port" || argument == "-p")
    {
      if (hasValue)
        port = std::atoi(argv[++i]);
    }
    else if (argument == "--data")
    {
      if (hasValue)
        paths.dataDir = argv[++i];
    }
    else if (argument == "--models")
    {
      if (hasValue)
        paths.modelsDir = argv[++i];
    }
    else if (argument == "--web")
    {
      if (hasValue)
        paths.webDir = argv[++i];
    }
    else if (argument == "--state")
    {
      if (hasValue)
        paths.stateDir = argv[++i];
    }
    else if (argument == "--routes")
    {
      showRoutes = true;
    }
    else if (argument == "--help" || argument == "-h")
    {
      std::cout << "GolddiggerUI [--port N] [--data DIR] [--models DIR] [--web DIR]\n"
                << "               [--state DIR] [--routes]\n"
                << "  --port    port to listen on (default 5556, env GOLDDIGGER_UI_PORT)\n"
                << "  --data    directory with the candle csv files (env GOLDDIGGER_DATA_DIR)\n"
                << "  --models  directory with the trained .dat models (env GOLDDIGGER_MODELS_DIR)\n"
                << "  --web     directory with the static front end (env GOLDDIGGER_UI_WEB_DIR)\n"
                << "  --state   directory for settings and orders (env GOLDDIGGER_UI_STATE_DIR)\n"
                << "  --routes  print the registered routes on start up\n";
      return 0;
    }
  }

  paths.settingsFile = (fs::path(paths.stateDir) / "settings.json").string();
  paths.ordersFile = (fs::path(paths.stateDir) / "orders.csv").string();
  golddiggerui::setAppPaths(paths);
  golddiggerui::ensureStateDir(paths);

  if (!fs::is_directory(paths.webDir))
  {
    std::cerr << "GolddiggerUI: the web directory '" << paths.webDir << "' does not exist\n";
    return 1;
  }

  golddiggerui::initServices(paths);

  std::cout << "GolddiggerUI listening on http://0.0.0.0:" << port << "\n"
            << "  data directory  : " << paths.dataDir << "\n"
            << "  models directory: " << paths.modelsDir << "\n"
            << "  state directory : " << paths.stateDir << "\n"
            << "  web directory   : " << paths.webDir << std::endl;

  auto &app = drogon::app();
  app.addListener("0.0.0.0", port);
  app.setDocumentRoot(paths.webDir);
  app.setThreadNum(4);

  // The single page application.
  app.registerHandler(
      "/",
      [webDir = paths.webDir](const drogon::HttpRequestPtr & /*req*/,
                              std::function<void(const drogon::HttpResponsePtr &)> &&callback)
      {
        auto response = serveFile(webDir, "index.html");
        callback(response ? response : notFound());
      },
      {drogon::Get});

  // Its assets.
  app.registerHandler(
      "/static/{1}",
      [webDir = paths.webDir](const drogon::HttpRequestPtr & /*req*/,
                              std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                              const std::string &name)
      {
        auto response = serveFile(webDir, "static/" + name);
        callback(response ? response : notFound());
      },
      {drogon::Get});

  if (showRoutes)
  {
    app.registerBeginningAdvice([]()
                                {
      std::cout << "--- registered routes ---" << std::endl;
      for (const auto &info : drogon::app().getHandlersInfo())
      {
        std::cout << "  " << std::get<0>(info)
                  << "  method=" << static_cast<int>(std::get<1>(info))
                  << "  " << std::get<2>(info) << std::endl;
      }
      std::cout << "--------------------------" << std::endl; });
  }

  // The server side auto trader. It sleeps in short slices so shutdown stays
  // responsive, and reads the poll interval from the settings every cycle.
  std::atomic<bool> running{true};
  std::thread autoTrader([&running]()
                         {
    auto nextRun = std::chrono::steady_clock::now();
    while (running.load())
    {
      const auto now = std::chrono::steady_clock::now();
      if (now >= nextRun)
      {
        int pollSeconds = 15;
        try
        {
          pollSeconds = golddiggerui::services().settings->get().pollSeconds;
          golddiggerui::services().trading->runAutoCycle();
        }
        catch (const std::exception &ex)
        {
          std::cerr << "GolddiggerUI auto trader: " << ex.what() << '\n';
        }
        nextRun = now + std::chrono::seconds(std::max(5, pollSeconds));
      }
      for (int slice = 0; slice < 10 && running.load(); ++slice)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } });

  app.run();

  running = false;
  if (autoTrader.joinable())
    autoTrader.join();
  return 0;
}
