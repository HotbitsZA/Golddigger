#pragma once

#include "Indicators/indicators.h"
#include "Training/model_types.h"

#include <cstddef>
#include <vector>

namespace feature_builder
{
    constexpr std::size_t kFeatureCount = 15;
    constexpr int kReturnFeatureCount = 5;
    constexpr int kRsiPeriod = 14;
    constexpr int kSma20Period = 20;
    constexpr int kSma50Period = 50;
    constexpr int kAdxPeriod = 14;
    constexpr int kAdxWindow = 28;
    constexpr std::size_t kM15HorizonCandles = 6;
    constexpr std::size_t kDefaultHorizonCandles = 4;
    constexpr std::size_t kFeatureStartIndex = 50;

    std::size_t min_history_candles();
    sample_type build_feature_sample(const std::vector<Candle> &candles, std::size_t index);
}