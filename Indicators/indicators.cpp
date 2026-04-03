#include "indicators.h"

// 1. Simple Moving Average (SMA)
double get_sma(const std::vector<double> &data, int period)
{
    if (data.size() < period)
        return 0.0;
    double sum = std::accumulate(data.end() - period, data.end(), 0.0);
    return sum / period;
}

// 2. Relative Strength Index (RSI) - Wilder's Smoothing
double get_rsi(const std::vector<double> &data, int period)
{
    if (data.size() <= period)
        return 50.0;
    double avg_gain = 0, avg_loss = 0;
    for (size_t i = data.size() - period; i < data.size(); ++i)
    {
        double diff = data[i] - data[i - 1];
        if (diff >= 0)
            avg_gain += diff;
        else
            avg_loss -= diff;
    }
    avg_gain /= period;
    avg_loss /= period;
    if (avg_loss == 0)
        return 100.0;
    double rs = avg_gain / avg_loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

// 3. Average True Range (ATR)
double get_atr(const std::vector<Candle> &candles, int period)
{
    if (candles.size() <= period)
        return 0.0;
    double tr_sum = 0;
    for (size_t i = candles.size() - period; i < candles.size(); ++i)
    {
        double tr = std::max({candles[i].high - candles[i].low,
                              std::abs(candles[i].high - candles[i - 1].close),
                              std::abs(candles[i].low - candles[i - 1].close)});
        tr_sum += tr;
    }
    return tr_sum / period;
}

// 4. Fibonacci Retracement Levels
FibLevels get_fibs(double high, double low, Price_Direction dir)
{
    FibLevels levels;
    double diff = high - low;
    levels.p23 = (dir == Price_Direction::UP) ? high - (diff * 0.236) : low + (diff * 0.236);
    levels.p38 = (dir == Price_Direction::UP) ? high - (diff * 0.382) : low + (diff * 0.382);
    levels.p50 = (dir == Price_Direction::UP) ? high - (diff * 0.5) : low + (diff * 0.5);
    levels.p61 = (dir == Price_Direction::UP) ? high - (diff * 0.618) : low + (diff * 0.618);
    levels.p78 = (dir == Price_Direction::UP) ? high - (diff * 0.786) : low + (diff * 0.786);
    return levels;
}

// 5. Average Directional Index (ADX) IMPLEMENTATION (14-period default) ---
double calculate_adx(const std::vector<Candle> &candles, int period)
{
    if (candles.size() < period * 2)
        return 0.0;

    std::vector<double> tr, plus_dm, minus_dm;
    for (size_t i = 1; i < candles.size(); ++i)
    {
        // True Range (TR)
        double tr_val = std::max({candles[i].high - candles[i].low,
                                  std::abs(candles[i].high - candles[i - 1].close),
                                  std::abs(candles[i].low - candles[i - 1].close)});
        tr.push_back(tr_val);

        // Directional Movement (+DM / -DM)
        double up_move = candles[i].high - candles[i - 1].high;
        double down_move = candles[i - 1].low - candles[i].low;

        plus_dm.push_back((up_move > down_move && up_move > 0) ? up_move : 0);
        minus_dm.push_back((down_move > up_move && down_move > 0) ? down_move : 0);
    }

    // Wilder's Smoothing for ATR, +DI, and -DI
    auto smooth = [&](const std::vector<double> &data)
    {
        double val = 0;
        for (int i = 0; i < period; ++i)
            val += data[i];
        for (size_t i = period; i < data.size(); ++i)
        {
            val = (val - (val / period)) + data[i];
        }
        return val;
    };

    double atr = smooth(tr);
    double plus_di = 100 * (smooth(plus_dm) / atr);
    double minus_di = 100 * (smooth(minus_dm) / atr);

    // Directional Index (DX) and eventually ADX
    return 100 * (std::abs(plus_di - minus_di) / (plus_di + minus_di));
}
