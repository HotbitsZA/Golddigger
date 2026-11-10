#pragma once

/**
  Small helpers that turn the service structs into JSON and JSON into Drogon
  responses. The whole API uses nlohmann::json (the rest of Golddigger does),
  so the services stay free of jsoncpp.
*/

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "Indicators/indicators.h"
#include "MarketData/candle_data_provider.h"

namespace golddiggerui
{
  inline std::string utcIsoFromMs(std::uint64_t timestampMs)
  {
    const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
  }

  inline std::uint64_t nowMs()
  {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  inline nlohmann::json candleJson(const Candle &candle)
  {
    nlohmann::json json;
    json["timestamp"] = candle.timestamp;
    json["utc"] = utcIsoFromMs(candle.timestamp);
    json["local"] = format_local_timestamp(candle.timestamp);
    json["open"] = candle.open;
    json["high"] = candle.high;
    json["low"] = candle.low;
    json["close"] = candle.close;
    json["volume"] = candle.volume;
    json["bullish"] = candle.close >= candle.open;
    json["change"] = candle.close - candle.open;
    json["changePct"] = candle.open != 0.0 ? ((candle.close - candle.open) / candle.open) * 100.0 : 0.0;
    json["range"] = candle.high - candle.low;
    return json;
  }

  inline drogon::HttpResponsePtr jsonResponse(const nlohmann::json &body,
                                              drogon::HttpStatusCode status = drogon::k200OK)
  {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeString("application/json; charset=utf-8");
    response->addHeader("Cache-Control", "no-store");
    response->setBody(body.dump());
    return response;
  }

  inline drogon::HttpResponsePtr jsonError(const std::string &message,
                                           drogon::HttpStatusCode status = drogon::k400BadRequest)
  {
    nlohmann::json body;
    body["error"] = message;
    return jsonResponse(body, status);
  }
} // namespace golddiggerui
