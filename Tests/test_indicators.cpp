#include "doctest.h"

#include "Indicators/indicators.h"

#include <cmath>
#include <vector>

TEST_CASE("SMA computes the last-period mean")
{
    REQUIRE(std::abs(get_sma({}, 14) - 0.0) < 1e-12);
    REQUIRE(std::abs(get_sma({1, 2, 3, 4, 5}, 2) - 4.5) < 1e-12);
    REQUIRE(std::abs(get_sma({1, 2, 3, 4, 5}, 5) - 3.0) < 1e-12);
    REQUIRE(std::abs(get_sma({1, 2, 3, 4, 5}, 6) - 0.0) < 1e-12);
}

TEST_CASE("RSI edge cases")
{
    // Nothing to compute with -> neutral.
    REQUIRE(std::abs(get_rsi({}, 14) - 50.0) < 1e-12);
    REQUIRE(std::abs(get_rsi({1, 2, 3}, 5) - 50.0) < 1e-12);

    // All gains -> 100.
    std::vector<double> rising;
    for (int i = 1; i <= 15; ++i)
        rising.push_back(static_cast<double>(i));
    REQUIRE(std::abs(get_rsi(rising, 14) - 100.0) < 1e-9);

    // All losses -> 0.
    std::vector<double> falling(rising.rbegin(), rising.rend());
    REQUIRE(std::abs(get_rsi(falling, 14) - 0.0) < 1e-9);

    // Flat series -> neutral 50 instead of a fake 100.
    std::vector<double> flat(15, 42.0);
    REQUIRE(std::abs(get_rsi(flat, 14) - 50.0) < 1e-9);
}

TEST_CASE("RSI golden value")
{
    // Hand-computed against the Wilder implementation for a 15-point window.
    const std::vector<double> data{44, 49, 52, 50, 48, 51, 53, 47,
                                   55, 50, 54, 52, 56, 60, 58};
    REQUIRE(std::abs(get_rsi(data, 14) - 63.461538) < 1e-3);
}

TEST_CASE("RSI is finite and bounded")
{
    const std::vector<double> data{44, 49, 52, 50, 48, 51, 53, 47,
                                   55, 50, 54, 52, 56, 60, 58, 57, 61};
    const double rsi = get_rsi(data, 14);
    REQUIRE(std::isfinite(rsi));
    REQUIRE(rsi >= 0.0);
    REQUIRE(rsi <= 100.0);
}

TEST_CASE("ATR golden value")
{
    std::vector<Candle> candles{
        Candle{1000, 1.0, 2.0, 1.0, 1.5, 0.0},
        Candle{1010, 3.0, 4.0, 3.0, 3.0, 0.0},
        Candle{1020, 4.5, 5.0, 4.0, 4.5, 0.0},
        Candle{1030, 6.5, 7.0, 6.0, 6.5, 0.0},
    };
    // TRs: 2.5, 2.0, 2.5 -> mean 7/3.
    REQUIRE(std::abs(get_atr(candles, 3) - (2.5 + 2.0 + 2.5) / 3.0) < 1e-9);
    REQUIRE(std::abs(get_atr(candles, 5) - 0.0) < 1e-12);
}

namespace
{
    Candle rangeCandle(std::uint64_t ts, double high, double low)
    {
        return Candle{ts, (high + low) / 2.0, high, low, (high + low) / 2.0, 0.0};
    }
}

TEST_CASE("ADX guard: flat candles never produce NaN")
{
    std::vector<Candle> candles;
    for (int i = 0; i < 30; ++i)
        candles.push_back(rangeCandle(static_cast<std::uint64_t>(i * 60'000), 100.0, 100.0));

    const double adx = calculate_adx(candles, 14);
    REQUIRE(std::isfinite(adx));
    REQUIRE(std::abs(adx) < 1e-9);
}

TEST_CASE("ADX guard: no directional movement never produces NaN")
{
    // Constant range but no up/down directional movement -> TR > 0 but +DI = -DI = 0.
    std::vector<Candle> candles;
    for (int i = 0; i < 20; ++i)
        candles.push_back(rangeCandle(static_cast<std::uint64_t>(i * 60'000), 101.0, 99.0));

    const double adx = calculate_adx(candles, 14);
    REQUIRE(std::isfinite(adx));
    REQUIRE(std::abs(adx) < 1e-9);
}

TEST_CASE("ADX monotonic uptrend rises toward the strong-trend ceiling")
{
    std::vector<Candle> candles;
    for (int i = 0; i < 30; ++i)
        candles.push_back(
            Candle{static_cast<std::uint64_t>(i * 60'000),
                   static_cast<double>(i),
                   static_cast<double>(i) + 1.5,
                   static_cast<double>(i) - 0.5,
                   static_cast<double>(i),
                   0.0});

    const double adx = calculate_adx(candles, 14);
    REQUIRE(std::isfinite(adx));
    CHECK(adx > 50.0);
    REQUIRE(adx <= 100.0);
}

TEST_CASE("ADX too little data returns zero")
{
    std::vector<Candle> candles{rangeCandle(0, 10, 9), rangeCandle(1, 11, 10), rangeCandle(2, 12, 11)};
    REQUIRE(std::abs(calculate_adx(candles, 14) - 0.0) < 1e-12);
}