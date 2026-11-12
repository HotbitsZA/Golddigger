#pragma once

/**
  GolddiggerUI - signals and order execution.

  A signal is the newest candle plus the inference result of the model the
  timeframe selected. Execution can be manual (the user presses a button) or
  automatic. Automatic orders are only placed when the mode is "auto", the
  auto trader is armed, the signal is not stale and the candle has not been
  traded before.

  Orders are always appended to `orders.csv` in the state directory. Paper
  trading stops there; live trading sends the same order over the MT5 ZeroMQ
  contract and records the broker answer as well.
*/

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "MarketData/candle_data_provider.h"
#include "services/AppPaths.h"
#include "services/MarketDataService.h"
#include "services/ModelService.h"
#include "services/Settings.h"

namespace golddiggerui
{
  struct OrderRecord
  {
    std::uint64_t timestampMs = 0;
    std::string timeUtc;
    bool automatic = false;
    std::string instrument;
    std::string timeframe;
    std::uint64_t candleTimestampMs = 0;
    std::string side; ///< BUY or SELL
    double volumeLots = 0.0;
    double referencePrice = 0.0;
    double stopLoss = 0.0;
    double takeProfit = 0.0;
    std::string status; ///< paper, filled, rejected, error
    std::string message;
    long long ticket = 0;

    nlohmann::json toJson() const;
  };

  struct SignalResult
  {
    PredictionResult prediction;
    std::string source;
    std::string dataPath;
    std::string warning;
    double price = 0.0;
    double candleAgeSeconds = 0.0;
    bool stale = true;

    std::string executionMode{"manual"};
    bool autoArmed = false;
    bool paperTrading = true;
    double volumeLots = 0.0;
    bool autoWouldExecute = false;

    nlohmann::json toJson() const;
  };

  struct EvaluationResult
  {
    SignalResult signal;
    bool executed = false;
    std::string reason;
    bool hasOrder = false;
    OrderRecord order;

    nlohmann::json toJson() const;
  };

  class TradingService
  {
  public:
    TradingService(AppPaths paths,
                   SettingsStore &settings,
                   MarketDataService &market,
                   ModelService &models);

    /// Build a signal without ever placing an order.
    SignalResult signal(CandleTimeframe timeframe,
                        const std::string &sourceOverride = std::string());

    /// Build a signal and, when allowed, place an automatic order.
    EvaluationResult evaluate(CandleTimeframe timeframe, bool allowAutoExecute);

    /// Place an order. `price` 0 means the newest close of the timeframe.
    /// `stopLoss`/`takeProfit` 0 fall back to the configured distances.
    OrderRecord placeOrder(CandleTimeframe timeframe,
                           const std::string &side,
                           double volumeLots,
                           double price,
                           double stopLoss,
                           double takeProfit,
                           bool automatic,
                           const PredictionResult *prediction,
                           const std::string &comment);

    /// The newest `limit` orders, newest first.
    std::vector<OrderRecord> orders(std::size_t limit) const;

    /// Number of rows in the order log.
    std::size_t orderCount() const;

    /// One pass of the server side auto trader.
    void runAutoCycle();

    const AppPaths &paths() const { return m_paths; }

  private:
    std::vector<Candle> candlesForSignal(CandleTimeframe timeframe,
                                         const Settings &settings,
                                         const std::string &source,
                                         std::string &warning,
                                         std::string &error) const;

    void appendOrder(const OrderRecord &order) const;
    std::vector<OrderRecord> readOrders() const;
    void ensureAutoKeysLocked() const;
    void invalidateAutoKeys();

    AppPaths m_paths;
    SettingsStore &m_settings;
    MarketDataService &m_market;
    ModelService &m_models;

    mutable std::mutex m_mutex;
    mutable std::set<std::string> m_autoKeys;
    mutable bool m_autoKeysLoaded = false;
  };
} // namespace golddiggerui
