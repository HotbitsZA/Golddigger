/**
  Service level tests.

  No HTTP here: these tests exercise the file backed services directly. They
  are the fast feedback loop for the model auto-selection, the candle reader,
  the settings validation and the order log.
*/

#include <drogon/drogon_test.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "services/AppPaths.h"
#include "services/MarketDataService.h"
#include "services/ModelService.h"
#include "services/ServiceRegistry.h"
#include "services/Settings.h"
#include "services/TradingService.h"
#include "test_env.h"

namespace
{
  namespace fs = std::filesystem;

  golddiggerui::AppPaths makePaths(const fs::path &root)
  {
    std::error_code ec;
    fs::create_directories(root / "Data", ec);
    fs::create_directories(root / "Models", ec);
    fs::create_directories(root / "state", ec);
    fs::create_directories(root / "web", ec);
    return golddiggerui::buildAppPaths(root.string(),
                                       (root / "Data").string(),
                                       (root / "Models").string(),
                                       (root / "web").string(),
                                       (root / "state").string());
  }

  void touch(const fs::path &path)
  {
    std::ofstream out(path, std::ios::trunc);
    out << "test";
  }

  const char *candleCsv =
      "timestamp,open,high,low,close,volume\n"
      "1700000000000,2000,2005,1995,2001,10\n"
      "1700000900000,2001,2010,1999,2008,11\n"
      "1700001800000,2008,2015,2003,2004,12\n";
} // namespace

DROGON_TEST(AppPathsDeriveStateFiles)
{
  golddiggerui::test::TempDir dir("golddiggerui-paths");
  const auto paths = makePaths(dir.path());

  CHECK(paths.stateDir == (dir.path() / "state").string());
  CHECK(paths.settingsFile == (dir.path() / "state" / "settings.json").string());
  CHECK(paths.ordersFile == (dir.path() / "state" / "orders.csv").string());
  CHECK(paths.dataDir == (dir.path() / "Data").string());
  CHECK(paths.modelsDir == (dir.path() / "Models").string());
}

DROGON_TEST(ModelResolutionPrefersCanonicalName)
{
  golddiggerui::test::TempDir dir("golddiggerui-models");
  const auto paths = makePaths(dir.path());

  touch(dir.path() / "Models" / "gold_digger_h1.dat");
  touch(dir.path() / "Models" / "xauusd-h1-bid.dat");

  const golddiggerui::ModelService models(paths);
  const auto resolved = models.resolve(CandleTimeframe::H1, "xauusd");

  CHECK(fs::path(resolved).filename() == "xauusd-h1-bid.dat");
  CHECK(models.hasModel(CandleTimeframe::H1, "xauusd"));
}

DROGON_TEST(ModelResolutionFallsBackToNewestTaggedFile)
{
  golddiggerui::test::TempDir dir("golddiggerui-model-fallback");
  const auto paths = makePaths(dir.path());

  const auto older = dir.path() / "Models" / "renamed-m15-old.dat";
  const auto newer = dir.path() / "Models" / "renamed-m15-new.dat";
  touch(older);
  touch(newer);
  fs::last_write_time(older, fs::file_time_type::clock::now() - std::chrono::hours(2));

  const golddiggerui::ModelService models(paths);
  const auto resolved = models.resolve(CandleTimeframe::M15, "xauusd");

  CHECK(fs::path(resolved).filename() == "renamed-m15-new.dat");
}

DROGON_TEST(ModelListSkipsTunerFiles)
{
  golddiggerui::test::TempDir dir("golddiggerui-tuner");
  const auto paths = makePaths(dir.path());

  touch(dir.path() / "Models" / "tuner_xauusd-m15.dat");
  touch(dir.path() / "Models" / "tunner_xauusd-h1.dat");
  touch(dir.path() / "Models" / "xauusd-d1-bid.dat");

  const golddiggerui::ModelService models(paths);
  const auto list = models.list();

  REQUIRE(list.size() == 1);
  CHECK(list.front().name == "xauusd-d1-bid.dat");
  CHECK(list.front().timeframe == "d1");

  // A tuner file must never be selected, even when it is the only .dat file
  // with this timeframe in its name.
  const auto resolved = models.resolve(CandleTimeframe::M15, "xauusd");
  CHECK(fs::path(resolved).filename() == "xauusd-m15-bid.dat");
}

DROGON_TEST(MarketDataReadsNewestCandles)
{
  golddiggerui::test::TempDir dir("golddiggerui-market");
  const auto paths = makePaths(dir.path());
  std::ofstream(dir.path() / "Data" / "xauusd-m15-bid.csv", std::ios::trunc) << candleCsv;

  const golddiggerui::MarketDataService market(paths);
  CHECK(market.hasData(CandleTimeframe::M15, "xauusd"));
  CHECK(market.dataPath(CandleTimeframe::M15, "xauusd") ==
        (dir.path() / "Data" / "xauusd-m15-bid.csv").string());

  const auto all = market.allCandles(CandleTimeframe::M15, "xauusd");
  REQUIRE(all != nullptr);
  CHECK(all->size() == 3);

  const auto recent = market.recentCandles(CandleTimeframe::M15, "xauusd", 2);
  REQUIRE(recent.size() == 2);
  CHECK(recent.front().timestamp == 1700000900000ULL);
  CHECK(recent.back().timestamp == 1700001800000ULL);
  CHECK(std::abs(recent.back().close - 2004.0) < 1e-9);
}

DROGON_TEST(SettingsDefaultsAreSafe)
{
  golddiggerui::test::TempDir dir("golddiggerui-settings");
  const auto paths = makePaths(dir.path());
  golddiggerui::SettingsStore store(paths.settingsFile);

  const auto settings = store.get();
  CHECK(settings.executionMode == "manual");
  CHECK(settings.autoArmed == false);
  CHECK(settings.paperTrading == true);
  CHECK(settings.dataSource == "file");
  CHECK(settings.instrument == "xauusd");
  CHECK(settings.volumeLots > 0.0);
}

DROGON_TEST(SettingsRejectUnsafeLiveSwitch)
{
  golddiggerui::test::TempDir dir("golddiggerui-live");
  const auto paths = makePaths(dir.path());
  golddiggerui::SettingsStore store(paths.settingsFile);

  CHECK_THROWS_AS(store.update((nlohmann::json{{"paperTrading", false}}), false),
                  std::invalid_argument);
  CHECK(store.get().paperTrading == true);

  const auto updated = store.update(nlohmann::json{{"paperTrading", false}}, true);
  CHECK(updated.paperTrading == false);

  // And the live switch has to survive a fresh store.
  golddiggerui::SettingsStore reloaded(paths.settingsFile);
  CHECK(reloaded.get().paperTrading == false);

  // Editing another field while live is already on does not need the
  // confirmation again: there is no new transition from paper to live.
  const auto edited = store.update(nlohmann::json{{"volumeLots", 0.2}}, false);
  CHECK(edited.paperTrading == false);
  CHECK(std::abs(edited.volumeLots - 0.2) < 1e-9);
}

DROGON_TEST(SettingsValidateAndPersist)
{
  golddiggerui::test::TempDir dir("golddiggerui-settings-update");
  const auto paths = makePaths(dir.path());
  golddiggerui::SettingsStore store(paths.settingsFile);

  const auto updated = store.update(nlohmann::json{
                                        {"volumeLots", 0.25},
                                        {"estimatedSpread", 0.55},
                                        {"pollSeconds", 30},
                                        {"autoTimeframes", {"m15", "h1"}},
                                    },
                                    false);
  CHECK(std::abs(updated.volumeLots - 0.25) < 1e-9);
  CHECK(std::abs(updated.estimatedSpread - 0.55) < 1e-9);
  CHECK(updated.pollSeconds == 30);
  REQUIRE(updated.autoTimeframes.size() == 2);

  golddiggerui::SettingsStore reloaded(paths.settingsFile);
  CHECK(std::abs(reloaded.get().volumeLots - 0.25) < 1e-9);
  CHECK(reloaded.get().pollSeconds == 30);

  CHECK_THROWS_AS(store.update((nlohmann::json{{"volumeLots", -1}}), false), std::invalid_argument);
  CHECK_THROWS_AS(store.update((nlohmann::json{{"pollSeconds", 1}}), false), std::invalid_argument);
  CHECK_THROWS_AS(store.update((nlohmann::json{{"executionMode", "sometimes"}}), false),
                  std::invalid_argument);
  CHECK_THROWS_AS(store.update((nlohmann::json{{"autoTimeframes", {"m5"}}}), false),
                  std::invalid_argument);
}

DROGON_TEST(SettingsRedactAlphaVantageKey)
{
  golddiggerui::test::TempDir dir("golddiggerui-secret");
  const auto paths = makePaths(dir.path());
  golddiggerui::SettingsStore store(paths.settingsFile);

  store.update(nlohmann::json{{"alphaVantageApiKey", "topsecret"}}, false);

  const auto settings = store.get();
  const auto json = settings.toJson(false);
  CHECK(json.at("alphaVantageApiKey").get<std::string>().empty());
  CHECK(json.at("alphaVantageApiKeySet").get<bool>() == true);

  // The file itself keeps the key so the provider can use it.
  std::ifstream in(paths.settingsFile);
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  CHECK(content.find("topsecret") != std::string::npos);
}

DROGON_TEST(ModelPredictProducesBuySignal)
{
  golddiggerui::test::resetState();
  auto &registry = golddiggerui::services();

  const auto candles =
      registry.market->recentCandles(CandleTimeframe::M15, "xauusd", 96);
  REQUIRE(candles.size() == 96);

  const auto result =
      registry.models->predict(CandleTimeframe::M15, "xauusd", candles, 0.4);

  REQUIRE(result.available);
  CHECK(result.action == "BUY");
  CHECK(result.change > 0.4);
  CHECK(result.horizonCandles == 6);
  CHECK(result.candlesUsed == 96);
  CHECK(std::isfinite(result.targetPrice));
}

DROGON_TEST(TradingRejectsBadOrderInput)
{
  golddiggerui::test::resetState();
  auto &trading = *golddiggerui::services().trading;

  CHECK_THROWS_AS(trading.placeOrder(CandleTimeframe::M15, "HOLD", 0.1, 2000.0,
                                     0.0, 0.0, false, nullptr, "test"),
                  std::invalid_argument);
  CHECK_THROWS_AS(trading.placeOrder(CandleTimeframe::M15, "BUY", -1.0, 2000.0,
                                     0.0, 0.0, false, nullptr, "test"),
                  std::invalid_argument);
  CHECK_THROWS_AS(trading.placeOrder(CandleTimeframe::M15, "BUY", 0.1, 2000.0,
                                     -1.0, 0.0, false, nullptr, "test"),
                  std::invalid_argument);
}

DROGON_TEST(TradingLogsPaperOrder)
{
  golddiggerui::test::resetState();
  auto &trading = *golddiggerui::services().trading;

  const auto order = trading.placeOrder(CandleTimeframe::M15, "buy", 0.25, 2100.5,
                                        0.0, 0.0, false, nullptr, "unit test order");
  CHECK(order.status == "paper");
  CHECK(order.side == "BUY");
  CHECK(std::abs(order.volumeLots - 0.25) < 1e-9);
  CHECK(std::abs(order.referencePrice - 2100.5) < 1e-9);
  CHECK(order.automatic == false);

  const auto orders = trading.orders(10);
  REQUIRE(orders.size() == 1);
  CHECK(orders.front().message.find("unit test order") != std::string::npos);

  std::ifstream in(golddiggerui::services().paths().ordersFile);
  std::string header;
  std::getline(in, header);
  CHECK(header.rfind("timestamp_ms,time_utc,automatic", 0) == 0);
  std::string row;
  std::getline(in, row);
  CHECK(row.find(",paper,") != std::string::npos);
}
