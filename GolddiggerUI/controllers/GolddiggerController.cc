#include "GolddiggerController.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "services/HttpJson.h"
#include "services/ServiceRegistry.h"

namespace golddiggerui
{
  namespace fs = std::filesystem;

  namespace
  {
    using json = nlohmann::json;

    struct HttpError : std::runtime_error
    {
      HttpError(const std::string &message, drogon::HttpStatusCode statusCode)
          : std::runtime_error(message), status(statusCode) {}
      drogon::HttpStatusCode status;
    };

    std::string lowerCopy(std::string value)
    {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                     { return static_cast<char>(std::tolower(ch)); });
      return value;
    }

    CandleTimeframe parseTimeframe(const std::string &value)
    {
      CandleTimeframe timeframe{};
      if (!parse_timeframe(lowerCopy(value), timeframe))
        throw std::invalid_argument("unsupported timeframe '" + value + "'; use m15, h1 or d1");
      return timeframe;
    }

    CandleTimeframe timeframeFrom(const drogon::HttpRequestPtr &req, const json &body)
    {
      if (body.contains("timeframe"))
      {
        if (!body["timeframe"].is_string())
          throw std::invalid_argument("'timeframe' has to be a string");
        return parseTimeframe(body["timeframe"].get<std::string>());
      }

      const auto parameter = req->getParameter("timeframe");
      if (!parameter.empty())
        return parseTimeframe(parameter);

      return CandleTimeframe::M15;
    }

    std::string textField(const json &body, const char *key, const std::string &fallback = std::string())
    {
      if (!body.contains(key) || body[key].is_null())
        return fallback;
      if (!body[key].is_string())
        throw std::invalid_argument(std::string("'") + key + "' has to be a string");
      return body[key].get<std::string>();
    }

    double numberField(const json &body, const char *key, double fallback)
    {
      if (!body.contains(key) || body[key].is_null())
        return fallback;
      const auto &value = body[key];
      if (value.is_number())
        return value.get<double>();
      if (value.is_string())
      {
        const std::string text = value.get<std::string>();
        try
        {
          std::size_t used = 0;
          const double parsed = std::stod(text, &used);
          if (used != text.size())
            throw std::invalid_argument("trailing characters");
          return parsed;
        }
        catch (const std::exception &)
        {
          throw std::invalid_argument(std::string("'") + key + "' is not a number");
        }
      }
      throw std::invalid_argument(std::string("'") + key + "' is not a number");
    }

    bool boolField(const json &body, const char *key, bool fallback)
    {
      if (!body.contains(key) || body[key].is_null())
        return fallback;
      const auto &value = body[key];
      if (value.is_boolean())
        return value.get<bool>();
      if (value.is_number())
        return value.get<double>() != 0.0;
      if (value.is_string())
      {
        const std::string text = lowerCopy(value.get<std::string>());
        if (text == "true" || text == "1" || text == "yes" || text == "on")
          return true;
        if (text == "false" || text == "0" || text == "no" || text == "off")
          return false;
      }
      throw std::invalid_argument(std::string("'") + key + "' is not a boolean");
    }

    json parseBody(const drogon::HttpRequestPtr &req, bool allowEmpty = true)
    {
      const std::string raw(req->body());
      if (raw.empty())
      {
        if (allowEmpty)
          return json::object();
        throw std::invalid_argument("the request body has to be a JSON object");
      }

      try
      {
        auto body = json::parse(raw);
        if (!body.is_object())
          throw std::invalid_argument("the request body has to be a JSON object");
        return body;
      }
      catch (const std::invalid_argument &)
      {
        throw;
      }
      catch (const std::exception &)
      {
        throw std::invalid_argument("the request body has to be a JSON object");
      }
    }

    std::size_t countParameter(const drogon::HttpRequestPtr &req,
                               const char *name,
                               std::size_t fallback,
                               std::size_t maximum)
    {
      const std::string value = req->getParameter(name);
      if (value.empty())
        return fallback;
      char *end = nullptr;
      const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
      if (end == value.c_str())
        return fallback;
      return std::min<std::size_t>(std::max<std::size_t>(parsed, 1), maximum);
    }

    json pathsJson(const AppPaths &paths)
    {
      json result;
      result["root"] = paths.root;
      result["dataDir"] = paths.dataDir;
      result["modelsDir"] = paths.modelsDir;
      result["webDir"] = paths.webDir;
      result["stateDir"] = paths.stateDir;
      result["settingsFile"] = paths.settingsFile;
      result["ordersFile"] = paths.ordersFile;
      return result;
    }

    json providerReadiness(const Settings &settings)
    {
      json providers;

      providers["dukascopy"] = {
          {"ready", !settings.dukascopyCommand.empty()},
          {"detail", settings.dukascopyCommand.empty() ? "no Dukascopy command configured"
                                                       : settings.dukascopyCommand},
      };
      providers["alphavantage"] = {
          {"ready", !settings.alphaVantageApiKey.empty()},
          {"detail", settings.alphaVantageApiKey.empty() ? "ALPHAVANTAGE_API_KEY is not set"
                                                         : "API key configured"},
      };
      providers["mt5"] = {
          {"ready", !settings.mt5Endpoint.empty()},
          {"detail", settings.mt5Endpoint.empty() ? "no MT5 endpoint configured" : settings.mt5Endpoint},
      };
      return providers;
    }

    std::string timeframeLabel(CandleTimeframe timeframe)
    {
      switch (timeframe)
      {
      case CandleTimeframe::M15:
        return "M15";
      case CandleTimeframe::H1:
        return "H1";
      case CandleTimeframe::D1:
        return "D1";
      }
      return "M15";
    }

    json timeframeSummary(CandleTimeframe timeframe, const Settings &settings)
    {
      auto &registry = services();
      const std::string id = to_string(timeframe);
      const auto intervalMs = static_cast<std::uint64_t>(candle_interval(timeframe).count());
      const auto delayMs = static_cast<std::uint64_t>(std::max(0, settings.availabilityDelaySeconds)) * 1000ULL;
      const auto now = nowMs();

      const std::string dataPath = registry.market->dataPath(timeframe, settings.instrument);
      const bool dataExists = registry.market->hasData(timeframe, settings.instrument);

      std::shared_ptr<const std::vector<Candle>> candles;
      try
      {
        candles = registry.market->allCandles(timeframe, settings.instrument);
      }
      catch (const std::exception &)
      {
        candles = nullptr;
      }

      const std::uint64_t latestMs =
          candles && !candles->empty() ? candles->back().timestamp : 0;
      const auto ageMs = (latestMs > 0 && now > latestMs) ? (now - latestMs) : 0;
      const bool stale = latestMs == 0 || ageMs > (intervalMs * 3ULL + delayMs);

      const std::string modelPath = registry.models->resolve(timeframe, settings.instrument);
      std::error_code ec;
      const bool modelExists = fs::is_regular_file(modelPath, ec);

      json model;
      model["path"] = modelPath;
      model["name"] = fs::path(modelPath).filename().string();
      model["exists"] = modelExists;

      json data;
      data["path"] = dataPath;
      data["exists"] = dataExists;
      data["candles"] = candles ? candles->size() : 0;
      data["latestTimestamp"] = latestMs;
      data["latestUtc"] = latestMs > 0 ? utcIsoFromMs(latestMs) : std::string();
      data["latestLocal"] = latestMs > 0 ? format_local_timestamp(latestMs) : std::string();
      data["ageSeconds"] = static_cast<double>(ageMs) / 1000.0;
      data["stale"] = stale;

      json summary;
      summary["id"] = id;
      summary["label"] = timeframeLabel(timeframe);
      summary["intervalMs"] = intervalMs;
      summary["model"] = model;
      summary["data"] = data;
      return summary;
    }

    json allTimeframeSummaries(const Settings &settings)
    {
      json list = json::array();
      for (const auto timeframe : {CandleTimeframe::M15, CandleTimeframe::H1, CandleTimeframe::D1})
        list.push_back(timeframeSummary(timeframe, settings));
      return list;
    }

    template <typename Function>
    void guarded(const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                 Function &&body)
    {
      try
      {
        body();
      }
      catch (const HttpError &ex)
      {
        callback(jsonError(ex.what(), ex.status));
      }
      catch (const std::invalid_argument &ex)
      {
        callback(jsonError(ex.what(), drogon::k400BadRequest));
      }
      catch (const std::exception &ex)
      {
        callback(jsonError(ex.what(), drogon::k500InternalServerError));
      }
    }
  } // namespace

  void GolddiggerController::status(const drogon::HttpRequestPtr & /*req*/,
                                    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const Settings settings = registry.settings->get();

      json body;
      body["app"] = "GolddiggerUI";
      body["version"] = "1.0.0";
      body["instrument"] = settings.instrument;
      body["now"] = nowMs();
      body["nowUtc"] = utcIsoFromMs(nowMs());
      body["paths"] = pathsJson(registry.paths());
      body["settings"] = settings.toJson();
      body["providers"] = providerReadiness(settings);
      body["timeframes"] = allTimeframeSummaries(settings);
      body["modelCount"] = registry.models->list().size();
      body["orderCount"] = registry.trading->orderCount();
      body["ordersFile"] = registry.paths().ordersFile;
      callback(jsonResponse(body)); });
  }

  void GolddiggerController::timeframes(const drogon::HttpRequestPtr & /*req*/,
                                        std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const Settings settings = registry.settings->get();
      json body;
      body["instrument"] = settings.instrument;
      body["source"] = settings.dataSource;
      body["timeframes"] = allTimeframeSummaries(settings);
      callback(jsonResponse(body)); });
  }

  void GolddiggerController::models(const drogon::HttpRequestPtr & /*req*/,
                                    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const Settings settings = registry.settings->get();

      json list = json::array();
      for (const auto &model : registry.models->list())
      {
        json entry;
        entry["timeframe"] = model.timeframe;
        entry["name"] = model.name;
        entry["path"] = model.path;
        entry["exists"] = model.exists;
        entry["sizeBytes"] = model.sizeBytes;
        entry["modifiedUtc"] = model.modifiedUtc;
        list.push_back(entry);
      }

      json selected;
      for (const auto timeframe : {CandleTimeframe::M15, CandleTimeframe::H1, CandleTimeframe::D1})
      {
        selected[to_string(timeframe)] = registry.models->resolve(timeframe, settings.instrument);
      }

      json body;
      body["modelsDir"] = registry.paths().modelsDir;
      body["models"] = list;
      body["selected"] = selected;
      callback(jsonResponse(body)); });
  }

  void GolddiggerController::candles(const drogon::HttpRequestPtr &req,
                                     std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const Settings settings = registry.settings->get();
      const CandleTimeframe timeframe = timeframeFrom(req, json::object());
      const std::size_t count = countParameter(req, "count", 5, 200);

      std::string source = req->getParameter("source");
      source = source.empty() ? settings.dataSource : lowerCopy(source);
      if (source != "file" && source != "live" && source != "auto")
        throw std::invalid_argument("'source' has to be file, live or auto");

      std::vector<Candle> candles;
      std::string warning;
      if (source == "live" || source == "auto")
      {
        try
        {
          candles = registry.market->liveCandles(timeframe, settings.instrument, settings, count);
          if (candles.empty() && source == "live")
            throw std::runtime_error("the provider returned no candles");
        }
        catch (const std::exception &ex)
        {
          if (source == "live")
            throw HttpError(std::string("live candle fetch failed: ") + ex.what(),
                            drogon::k502BadGateway);
          warning = std::string("live candle fetch failed (") + ex.what() + "); showing the csv files";
        }
      }

      if (candles.empty())
      {
        try
        {
          candles = registry.market->recentCandles(timeframe, settings.instrument, count);
        }
        catch (const std::exception &ex)
        {
          throw std::runtime_error(std::string("cannot read the candle file: ") + ex.what());
        }
      }

      if (candles.empty() && warning.empty())
        warning = "no candles are available for " + to_string(timeframe) +
                  "; run the patcher or fetch live data";

      json list = json::array();
      for (const auto &candle : candles)
        list.push_back(candleJson(candle));

      json body;
      body["timeframe"] = to_string(timeframe);
      body["source"] = source;
      body["dataPath"] = registry.market->dataPath(timeframe, settings.instrument);
      body["count"] = list.size();
      body["warning"] = warning;
      body["candles"] = list;
      callback(jsonResponse(body)); });
  }

  void GolddiggerController::refreshCandles(const drogon::HttpRequestPtr &req,
                                            std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const Settings settings = registry.settings->get();
      const json body = parseBody(req);
      const CandleTimeframe timeframe = timeframeFrom(req, body);
      const std::size_t count = static_cast<std::size_t>(
          std::max(1.0, std::min(200.0, numberField(body, "count", 5.0))));
      const bool persist = boolField(body, "persist", true);

      RefreshResult result;
      try
      {
        result = registry.market->refresh(timeframe, settings.instrument, settings, count, persist);
      }
      catch (const std::exception &ex)
      {
        throw HttpError(std::string("live candle fetch failed: ") + ex.what(),
                        drogon::k502BadGateway);
      }

      json candles = json::array();
      for (const auto &candle : result.candles)
        candles.push_back(candleJson(candle));

      json response;
      response["timeframe"] = to_string(timeframe);
      response["source"] = "live";
      response["path"] = result.path;
      response["fetched"] = result.fetched;
      response["added"] = result.added;
      response["replaced"] = result.replaced;
      response["total"] = result.total;
      response["persisted"] = result.persisted;
      response["candles"] = candles;
      callback(jsonResponse(response)); });
  }

  void GolddiggerController::signal(const drogon::HttpRequestPtr &req,
                                    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const CandleTimeframe timeframe = timeframeFrom(req, json::object());
      std::string source = req->getParameter("source");
      source = lowerCopy(source);
      if (!source.empty() && source != "file" && source != "live" && source != "auto")
        throw std::invalid_argument("'source' has to be file, live or auto");
      const auto result = registry.trading->signal(timeframe, source);
      callback(jsonResponse(result.toJson())); });
  }

  void GolddiggerController::evaluateSignal(const drogon::HttpRequestPtr &req,
                                            std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const json body = parseBody(req);
      const CandleTimeframe timeframe = timeframeFrom(req, body);
      const bool execute = boolField(body, "execute", true);
      const auto result = registry.trading->evaluate(timeframe, execute);
      callback(jsonResponse(result.toJson())); });
  }

  void GolddiggerController::listOrders(const drogon::HttpRequestPtr &req,
                                        std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const std::size_t limit = countParameter(req, "limit", 50, 1000);
      const auto orders = registry.trading->orders(limit);

      json list = json::array();
      for (const auto &order : orders)
        list.push_back(order.toJson());

      json body;
      body["orders"] = list;
      body["count"] = list.size();
      body["total"] = registry.trading->orderCount();
      body["ordersFile"] = registry.paths().ordersFile;
      callback(jsonResponse(body)); });
  }

  void GolddiggerController::placeOrder(const drogon::HttpRequestPtr &req,
                                        std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const json body = parseBody(req);
      const CandleTimeframe timeframe = timeframeFrom(req, body);

      const std::string side = textField(body, "side");
      if (side.empty())
        throw std::invalid_argument("'side' is required and has to be BUY or SELL");

      const double volume = numberField(body, "volumeLots", 0.0);
      const double price = numberField(body, "price", 0.0);
      const double stopLoss = numberField(body, "stopLoss", 0.0);
      const double takeProfit = numberField(body, "takeProfit", 0.0);
      const std::string comment = textField(body, "comment", "manual order from GolddiggerUI");

      const auto order = registry.trading->placeOrder(
          timeframe, side, volume, price, stopLoss, takeProfit, false, nullptr, comment);

      json response;
      response["order"] = order.toJson();
      const bool failed = order.status == "error" || order.status == "rejected";
      if (order.status == "error")
        response["error"] = order.message;
      callback(jsonResponse(response, failed ? drogon::k502BadGateway : drogon::k200OK)); });
  }

  void GolddiggerController::getSettings(const drogon::HttpRequestPtr & /*req*/,
                                         std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const Settings settings = registry.settings->get();

      json body;
      body["settings"] = settings.toJson();
      body["paths"] = pathsJson(registry.paths());
      body["providers"] = providerReadiness(settings);
      body["confirmLiveRequired"] = true;
      callback(jsonResponse(body)); });
  }

  void GolddiggerController::updateSettings(const drogon::HttpRequestPtr &req,
                                            std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const json body = parseBody(req, false);
      const bool liveAuthorised = boolField(body, "confirmLive", false);

      const Settings updated = registry.settings->update(body, liveAuthorised);

      json response;
      response["settings"] = updated.toJson();
      response["paths"] = pathsJson(registry.paths());
      response["providers"] = providerReadiness(updated);
      callback(jsonResponse(response)); });
  }

  void GolddiggerController::testProvider(const drogon::HttpRequestPtr &req,
                                          std::function<void(const drogon::HttpResponsePtr &)> &&callback)
  {
    guarded(callback, [&]()
            {
      auto &registry = services();
      const Settings settings = registry.settings->get();
      const json body = parseBody(req);
      const CandleTimeframe timeframe = timeframeFrom(req, body);

      std::vector<Candle> candles;
      try
      {
        candles = registry.market->liveCandles(timeframe, settings.instrument, settings, 2);
      }
      catch (const std::exception &ex)
      {
        throw HttpError(std::string("provider test failed: ") + ex.what(),
                        drogon::k502BadGateway);
      }

      json list = json::array();
      for (const auto &candle : candles)
        list.push_back(candleJson(candle));

      json response;
      response["provider"] = settings.provider;
      response["timeframe"] = to_string(timeframe);
      response["count"] = list.size();
      response["candles"] = list;
      callback(jsonResponse(response)); });
  }
} // namespace golddiggerui
