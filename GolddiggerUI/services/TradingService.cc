#include "TradingService.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "MT5/mt5_zeromq_client.h"
#include "services/HttpJson.h"

namespace golddiggerui
{
  namespace fs = std::filesystem;

  namespace
  {
    std::string lowerCopy(std::string value)
    {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                     { return static_cast<char>(std::tolower(ch)); });
      return value;
    }

    std::string upperCopy(std::string value)
    {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                     { return static_cast<char>(std::toupper(ch)); });
      return value;
    }

    std::string csvEscape(const std::string &value)
    {
      if (value.find_first_of(",\"\r\n") == std::string::npos)
        return value;

      std::string escaped = "\"";
      for (const char ch : value)
      {
        if (ch == '"')
          escaped += '"';
        escaped += ch;
      }
      escaped += '"';
      return escaped;
    }

    std::vector<std::string> splitCsv(const std::string &line)
    {
      std::vector<std::string> fields;
      std::string current;
      bool quoted = false;

      for (std::size_t i = 0; i < line.size(); ++i)
      {
        const char ch = line[i];
        if (quoted)
        {
          if (ch == '"')
          {
            if (i + 1 < line.size() && line[i + 1] == '"')
            {
              current += '"';
              ++i;
            }
            else
            {
              quoted = false;
            }
          }
          else
          {
            current += ch;
          }
        }
        else if (ch == '"')
        {
          quoted = true;
        }
        else if (ch == ',')
        {
          fields.push_back(current);
          current.clear();
        }
        else if (ch != '\r')
        {
          current += ch;
        }
      }
      fields.push_back(current);
      return fields;
    }

    double toDouble(const std::vector<std::string> &fields, std::size_t index)
    {
      if (index >= fields.size() || fields[index].empty())
        return 0.0;
      return std::stod(fields[index]);
    }

    long long toLongLong(const std::vector<std::string> &fields, std::size_t index)
    {
      if (index >= fields.size() || fields[index].empty())
        return 0;
      return std::stoll(fields[index]);
    }

    std::uint64_t toUint64(const std::vector<std::string> &fields, std::size_t index)
    {
      if (index >= fields.size() || fields[index].empty())
        return 0;
      return std::stoull(fields[index]);
    }

    const char *kOrderHeader =
        "timestamp_ms,time_utc,automatic,instrument,timeframe,candle_timestamp_ms,"
        "side,volume_lots,reference_price,stop_loss,take_profit,status,message,ticket\n";
  } // namespace

  nlohmann::json OrderRecord::toJson() const
  {
    nlohmann::json json;
    json["timestamp"] = timestampMs;
    json["utc"] = timeUtc;
    json["automatic"] = automatic;
    json["instrument"] = instrument;
    json["timeframe"] = timeframe;
    json["candleTimestamp"] = candleTimestampMs;
    json["side"] = side;
    json["volumeLots"] = volumeLots;
    json["referencePrice"] = referencePrice;
    json["stopLoss"] = stopLoss;
    json["takeProfit"] = takeProfit;
    json["status"] = status;
    json["message"] = message;
    json["ticket"] = ticket;
    return json;
  }

  nlohmann::json SignalResult::toJson() const
  {
    nlohmann::json json;
    json["available"] = prediction.available;
    json["timeframe"] = prediction.timeframe;
    json["source"] = source;
    json["dataPath"] = dataPath;
    json["modelPath"] = prediction.modelPath;
    json["modelName"] = fs::path(prediction.modelPath).filename().string();
    json["candlesUsed"] = prediction.candlesUsed;
    json["candle"] = candleJson(prediction.candle);
    json["price"] = price;
    json["action"] = prediction.action;
    json["estimatedSpread"] = prediction.estimatedSpread;
    json["signalStrength"] =
        prediction.estimatedSpread > 0.0 ? prediction.change / prediction.estimatedSpread : 0.0;
    json["candleAgeSeconds"] = candleAgeSeconds;
    json["stale"] = stale;
    json["warning"] = warning;
    json["error"] = prediction.error;
    json["prediction"] = {
        {"logReturn", prediction.predictedLogReturn},
        {"targetPrice", prediction.targetPrice},
        {"change", prediction.change},
        {"changePct", prediction.changePct},
        {"action", prediction.action},
        {"horizonCandles", prediction.horizonCandles},
        {"horizonEndMs", prediction.horizonEndMs},
        {"horizonEndUtc", utcIsoFromMs(prediction.horizonEndMs)},
    };
    json["execution"] = {
        {"mode", executionMode},
        {"armed", autoArmed},
        {"paperTrading", paperTrading},
        {"volumeLots", volumeLots},
        {"autoWouldExecute", autoWouldExecute},
    };
    return json;
  }

  nlohmann::json EvaluationResult::toJson() const
  {
    nlohmann::json json;
    json["signal"] = signal.toJson();
    json["execution"] = {
        {"executed", executed},
        {"reason", reason},
        {"order", hasOrder ? order.toJson() : nlohmann::json(nullptr)},
    };
    return json;
  }

  TradingService::TradingService(AppPaths paths,
                                 SettingsStore &settings,
                                 MarketDataService &market,
                                 ModelService &models)
      : m_paths(std::move(paths)),
        m_settings(settings),
        m_market(market),
        m_models(models)
  {
  }

  std::vector<Candle> TradingService::candlesForSignal(CandleTimeframe timeframe,
                                                       const Settings &settings,
                                                       const std::string &source,
                                                       std::string &warning,
                                                       std::string &error) const
  {
    const std::size_t needed = 96;
    const std::string effective = lowerCopy(source);

    if (effective == "live")
    {
      try
      {
        return m_market.liveCandles(timeframe, settings.instrument, settings, needed);
      }
      catch (const std::exception &ex)
      {
        error = std::string("live candle fetch failed: ") + ex.what();
        return {};
      }
    }

    if (effective == "auto")
    {
      try
      {
        auto candles = m_market.liveCandles(timeframe, settings.instrument, settings, needed);
        if (!candles.empty())
          return candles;
        warning = "the provider returned no live candles; using the csv files";
      }
      catch (const std::exception &ex)
      {
        warning = std::string("live candle fetch failed (") + ex.what() + "); using the csv files";
      }
    }

    return m_market.recentCandles(timeframe, settings.instrument, needed);
  }

  SignalResult TradingService::signal(CandleTimeframe timeframe, const std::string &sourceOverride)
  {
    const Settings settings = m_settings.get();

    SignalResult result;
    result.executionMode = settings.executionMode;
    result.autoArmed = settings.autoArmed;
    result.paperTrading = settings.paperTrading;
    result.volumeLots = settings.volumeLots;
    result.source = sourceOverride.empty() ? settings.dataSource : lowerCopy(sourceOverride);
    result.dataPath = m_market.dataPath(timeframe, settings.instrument);
    result.prediction.timeframe = to_string(timeframe);
    result.prediction.estimatedSpread = settings.estimatedSpread;

    std::string warning;
    std::string error;
    std::vector<Candle> candles;
    try
    {
      candles = candlesForSignal(timeframe, settings, result.source, warning, error);
    }
    catch (const std::exception &ex)
    {
      error = ex.what();
    }
    result.warning = warning;

    if (!error.empty())
    {
      result.prediction.error = error;
      return result;
    }

    if (candles.empty())
    {
      result.prediction.error = "no candles available for " + to_string(timeframe) +
                                "; expected " + result.dataPath;
      return result;
    }

    result.price = candles.back().close;
    result.prediction = m_models.predict(timeframe, settings.instrument, candles,
                                        settings.estimatedSpread);

    const auto now = nowMs();
    const auto timestamp = candles.back().timestamp;
    const auto ageMs = (now > timestamp) ? (now - timestamp) : 0;
    result.candleAgeSeconds = static_cast<double>(ageMs) / 1000.0;

    const auto intervalMs = static_cast<std::uint64_t>(candle_interval(timeframe).count());
    const auto delayMs = static_cast<std::uint64_t>(std::max(0, settings.availabilityDelaySeconds)) * 1000ULL;
    const auto maxAgeMs = (intervalMs * 3ULL) + delayMs;
    result.stale = ageMs > maxAgeMs;

    result.autoWouldExecute = settings.executionMode == "auto" && settings.autoArmed &&
                              result.prediction.available && !result.stale &&
                              result.prediction.action != "HOLD";
    return result;
  }

  OrderRecord TradingService::placeOrder(CandleTimeframe timeframe,
                                         const std::string &side,
                                         double volumeLots,
                                         double price,
                                         double stopLoss,
                                         double takeProfit,
                                         bool automatic,
                                         const PredictionResult *prediction,
                                         const std::string &comment)
  {
    const Settings settings = m_settings.get();
    const std::string normalizedSide = upperCopy(side);
    if (normalizedSide != "BUY" && normalizedSide != "SELL")
      throw std::invalid_argument("side has to be BUY or SELL");

    if (volumeLots < 0.0)
      throw std::invalid_argument("'volumeLots' must not be negative");
    double volume = volumeLots > 0.0 ? volumeLots : settings.volumeLots;
    if (!(volume > 0.0) || volume > 1000.0)
      throw std::invalid_argument("'volumeLots' has to be greater than 0 and at most 1000");
    if (stopLoss < 0.0 || takeProfit < 0.0)
      throw std::invalid_argument("'stopLoss' and 'takeProfit' must not be negative");
    if (price < 0.0)
      throw std::invalid_argument("'price' must not be negative");

    OrderRecord order;
    order.timestampMs = nowMs();
    order.timeUtc = utcIsoFromMs(order.timestampMs);
    order.automatic = automatic;
    order.instrument = settings.instrument;
    order.timeframe = to_string(timeframe);
    order.side = normalizedSide;
    order.volumeLots = volume;
    order.message = comment;
    if (prediction != nullptr)
      order.candleTimestampMs = prediction->candle.timestamp;

    double reference = price;
    if (!(reference > 0.0) && prediction != nullptr && prediction->candle.close > 0.0)
      reference = prediction->candle.close;
    if (!(reference > 0.0))
    {
      try
      {
        const auto candles = m_market.recentCandles(timeframe, settings.instrument, 1);
        if (!candles.empty())
          reference = candles.back().close;
      }
      catch (const std::exception &)
      {
      }
    }
    if (!(reference > 0.0))
      throw std::runtime_error("no reference price is available for " + to_string(timeframe));

    order.referencePrice = reference;

    double sl = stopLoss;
    double tp = takeProfit;
    if (!(sl > 0.0) && settings.stopLossDistance > 0.0)
      sl = normalizedSide == "BUY" ? reference - settings.stopLossDistance
                                   : reference + settings.stopLossDistance;
    if (!(tp > 0.0) && settings.takeProfitDistance > 0.0)
      tp = normalizedSide == "BUY" ? reference + settings.takeProfitDistance
                                   : reference - settings.takeProfitDistance;
    order.stopLoss = sl > 0.0 ? sl : 0.0;
    order.takeProfit = tp > 0.0 ? tp : 0.0;

    if (settings.paperTrading)
    {
      order.status = "paper";
      order.message = "paper trade: the order was logged but not sent to a broker";
    }
    else
    {
      try
      {
        mt5::ClientConfig clientConfig;
        clientConfig.endpoint = settings.mt5Endpoint;
        clientConfig.clientId = settings.mt5ClientId;
        clientConfig.sendTimeoutMs = settings.mt5SendTimeoutMs;
        clientConfig.receiveTimeoutMs = settings.mt5ReceiveTimeoutMs;

        mt5::ZeroMqClient client(clientConfig);

        mt5::TradeRequest request;
        request.instrument = settings.instrument;
        request.volumeLots = volume;
        if (order.stopLoss > 0.0)
          request.stopLoss = order.stopLoss;
        if (order.takeProfit > 0.0)
          request.takeProfit = order.takeProfit;
        request.deviationPoints = 20;
        request.magic = static_cast<std::uint64_t>(std::max<long long>(0, settings.magic));
        request.comment = comment.empty() ? "golddigger-ui" : comment;

        const auto result = normalizedSide == "BUY" ? client.buy(request) : client.sell(request);
        order.status = result.accepted ? "filled" : "rejected";
        order.message = result.message;
        if (result.orderTicket.has_value())
          order.ticket = *result.orderTicket;
      }
      catch (const std::exception &ex)
      {
        order.status = "error";
        order.message = ex.what();
      }
    }

    appendOrder(order);
    return order;
  }

  EvaluationResult TradingService::evaluate(CandleTimeframe timeframe, bool allowAutoExecute)
  {
    EvaluationResult result;
    result.signal = signal(timeframe);

    const Settings settings = m_settings.get();
    if (!allowAutoExecute)
    {
      result.reason = "evaluation only";
      return result;
    }
    if (settings.executionMode != "auto")
    {
      result.reason = "manual mode: press execute or use the order ticket";
      return result;
    }
    if (!settings.autoArmed)
    {
      result.reason = "automatic execution is not armed";
      return result;
    }
    if (!result.signal.prediction.available)
    {
      result.reason = result.signal.prediction.error.empty() ? "no signal is available"
                                                             : result.signal.prediction.error;
      return result;
    }
    if (result.signal.stale)
    {
      result.reason = "the newest candle is stale; fetch live data first";
      return result;
    }
    if (result.signal.prediction.action == "HOLD")
    {
      result.reason = "the signal is HOLD";
      return result;
    }

    const std::string key = result.signal.prediction.timeframe + ":" +
                            std::to_string(result.signal.prediction.candle.timestamp);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ensureAutoKeysLocked();
      if (m_autoKeys.count(key) != 0)
      {
        result.reason = "an automatic order was already placed for this candle";
        return result;
      }
      m_autoKeys.insert(key);
    }

    try
    {
      result.order = placeOrder(timeframe,
                                result.signal.prediction.action,
                                settings.volumeLots,
                                0.0,
                                0.0,
                                0.0,
                                true,
                                &result.signal.prediction,
                                "automatic " + result.signal.prediction.action + " signal");
      result.executed = true;
      result.hasOrder = true;
      result.reason = (result.order.status == "filled" || result.order.status == "paper")
                          ? "automatic order placed"
                          : "automatic order failed: " + result.order.message;
    }
    catch (const std::exception &ex)
    {
      // No order was written, so the candle may be retried on the next cycle.
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_autoKeys.erase(key);
      }
      result.reason = std::string("automatic order failed: ") + ex.what();
    }
    return result;
  }

  std::vector<OrderRecord> TradingService::orders(std::size_t limit) const
  {
    auto all = readOrders();
    std::reverse(all.begin(), all.end()); // newest first
    if (all.size() > limit)
      all.resize(limit);
    return all;
  }

  std::size_t TradingService::orderCount() const
  {
    return readOrders().size();
  }

  void TradingService::runAutoCycle()
  {
    const Settings settings = m_settings.get();
    if (settings.executionMode != "auto" || !settings.autoArmed)
      return;

    for (const auto &id : settings.autoTimeframes)
    {
      CandleTimeframe timeframe{};
      if (!parse_timeframe(id, timeframe))
        continue;

      try
      {
        const auto result = evaluate(timeframe, true);
        if (result.executed)
        {
          std::cout << "GolddiggerUI auto trader: " << result.order.timeframe << ' '
                    << result.order.side << ' ' << result.order.volumeLots
                    << " lots at " << result.order.referencePrice << " ("
                    << result.order.status << ")" << std::endl;
        }
      }
      catch (const std::exception &ex)
      {
        std::cerr << "GolddiggerUI auto trader failed for " << id << ": " << ex.what() << '\n';
      }
    }
  }

  void TradingService::appendOrder(const OrderRecord &order) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    const fs::path path(m_paths.ordersFile);
    if (path.has_parent_path())
      fs::create_directories(path.parent_path());

    std::error_code ec;
    const bool exists = fs::is_regular_file(path, ec);

    std::ofstream out(path, std::ios::app);
    if (!out)
      throw std::runtime_error("cannot append to the order log: " + m_paths.ordersFile);

    if (!exists)
      out << kOrderHeader;

    out << order.timestampMs << ','
        << csvEscape(order.timeUtc) << ','
        << (order.automatic ? '1' : '0') << ','
        << csvEscape(order.instrument) << ','
        << csvEscape(order.timeframe) << ','
        << order.candleTimestampMs << ','
        << csvEscape(order.side) << ','
        << order.volumeLots << ','
        << order.referencePrice << ','
        << order.stopLoss << ','
        << order.takeProfit << ','
        << csvEscape(order.status) << ','
        << csvEscape(order.message) << ','
        << order.ticket << '\n';
  }

  std::vector<OrderRecord> TradingService::readOrders() const
  {
    std::vector<OrderRecord> orders;
    std::ifstream in(m_paths.ordersFile);
    if (!in)
      return orders;

    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line))
    {
      if (line.empty())
        continue;

      const auto fields = splitCsv(line);
      if (fields.size() < 13)
        continue;

      try
      {
        OrderRecord order;
        order.timestampMs = toUint64(fields, 0);
        order.timeUtc = fields[1];
        order.automatic = fields[2] == "1" || fields[2] == "true";
        order.instrument = fields[3];
        order.timeframe = fields[4];
        order.candleTimestampMs = toUint64(fields, 5);
        order.side = fields[6];
        order.volumeLots = toDouble(fields, 7);
        order.referencePrice = toDouble(fields, 8);
        order.stopLoss = toDouble(fields, 9);
        order.takeProfit = toDouble(fields, 10);
        order.status = fields[11];
        order.message = fields[12];
        order.ticket = toLongLong(fields, 13);
        orders.push_back(std::move(order));
      }
      catch (const std::exception &)
      {
        // Skip malformed rows; the log is append only and a partial write
        // must not take the whole UI down.
      }
    }
    return orders;
  }

  void TradingService::ensureAutoKeysLocked() const
  {
    if (m_autoKeysLoaded)
      return;
    m_autoKeysLoaded = true;
    for (const auto &order : readOrders())
    {
      if (!order.automatic)
        continue;
      m_autoKeys.insert(order.timeframe + ":" + std::to_string(order.candleTimestampMs));
    }
  }

  void TradingService::invalidateAutoKeys()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_autoKeys.clear();
    m_autoKeysLoaded = false;
  }
} // namespace golddiggerui
