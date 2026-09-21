#include "doctest.h"

#include "Features/feature_builder.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
    std::vector<Candle> synthetic_candles(std::size_t count)
    {
        std::vector<Candle> candles;
        candles.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const double close = 1800.0 + static_cast<double>(i) * 0.5 + std::sin(static_cast<double>(i));
            candles.push_back(Candle{
                1641164400000ULL + static_cast<std::uint64_t>(i) * 900000ULL,
                close,
                close + 2.0,
                close - 2.0,
                close,
                static_cast<double>(i)});
        }
        return candles;
    }

    // Reference implementation that mirrors the canonical feature construction so the
    // shared builder cannot drift from the documented semantics without failing a test.
    sample_type reference_sample(const std::vector<Candle> &candles, std::size_t index)
    {
        const int kReturnFeatureCount = 5;
        const int kRsiPeriod = 14;
        const int kSma20Period = 20;
        const int kSma50Period = 50;
        const int kAdxPeriod = 14;
        const int kAdxWindow = 28;

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

        const auto timePoint = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(candles[index].timestamp));
        const auto hoursSinceEpoch = std::chrono::duration_cast<std::chrono::hours>(
                                         timePoint.time_since_epoch())
                                         .count();
        const double hourOfDay = static_cast<double>(hoursSinceEpoch % 24);
        const double dayOfWeek = static_cast<double>((hoursSinceEpoch / 24) % 7);
        const double hourAngle = (2 * M_PI * hourOfDay) / 24.0;
        const double dayAngle = (2 * M_PI * dayOfWeek) / 7.0;
        sample(11) = std::sin(hourAngle);
        sample(12) = std::cos(hourAngle);
        sample(13) = std::sin(dayAngle);
        sample(14) = std::cos(dayAngle);

        return sample;
    }
}

TEST_CASE("minimum history requirements are stable")
{
    REQUIRE(feature_builder::min_history_candles() == 50);
    REQUIRE(feature_builder::kFeatureCount == 15);
    REQUIRE(feature_builder::kM15HorizonCandles == 6);
    REQUIRE(feature_builder::kDefaultHorizonCandles == 4);
}

TEST_CASE("feature builder rejects undersized input")
{
    const auto candles = synthetic_candles(20);
    REQUIRE_THROWS_AS(feature_builder::build_feature_sample(candles, 10), std::runtime_error);
}

TEST_CASE("feature builder rejects out of range index")
{
    const auto candles = synthetic_candles(80);
    // The last valid index is size - 1; one past the end must throw.
    REQUIRE_NOTHROW(feature_builder::build_feature_sample(candles, 79));
    REQUIRE_THROWS_AS(feature_builder::build_feature_sample(candles, 80), std::runtime_error);
    REQUIRE_THROWS_AS(feature_builder::build_feature_sample(candles, 500), std::runtime_error);
}

TEST_CASE("feature builder matches the reference construction (feature parity)")
{
    const auto candles = synthetic_candles(120);

    for (const std::size_t index : {std::size_t{50}, std::size_t{65}, std::size_t{100}, std::size_t{119}})
    {
        const auto actual = feature_builder::build_feature_sample(candles, index);
        const auto expected = reference_sample(candles, index);

        REQUIRE(actual.nr() == expected.nr());
        REQUIRE(actual.nc() == expected.nc());
        for (long j = 0; j < expected.nr(); ++j)
        {
            CAPTURE(index);
            CAPTURE(j);
            CHECK(std::abs(actual(j) - expected(j)) < 1e-12);
        }
    }
}

TEST_CASE("feature builder output is finite")
{
    const auto candles = synthetic_candles(200);
    for (std::size_t index = 50; index < candles.size(); ++index)
    {
        const auto sample = feature_builder::build_feature_sample(candles, index);
        for (long j = 0; j < sample.nr(); ++j)
            REQUIRE(std::isfinite(sample(j)));
    }
}