#include "indicators.h"

// 1. Simple Moving Average (SMA)
double get_sma(const std::vector<double> &data, int period)
{
    if (period <= 0 || data.size() < static_cast<std::size_t>(period))
        return 0.0;
    double sum = std::accumulate(data.end() - period, data.end(), 0.0);
    return sum / period;
}

// 2. Relative Strength Index (RSI) - Wilder's Smoothing
double get_rsi(const std::vector<double> &data, int period)
{
    if (period <= 0 || data.size() <= static_cast<std::size_t>(period))
        return 50.0;

    // Seed the smoothing with the mean gain/loss over the first `period` changes.
    const std::size_t start = data.size() - static_cast<std::size_t>(period) - 1;
    double gain = 0.0;
    double loss = 0.0;
    for (std::size_t i = start + 1; i <= start + static_cast<std::size_t>(period); ++i)
    {
        const double diff = data[i] - data[i - 1];
        if (diff >= 0.0)
            gain += diff;
        else
            loss -= diff;
    }

    double avgGain = gain / period;
    double avgLoss = loss / period;

    // Wilder's smoothing over any remaining points.
    for (std::size_t i = start + static_cast<std::size_t>(period) + 1; i < data.size(); ++i)
    {
        const double diff = data[i] - data[i - 1];
        const double currentGain = (diff > 0.0) ? diff : 0.0;
        const double currentLoss = (diff < 0.0) ? -diff : 0.0;
        avgGain = (avgGain * (period - 1) + currentGain) / period;
        avgLoss = (avgLoss * (period - 1) + currentLoss) / period;
    }

    // A flat series has no gains and no losses: neutral RSI instead of a fake 100.
    if (avgLoss == 0.0)
        return (avgGain == 0.0) ? 50.0 : 100.0;

    const double rs = avgGain / avgLoss;
    return 100.0 - (100.0 / (1.0 + rs));
}

// 3. Average True Range (ATR)
double get_atr(const std::vector<Candle> &candles, int period)
{
    if (period <= 0 || candles.size() <= static_cast<std::size_t>(period))
        return 0.0;
    double tr_sum = 0;
    for (size_t i = candles.size() - static_cast<std::size_t>(period); i < candles.size(); ++i)
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
    if (period <= 0 || candles.size() < static_cast<std::size_t>(period) * 2)
        return 0.0;

    const std::size_t count = candles.size() - 1; // Number of TR / ±DM / DX values.
    std::vector<double> tr(count);
    std::vector<double> plusDm(count);
    std::vector<double> minusDm(count);

    for (std::size_t i = 1; i < candles.size(); ++i)
    {
        const std::size_t idx = i - 1;
        tr[idx] = std::max({candles[i].high - candles[i].low,
                            std::abs(candles[i].high - candles[i - 1].close),
                            std::abs(candles[i].low - candles[i - 1].close)});

        const double upMove = candles[i].high - candles[i - 1].high;
        const double downMove = candles[i - 1].low - candles[i].low;
        plusDm[idx] = (upMove > downMove && upMove > 0.0) ? upMove : 0.0;
        minusDm[idx] = (downMove > upMove && downMove > 0.0) ? downMove : 0.0;
    }

    // Wilder's smoothing of ATR and +DI / -DI, seeded with a mean over the first period.
    const std::size_t warm = static_cast<std::size_t>(period);
    double smoothAtr = 0.0;
    double smoothPlusDm = 0.0;
    double smoothMinusDm = 0.0;
    for (std::size_t i = 0; i < warm; ++i)
    {
        smoothAtr += tr[i];
        smoothPlusDm += plusDm[i];
        smoothMinusDm += minusDm[i];
    }

    if (smoothAtr <= 0.0)
        return 0.0;

    smoothAtr /= period;
    smoothPlusDm /= period;
    smoothMinusDm /= period;

    auto directionalIndex = [&](double &dxValue)
    {
        const double plusDi = 100.0 * (smoothPlusDm / smoothAtr);
        const double minusDi = 100.0 * (smoothMinusDm / smoothAtr);
        const double diSum = plusDi + minusDi;
        dxValue = (diSum > 0.0) ? 100.0 * std::abs(plusDi - minusDi) / diSum : 0.0;
    };

    std::vector<double> dx(count, 0.0);
    directionalIndex(dx[warm - 1]);

    for (std::size_t i = warm; i < count; ++i)
    {
        smoothAtr = (smoothAtr * (period - 1) + tr[i]) / period;
        smoothPlusDm = (smoothPlusDm * (period - 1) + plusDm[i]) / period;
        smoothMinusDm = (smoothMinusDm * (period - 1) + minusDm[i]) / period;
        directionalIndex(dx[i]);
    }

    // ADX is the mean of the DX values within the smoothing window.
    const std::size_t dxCount = count - warm + 1;
    const std::size_t adxWindow = std::min(warm, dxCount);
    double adx = 0.0;
    for (std::size_t i = count - adxWindow; i < count; ++i)
        adx += dx[i];

    return adx / adxWindow;
}
