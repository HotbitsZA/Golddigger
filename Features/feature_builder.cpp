#include "Features/feature_builder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace
{
    std::pair<double, double> cyclical_time_features(std::uint64_t timestampMs)
    {
        const auto timePoint = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
        const auto hoursSinceEpoch = std::chrono::duration_cast<std::chrono::hours>(
                                         timePoint.time_since_epoch())
                                         .count();

        const auto hourOfDay = static_cast<double>(hoursSinceEpoch % 24);
        const auto dayOfWeek = static_cast<double>((hoursSinceEpoch / 24) % 7);
        return {hourOfDay, dayOfWeek};
    }
}

namespace feature_builder
{
    std::size_t min_history_candles()
    {
        return std::max<std::size_t>({static_cast<std::size_t>(kSma50Period),
                                      static_cast<std::size_t>(kAdxWindow),
                                      static_cast<std::size_t>(kReturnFeatureCount + 1)});
    }

    sample_type build_feature_sample(const std::vector<Candle> &candles, std::size_t index)
    {
        if (candles.size() < min_history_candles())
            throw std::runtime_error("Not enough candles to build a feature sample.");

        if (index >= candles.size())
            throw std::runtime_error("Feature sample index is out of range.");

        // The SMA15/50 window reads candles[index-49 .. index], and the ADX/ATR window
        // reads candles[index-28 .. index-1], so the lowest buildable index is 49.
        if (index + 1 < static_cast<std::size_t>(kSma50Period))
            throw std::runtime_error("Feature sample index is too small for the indicators.");

        sample_type sample;

        for (int j = 0; j < kReturnFeatureCount; ++j)
            sample(j) = std::log(candles[index - j].close / candles[index - j - 1].close);

        std::vector<double> closes;
        closes.reserve(kSma50Period + 1);
        for (std::size_t k = index - kSma50Period; k <= index; ++k)
            closes.push_back(candles[k].close);

        sample(5) = get_rsi(closes, kRsiPeriod);

        std::vector<Candle> adxCandles(candles.begin() + static_cast<std::ptrdiff_t>(index - kAdxWindow),
                                       candles.begin() + static_cast<std::ptrdiff_t>(index));
        sample(6) = calculate_adx(adxCandles, kAdxPeriod);

        const double rawAtr = get_atr(adxCandles, kAdxPeriod);
        sample(7) = (rawAtr > 0.0) ? (rawAtr / candles[index].close) : 0.0;

        const double rawSma20 = get_sma(closes, kSma20Period);
        sample(8) = (rawSma20 > 0.0) ? ((candles[index].close - rawSma20) / candles[index].close) : 0.0;

        const double rawSma50 = get_sma(closes, kSma50Period);
        sample(9) = (rawSma50 > 0.0) ? ((candles[index].close - rawSma50) / candles[index].close) : 0.0;
        sample(10) = (rawSma50 > 0.0) ? (rawSma20 - rawSma50) / candles[index].close : 0.0;

        const auto [hourOfDay, dayOfWeek] = cyclical_time_features(candles[index].timestamp);
        const double hourAngle = (2 * M_PI * hourOfDay) / 24.0;
        const double dayAngle = (2 * M_PI * dayOfWeek) / 7.0;
        sample(11) = std::sin(hourAngle);
        sample(12) = std::cos(hourAngle);
        sample(13) = std::sin(dayAngle);
        sample(14) = std::cos(dayAngle);

        return sample;
    }
}