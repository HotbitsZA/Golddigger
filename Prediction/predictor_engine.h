#pragma once

#include "MarketData/candle_data_provider.h"
#include "Training/model_types.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct PredictorConfig
{
    std::string modelFile{"Models/gold_digger_m15.dat"};
    std::string instrument{"xauusd"};
    CandleTimeframe timeframe{CandleTimeframe::M15};
    std::size_t minimumCandles{50};
    std::size_t bootstrapCandles{64};
    std::size_t historyRetention{128};
    std::chrono::seconds pollInterval{60};
    std::chrono::seconds availabilityDelay{60};
    std::size_t maxPredictions{0};
    double estimatedSpread{0.4};    //Broker spread can be around 0.2-0.3 for XAUUSD, adding some buffer for safety.
};

class PredictorEngine
{
public:
    PredictorEngine(std::unique_ptr<ICandleDataProvider> provider, PredictorConfig config);

    int run();

private:
    bool loadModel();
    bool bootstrap();
    bool emitPrediction() const;
    void normalizeCandles(std::vector<Candle> &candles) const;
    void trimHistory();
    std::size_t requiredHistoryCandles() const noexcept;
    std::uint64_t latestAvailableUpperBoundMs(std::uint64_t nowTimestampMs) const;
    std::uint64_t nextFetchFromTimestampMs() const;
    std::chrono::system_clock::time_point nextAvailabilityTime() const;

    std::unique_ptr<ICandleDataProvider> m_provider;
    PredictorConfig m_config;
    dlib::decision_function<kernel_type> m_decisionFunction;
    dlib::vector_normalizer<sample_type> m_normalizer;
    std::vector<Candle> m_candles;
};
