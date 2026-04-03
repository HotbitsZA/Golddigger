#pragma once

#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cstdint>

enum class Price_Direction : uint8_t
{
    UP,
    DOWN
};

struct Candle
{
    uint64_t timestamp;
    double open, high, low, close, volume;
};

// 1. Simple Moving Average (SMA)
double get_sma(const std::vector<double> &data, int period);

// 2. Relative Strength Index (RSI) - Wilder's Smoothing
double get_rsi(const std::vector<double> &data, int period);

// 3. Average True Range (ATR)
double get_atr(const std::vector<Candle> &candles, int period);

// 4. Fibonacci Retracement Levels
struct FibLevels
{
    double p23, p38, p50, p61, p78;
};
FibLevels get_fibs(double high, double low, Price_Direction dir = Price_Direction::UP);

// 5. Average Directional Index (ADX) IMPLEMENTATION (14-period default) ---
double calculate_adx(const std::vector<Candle>& candles, int period = 14);