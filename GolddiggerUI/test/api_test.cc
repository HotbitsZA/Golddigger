/**
  HTTP level tests.

  They talk to the real drogon server the test binary starts. These are the
  guard against the silent failure mode of the project: when the drogon
  headers and library do not match, every route is dropped and the front end
  receives 404 instead of JSON.

  Every mutating test starts with resetState() so settings and orders do not
  leak between tests.
*/

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>

#include "test_env.h"

namespace
{
  namespace fs = std::filesystem;

  bool hasRoute(const std::string &path)
  {
    for (const auto &route : drogon::app().getHandlersInfo())
    {
      if (std::get<0>(route) == path)
        return true;
    }
    return false;
  }
} // namespace

DROGON_TEST(ApiRoutesAreRegistered)
{
  CHECK(hasRoute("/api/status"));
  CHECK(hasRoute("/api/timeframes"));
  CHECK(hasRoute("/api/models"));
  CHECK(hasRoute("/api/candles"));
  CHECK(hasRoute("/api/candles/refresh"));
  CHECK(hasRoute("/api/signal"));
  CHECK(hasRoute("/api/signal/evaluate"));
  CHECK(hasRoute("/api/orders"));
  CHECK(hasRoute("/api/settings"));
  CHECK(hasRoute("/api/provider/test"));
  CHECK(drogon::app().getHandlersInfo().size() >= 13);
}

DROGON_TEST(ApiStatusIsSafeByDefault)
{
  golddiggerui::test::resetState();

  const auto reply = golddiggerui::test::get("/api/status");
  CHECK(reply.result == drogon::ReqResult::Ok);
  CHECK(reply.status == drogon::k200OK);
  REQUIRE(reply.hasJson);

  CHECK(reply.json.at("app").get<std::string>() == "GolddiggerUI");
  CHECK(reply.json.at("instrument").get<std::string>() == "xauusd");
  CHECK(reply.json.at("modelCount").get<std::size_t>() == 3);
  REQUIRE(reply.json.at("timeframes").size() == 3);

  const auto settings = reply.json.at("settings");
  CHECK(settings.at("executionMode").get<std::string>() == "manual");
  CHECK(settings.at("autoArmed").get<bool>() == false);
  CHECK(settings.at("paperTrading").get<bool>() == true);
  CHECK(settings.at("dataSource").get<std::string>() == "file");

  const auto paths = reply.json.at("paths");
  CHECK(paths.at("stateDir").get<std::string>() ==
        golddiggerui::test::environment().stateDir.string());
  CHECK(paths.at("ordersFile").get<std::string>() ==
        (golddiggerui::test::environment().stateDir / "orders.csv").string());

  CHECK(reply.json.at("providers").at("mt5").at("ready").get<bool>() == true);
}

DROGON_TEST(ApiTimeframesSelectTheMatchingModel)
{
  golddiggerui::test::writeFreshCandles();
  golddiggerui::test::resetState();

  const auto reply = golddiggerui::test::get("/api/timeframes");
  CHECK(reply.status == drogon::k200OK);
  REQUIRE(reply.hasJson);
  CHECK(reply.json.at("instrument").get<std::string>() == "xauusd");

  const auto &timeframes = reply.json.at("timeframes");
  REQUIRE(timeframes.size() == 3);

  const std::string expected[] = {"m15", "h1", "d1"};
  for (std::size_t index = 0; index < timeframes.size(); ++index)
  {
    const auto &entry = timeframes.at(index);
    const std::string id = expected[index];
    CHECK(entry.at("id").get<std::string>() == id);
    CHECK(entry.at("model").at("exists").get<bool>() == true);
    CHECK(entry.at("model").at("name").get<std::string>() == "xauusd-" + id + "-bid.dat");
    CHECK(entry.at("data").at("exists").get<bool>() == true);
    CHECK(entry.at("data").at("stale").get<bool>() == false);
    CHECK(entry.at("data").at("candles").get<std::size_t>() == 140);
  }
}

DROGON_TEST(ApiModelsListsAndSelects)
{
  golddiggerui::test::resetState();

  const auto reply = golddiggerui::test::get("/api/models");
  CHECK(reply.status == drogon::k200OK);
  REQUIRE(reply.hasJson);

  CHECK(reply.json.at("models").size() == 3);
  CHECK(reply.json.at("modelsDir").get<std::string>() ==
        golddiggerui::test::environment().modelsDir.string());

  for (const char *timeframe : {"m15", "h1", "d1"})
  {
    const auto selected =
        reply.json.at("selected").at(timeframe).get<std::string>();
    CHECK(fs::path(selected).filename() ==
          std::string("xauusd-") + timeframe + "-bid.dat");
  }
}

DROGON_TEST(ApiCandlesReturnsTheLastFive)
{
  golddiggerui::test::writeFreshCandles();
  golddiggerui::test::resetState();

  const auto reply = golddiggerui::test::get("/api/candles?timeframe=m15&count=5&source=file");
  CHECK(reply.status == drogon::k200OK);
  REQUIRE(reply.hasJson);

  CHECK(reply.json.at("timeframe").get<std::string>() == "m15");
  CHECK(reply.json.at("source").get<std::string>() == "file");
  CHECK(reply.json.at("warning").get<std::string>().empty());

  const auto &candles = reply.json.at("candles");
  REQUIRE(candles.size() == 5);
  CHECK(reply.json.at("count").get<std::size_t>() == 5);

  std::uint64_t previous = 0;
  for (const auto &candle : candles)
  {
    CHECK(candle.at("timestamp").get<std::uint64_t>() > previous);
    previous = candle.at("timestamp").get<std::uint64_t>();
    CHECK(candle.at("close").get<double>() > 0.0);
    CHECK(candle.at("local").get<std::string>().size() >= 16);
    CHECK(candle.at("utc").get<std::string>().back() == 'Z');
  }
}

DROGON_TEST(ApiCandlesRejectsInvalidArguments)
{
  const auto badTimeframe =
      golddiggerui::test::get("/api/candles?timeframe=m5");
  CHECK(badTimeframe.status == drogon::k400BadRequest);
  REQUIRE(badTimeframe.hasJson);
  CHECK(badTimeframe.json.at("error").get<std::string>().find("unsupported timeframe") !=
        std::string::npos);

  const auto badSource =
      golddiggerui::test::get("/api/candles?timeframe=m15&source=telepathy");
  CHECK(badSource.status == drogon::k400BadRequest);

  const auto clamped =
      golddiggerui::test::get("/api/candles?timeframe=m15&count=999&source=file");
  CHECK(clamped.status == drogon::k200OK);
  REQUIRE(clamped.hasJson);
  CHECK(clamped.json.at("candles").size() <= 200);
}

DROGON_TEST(ApiSignalUsesTheResolvedModel)
{
  golddiggerui::test::writeFreshCandles();
  golddiggerui::test::resetState();

  const auto reply = golddiggerui::test::get("/api/signal?timeframe=m15&source=file");
  CHECK(reply.status == drogon::k200OK);
  REQUIRE(reply.hasJson);

  CHECK(reply.json.at("available").get<bool>() == true);
  CHECK(reply.json.at("action").get<std::string>() == "BUY");
  CHECK(reply.json.at("modelName").get<std::string>() == "xauusd-m15-bid.dat");
  CHECK(reply.json.at("stale").get<bool>() == false);
  CHECK(reply.json.at("candlesUsed").get<std::size_t>() == 96);
  CHECK(reply.json.at("candleAgeSeconds").get<double>() < 900.0 * 3.0 + 60.0);

  const auto prediction = reply.json.at("prediction");
  CHECK(prediction.at("change").get<double>() > 0.4);
  CHECK(prediction.at("horizonCandles").get<std::size_t>() == 6);
  CHECK(prediction.at("targetPrice").get<double>() > reply.json.at("price").get<double>());

  const auto execution = reply.json.at("execution");
  CHECK(execution.at("mode").get<std::string>() == "manual");
  CHECK(execution.at("armed").get<bool>() == false);
  CHECK(execution.at("paperTrading").get<bool>() == true);
  CHECK(execution.at("autoWouldExecute").get<bool>() == false);

  const auto badSource = golddiggerui::test::get("/api/signal?timeframe=m15&source=telepathy");
  CHECK(badSource.status == drogon::k400BadRequest);
  REQUIRE(badSource.hasJson);
  CHECK(badSource.json.at("error").get<std::string>().find("source") != std::string::npos);
}

DROGON_TEST(ApiSettingsUpdateValidatesAndPersists)
{
  golddiggerui::test::resetState();

  const auto before = golddiggerui::test::get("/api/settings");
  CHECK(before.status == drogon::k200OK);
  REQUIRE(before.hasJson);
  CHECK(before.json.at("confirmLiveRequired").get<bool>() == true);

  const auto updated = golddiggerui::test::put("/api/settings",
                                               R"({"volumeLots":0.25,"pollSeconds":30})");
  CHECK(updated.status == drogon::k200OK);
  REQUIRE(updated.hasJson);
  CHECK(std::abs(updated.json.at("settings").at("volumeLots").get<double>() - 0.25) < 1e-9);
  CHECK(updated.json.at("settings").at("pollSeconds").get<int>() == 30);

  std::ifstream in(golddiggerui::test::environment().stateDir / "settings.json");
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  CHECK(content.find("\"pollSeconds\": 30") != std::string::npos);

  const auto unsafe = golddiggerui::test::put("/api/settings", R"({"paperTrading":false})");
  CHECK(unsafe.status == drogon::k400BadRequest);
  REQUIRE(unsafe.hasJson);
  CHECK(unsafe.json.at("error").get<std::string>().find("confirmLive") != std::string::npos);

  CHECK(golddiggerui::test::put("/api/settings", R"({"volumeLots":-1})").status ==
        drogon::k400BadRequest);
  CHECK(golddiggerui::test::put("/api/settings", R"({"pollSeconds":4})").status ==
        drogon::k400BadRequest);
  CHECK(golddiggerui::test::put("/api/settings", R"({"autoTimeframes":["m5"]})").status ==
        drogon::k400BadRequest);
  CHECK(golddiggerui::test::put("/api/settings", "not json").status ==
        drogon::k400BadRequest);
}

DROGON_TEST(ApiSettingsRedactsTheAlphaVantageKey)
{
  golddiggerui::test::resetState();

  const auto reply =
      golddiggerui::test::put("/api/settings", R"({"alphaVantageApiKey":"topsecret"})");
  CHECK(reply.status == drogon::k200OK);
  REQUIRE(reply.hasJson);

  const auto settings = reply.json.at("settings");
  CHECK(settings.at("alphaVantageApiKey").get<std::string>().empty());
  CHECK(settings.at("alphaVantageApiKeySet").get<bool>() == true);

  const auto after = golddiggerui::test::get("/api/settings");
  REQUIRE(after.hasJson);
  CHECK(after.json.at("settings").at("alphaVantageApiKey").get<std::string>().empty());
  CHECK(after.json.at("settings").at("alphaVantageApiKeySet").get<bool>() == true);
}

DROGON_TEST(ApiManualOrderIsPaperAndLogged)
{
  golddiggerui::test::resetState();

  const auto placed = golddiggerui::test::post(
      "/api/orders",
      R"({"timeframe":"m15","side":"BUY","volumeLots":0.2,"price":2100.5,"comment":"api test"})");
  CHECK(placed.result == drogon::ReqResult::Ok);
  CHECK(placed.status == drogon::k200OK);
  REQUIRE(placed.hasJson);

  const auto order = placed.json.at("order");
  CHECK(order.at("status").get<std::string>() == "paper");
  CHECK(order.at("side").get<std::string>() == "BUY");
  CHECK(order.at("automatic").get<bool>() == false);
  CHECK(std::abs(order.at("volumeLots").get<double>() - 0.2) < 1e-9);
  CHECK(std::abs(order.at("referencePrice").get<double>() - 2100.5) < 1e-9);

  const auto listed = golddiggerui::test::get("/api/orders?limit=10");
  CHECK(listed.status == drogon::k200OK);
  REQUIRE(listed.hasJson);
  CHECK(listed.json.at("total").get<std::size_t>() == 1);
  REQUIRE(listed.json.at("orders").size() == 1);
  CHECK(listed.json.at("orders").at(0).at("message").get<std::string>().find("api test") !=
        std::string::npos);

  std::ifstream in(golddiggerui::test::environment().stateDir / "orders.csv");
  std::string header;
  std::getline(in, header);
  CHECK(header.rfind("timestamp_ms,", 0) == 0);
  std::string row;
  std::getline(in, row);
  CHECK(row.find(",paper,") != std::string::npos);

  CHECK(golddiggerui::test::post(
            "/api/orders", R"({"timeframe":"m15","side":"HOLD","volumeLots":0.2})")
            .status == drogon::k400BadRequest);
  CHECK(golddiggerui::test::post(
            "/api/orders", R"({"timeframe":"m15","side":"BUY","volumeLots":-1})")
            .status == drogon::k400BadRequest);
  CHECK(golddiggerui::test::post(
            "/api/orders", R"({"timeframe":"m15","side":"BUY","volumeLots":0.1,"stopLoss":-3})")
            .status == drogon::k400BadRequest);
  // The timeframe defaults to M15 when it is omitted.
  const auto defaulted =
      golddiggerui::test::post("/api/orders", R"({"side":"BUY","volumeLots":0.1})");
  CHECK(defaulted.status == drogon::k200OK);
  REQUIRE(defaulted.hasJson);
  CHECK(defaulted.json.at("order").at("timeframe").get<std::string>() == "m15");
}

DROGON_TEST(ApiEvaluateInManualModeDoesNotExecute)
{
  golddiggerui::test::writeFreshCandles();
  golddiggerui::test::resetState();

  const auto reply = golddiggerui::test::post("/api/signal/evaluate",
                                              R"({"timeframe":"m15","execute":true})");
  CHECK(reply.status == drogon::k200OK);
  REQUIRE(reply.hasJson);

  const auto execution = reply.json.at("execution");
  CHECK(execution.at("executed").get<bool>() == false);
  CHECK(execution.at("reason").get<std::string>().find("manual mode") != std::string::npos);
  CHECK(execution.at("order").is_null());

  const auto orders = golddiggerui::test::get("/api/orders");
  REQUIRE(orders.hasJson);
  CHECK(orders.json.at("total").get<std::size_t>() == 0);
}

DROGON_TEST(ApiAutoTraderRequiresArmingFreshCandleAndOneOrderPerCandle)
{
  golddiggerui::test::writeFreshCandles();
  golddiggerui::test::resetState();

  // Auto mode but not armed.
  CHECK(golddiggerui::test::put("/api/settings",
                                R"({"executionMode":"auto","autoArmed":false})")
            .status == drogon::k200OK);
  const auto unarmed = golddiggerui::test::post("/api/signal/evaluate",
                                                R"({"timeframe":"m15","execute":true})");
  REQUIRE(unarmed.hasJson);
  CHECK(unarmed.json.at("execution").at("executed").get<bool>() == false);
  CHECK(unarmed.json.at("execution").at("reason").get<std::string>().find("not armed") !=
        std::string::npos);

  // Armed, but a spread so wide that every prediction is below it.
  CHECK(golddiggerui::test::put("/api/settings",
                                R"({"executionMode":"auto","autoArmed":true,"estimatedSpread":10000})")
            .status == drogon::k200OK);
  const auto hold = golddiggerui::test::post("/api/signal/evaluate",
                                             R"({"timeframe":"m15","execute":true})");
  REQUIRE(hold.hasJson);
  CHECK(hold.json.at("execution").at("executed").get<bool>() == false);
  CHECK(hold.json.at("execution").at("reason").get<std::string>() == "the signal is HOLD");

  // Now an actionable, fresh signal. The first evaluation places a paper order.
  CHECK(golddiggerui::test::put("/api/settings",
                                R"({"executionMode":"auto","autoArmed":true,"estimatedSpread":0.4})")
            .status == drogon::k200OK);

  const auto signal = golddiggerui::test::get("/api/signal?timeframe=m15&source=file");
  REQUIRE(signal.hasJson);
  CHECK(signal.json.at("execution").at("autoWouldExecute").get<bool>() == true);

  const auto first = golddiggerui::test::post("/api/signal/evaluate",
                                              R"({"timeframe":"m15","execute":true})");
  REQUIRE(first.hasJson);
  CHECK(first.json.at("execution").at("executed").get<bool>() == true);
  const auto order = first.json.at("execution").at("order");
  CHECK(order.at("status").get<std::string>() == "paper");
  CHECK(order.at("automatic").get<bool>() == true);
  CHECK(order.at("side").get<std::string>() == "BUY");
  CHECK(order.at("candleTimestamp").get<std::uint64_t>() > 0);

  // The same candle must not be traded twice.
  const auto second = golddiggerui::test::post("/api/signal/evaluate",
                                               R"({"timeframe":"m15","execute":true})");
  REQUIRE(second.hasJson);
  CHECK(second.json.at("execution").at("executed").get<bool>() == false);
  CHECK(second.json.at("execution").at("reason").get<std::string>().find("already placed") !=
        std::string::npos);

  const auto orders = golddiggerui::test::get("/api/orders");
  REQUIRE(orders.hasJson);
  CHECK(orders.json.at("total").get<std::size_t>() == 1);
}

DROGON_TEST(ApiAutoTraderRefusesStaleCandles)
{
  golddiggerui::test::writeFreshCandles();
  golddiggerui::test::resetState();

  CHECK(golddiggerui::test::put("/api/settings",
                                R"({"executionMode":"auto","autoArmed":true})")
            .status == drogon::k200OK);

  golddiggerui::test::writeCandles("m15", 10);

  const auto signal = golddiggerui::test::get("/api/signal?timeframe=m15&source=file");
  REQUIRE(signal.hasJson);
  CHECK(signal.json.at("available").get<bool>() == true);
  CHECK(signal.json.at("stale").get<bool>() == true);
  CHECK(signal.json.at("execution").at("autoWouldExecute").get<bool>() == false);

  const auto reply = golddiggerui::test::post("/api/signal/evaluate",
                                              R"({"timeframe":"m15","execute":true})");
  REQUIRE(reply.hasJson);
  CHECK(reply.json.at("execution").at("executed").get<bool>() == false);
  CHECK(reply.json.at("execution").at("reason").get<std::string>().find("stale") !=
        std::string::npos);

  const auto orders = golddiggerui::test::get("/api/orders");
  REQUIRE(orders.hasJson);
  CHECK(orders.json.at("total").get<std::size_t>() == 0);

  golddiggerui::test::writeFreshCandles();
}

DROGON_TEST(ApiProviderTestValidatesTheTimeframe)
{
  golddiggerui::test::resetState();

  const auto reply = golddiggerui::test::post("/api/provider/test", R"({"timeframe":"m5"})");
  CHECK(reply.status == drogon::k400BadRequest);
  REQUIRE(reply.hasJson);
  CHECK(reply.json.at("error").get<std::string>().find("unsupported timeframe") !=
        std::string::npos);
}
