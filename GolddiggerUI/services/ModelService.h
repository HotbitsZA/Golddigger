#pragma once

/**
  GolddiggerUI - model discovery and inference.

  The UI links a timeframe to its model automatically. `Models/xauusd-m15-bid.dat`
  is the first choice for M15, then `gold_digger_m15.dat`, then any other
  non-tuner `.dat` whose name contains the timeframe tag.

  Inference is the same pipeline as `predictor`: build the 15 feature sample
  from the newest candle, run the serialized SVR decision function and turn the
  predicted log return into a target price and an action.
*/

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Indicators/indicators.h"
#include "MarketData/candle_data_provider.h"
#include "services/AppPaths.h"

namespace golddiggerui
{
  struct ModelInfo
  {
    std::string timeframe; ///< "m15", "h1", "d1" or empty when unknown
    std::string name;
    std::string path;
    bool exists = false;
    std::uintmax_t sizeBytes = 0;
    std::string modifiedUtc;
  };

  /// The result of one inference pass. `available` is false when the model is
  /// missing or there are not enough candles; `error` then says why.
  struct PredictionResult
  {
    bool available = false;
    std::string error;
    std::string timeframe;
    std::string modelPath;
    std::size_t candlesUsed = 0;

    Candle candle{};
    double predictedLogReturn = 0.0;
    double targetPrice = 0.0;
    double change = 0.0;
    double changePct = 0.0;
    std::string action{"HOLD"};
    double estimatedSpread = 0.0;
    std::size_t horizonCandles = 0;
    std::uint64_t horizonEndMs = 0;
  };

  class ModelService
  {
  public:
    explicit ModelService(AppPaths paths);

    const AppPaths &paths() const { return m_paths; }

    /// Every trained model in the models directory, tuner files excluded.
    std::vector<ModelInfo> list() const;

    /// The model the timeframe selects. Returns the preferred path even when
    /// it does not exist, so callers can show where the model is expected.
    std::string resolve(CandleTimeframe timeframe,
                        const std::string &instrument,
                        const std::string &overridePath = std::string()) const;

    /// True when the timeframe resolves to an existing model.
    bool hasModel(CandleTimeframe timeframe,
                  const std::string &instrument,
                  const std::string &overridePath = std::string()) const;

    /// Run the model over the newest candles. Never throws for missing data;
    /// the result carries `available` and `error` instead.
    PredictionResult predict(CandleTimeframe timeframe,
                             const std::string &instrument,
                             const std::vector<Candle> &candles,
                             double estimatedSpread,
                             const std::string &overridePath = std::string());

    static std::string timeframeOfModel(const std::string &fileName);

  private:
    struct LoadedModel;

    std::shared_ptr<const LoadedModel> loadModel(const std::string &path) const;

    AppPaths m_paths;
    mutable std::mutex m_mutex;
    mutable std::map<std::string, std::shared_ptr<const LoadedModel>> m_cache;
  };
} // namespace golddiggerui
