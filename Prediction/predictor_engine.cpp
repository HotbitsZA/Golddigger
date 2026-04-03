#include "Prediction/predictor_engine.h"

#include "Indicators/indicators.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <utility>

namespace
{
    constexpr int kReturnFeatureCount = 5;
    constexpr int kRsiPeriod = 14;
    constexpr int kSma20Period = 20;
    constexpr int kSma50Period = 50;
    constexpr int kAdxPeriod = 14;
    constexpr int kAdxWindow = 28;
    constexpr std::int64_t kHoursPerDay = 24;
    constexpr std::int64_t kDaysPerWeek = 7;

    std::uint64_t current_timestamp_ms()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    std::pair<double, double> cyclical_time_features(std::uint64_t timestampMs)
    {
        const auto timePoint = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
        const auto hoursSinceEpoch = std::chrono::duration_cast<std::chrono::hours>(
                                         timePoint.time_since_epoch())
                                         .count();

        const auto hourOfDay = static_cast<double>(hoursSinceEpoch % kHoursPerDay);
        const auto dayOfWeek = static_cast<double>((hoursSinceEpoch / kHoursPerDay) % kDaysPerWeek);
        return {hourOfDay, dayOfWeek};
    }
}

PredictorEngine::PredictorEngine(std::unique_ptr<ICandleDataProvider> provider, PredictorConfig config)
    : m_provider(std::move(provider)),
      m_config(std::move(config))
{
}

int PredictorEngine::run()
{
    if (!loadModel())
        return 1;

    if (!bootstrap())
        return 1;

    std::size_t predictionCount = 0;
    if (!emitPrediction())
        return 1;

    ++predictionCount;
    if ((m_config.maxPredictions != 0) && (predictionCount >= m_config.maxPredictions))
        return 0;

    while (true)
    {
        const auto readyAt = nextAvailabilityTime();
        if (std::chrono::system_clock::now() < readyAt)
            std::this_thread::sleep_until(readyAt);

        bool receivedNewCandle = false;
        while (!receivedNewCandle)
        {
            const auto requestToTimestampMs = latestAvailableUpperBoundMs(current_timestamp_ms());
            if (requestToTimestampMs <= nextFetchFromTimestampMs())
            {
                std::this_thread::sleep_for(m_config.pollInterval);
                continue;
            }

            const CandleFetchRequest request{
                m_config.instrument,
                m_config.timeframe,
                nextFetchFromTimestampMs(),
                requestToTimestampMs,
                true};

            auto fetchedCandles = m_provider->fetchCandles(request);
            normalizeCandles(fetchedCandles);

            for (const auto &candle : fetchedCandles)
            {
                if (!m_candles.empty() && (candle.timestamp <= m_candles.back().timestamp))
                    continue;

                m_candles.push_back(candle);
                trimHistory();
                receivedNewCandle = true;

                if (!emitPrediction())
                    return 1;

                ++predictionCount;
                if ((m_config.maxPredictions != 0) && (predictionCount >= m_config.maxPredictions))
                    return 0;
            }

            if (!receivedNewCandle)
                std::this_thread::sleep_for(m_config.pollInterval);
        }
    }
}

bool PredictorEngine::loadModel()
{
    try
    {
        dlib::deserialize(m_config.modelFile) >> m_decisionFunction >> m_normalizer;
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error loading model or normalizer from " << m_config.modelFile
                  << ": " << e.what() << '\n';
        return false;
    }
}

bool PredictorEngine::bootstrap()
{
    const auto intervalMs = static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
    const auto upperBoundMs = latestAvailableUpperBoundMs(current_timestamp_ms());
    const auto lookbackMs = intervalMs * static_cast<std::uint64_t>(m_config.bootstrapCandles);

    const CandleFetchRequest request{
        m_config.instrument,
        m_config.timeframe,
        (upperBoundMs > lookbackMs) ? (upperBoundMs - lookbackMs) : 0,
        upperBoundMs,
        true};

    m_candles = m_provider->fetchCandles(request);
    normalizeCandles(m_candles);
    trimHistory();

    if (m_candles.size() < m_config.minimumCandles)
    {
        std::cerr << "Not enough candles returned from provider. Need at least "
                  << m_config.minimumCandles << ", received " << m_candles.size() << ".\n";
        return false;
    }

    std::cout << "Loaded " << m_candles.size() << " candles for "
              << m_config.instrument << ' ' << to_string(m_config.timeframe)
              << ". Latest closed candle: " << format_local_timestamp(m_candles.back().timestamp) << '\n';

    return true;
}

bool PredictorEngine::emitPrediction() const
{
    if (m_candles.size() < m_config.minimumCandles)
    {
        std::cerr << "Cannot predict yet; only " << m_candles.size()
                  << " candles are available.\n";
        return false;
    }

    sample_type sample;
    for (int j = 0; j < kReturnFeatureCount; ++j)
        sample(j) = std::log(m_candles[m_candles.size() - 1 - j].close /
                             m_candles[m_candles.size() - 1 - j - 1].close);

    std::vector<double> closes;
    closes.reserve(kSma50Period);
    for (std::size_t k = m_candles.size() - kSma50Period; k < m_candles.size(); ++k)
        closes.push_back(m_candles[k].close);

    sample(5) = get_rsi(closes, kRsiPeriod);

    std::vector<Candle> adxCandles(m_candles.end() - kAdxWindow, m_candles.end());
    sample(6) = calculate_adx(adxCandles, kAdxPeriod);
    // Relative ATR (Volatility as a percentage of price) to capture volatility without scale issues
    double rawAtr = get_atr(adxCandles, kAdxPeriod);
    sample(7) = (rawAtr > 0.0) ? (rawAtr / m_candles.back().close) : 0.0;
    // Realtive SMA20 to capture short-term trend strength without scale issues
    double rawSma20 = get_sma(closes, kSma20Period);
    sample(8) = (rawSma20 > 0.0) ? ((m_candles.back().close - rawSma20) / m_candles.back().close) : 0.0;
    // Realtive SMA50 to capture long-term trend strength without scale issues
    double rawSma50 = get_sma(closes, kSma50Period);
    sample(9) = (rawSma50 > 0.0) ? ((m_candles.back().close - rawSma50) / m_candles.back().close) : 0.0;
    // If you are using multiple Moving Averages (e.g., a 20-period and a 50-period), you can also add a "Spread" feature:
    // This allows the model to see MA Crossovers in a scale-invariant way.
    sample(10) = (rawSma50 > 0.0) ? (rawSma20 - rawSma50) / m_candles.back().close : 0.0;
    // Advanced Sine/Cosine encoding for hour of day and day of week to capture cyclical patterns
    const auto [hourOfDay, dayOfWeek] = cyclical_time_features(m_candles.back().timestamp);
    double hourAngle = (2 * M_PI * hourOfDay) / 24.0;
    double dayAngle = (2 * M_PI * dayOfWeek) / 7.0;
    sample(11) = std::sin(hourAngle);  // Captures time-of-day position in a cyclical manner
    sample(12) = std::cos(hourAngle); // Captures proximity of 23:00 to 00:00 and 12:00 to 00:00
    sample(13) = std::sin(dayAngle);  // Captures day-of-week position in a cyclical manner
    sample(14) = std::cos(dayAngle);  // Captures proximity of Sunday to Monday and Wednesday to Sunday

    sample = m_normalizer(sample);

    const double predictedLogReturn = m_decisionFunction(sample);
    const double predictedPrice = m_candles.back().close * std::exp(predictedLogReturn);
    const double predictedChange = m_candles.back().close * (std::exp(predictedLogReturn) - 1.0);
    const double predictedPercentChange = (std::exp(predictedLogReturn) - 1.0) * 100.0;

    std::cout << '[' << format_local_timestamp(m_candles.back().timestamp) << "] "
              << "close=" << m_candles.back().close
              << " predicted_next_price=" << predictedPrice
              << " predicted_log_return=" << predictedLogReturn
              << " predicted_change_pct=" << predictedPercentChange << "%\n";

    // ACTION: If the predicted chnage is greater than the estimated spread, ACTION is STRONG BUY, if it's less than negative estimated spread, ACTION is STRONG SELL, otherwise HOLD.
    const std::string action = (predictedChange > m_config.estimatedSpread) ? "STRONG BUY" : ((predictedChange < -m_config.estimatedSpread) ? "STRONG SELL" : "HOLD");
    std::cout << "ACTION: " << action << "\n\n";
    return true;
}

void PredictorEngine::normalizeCandles(std::vector<Candle> &candles) const
{
    std::sort(candles.begin(), candles.end(),
              [](const Candle &lhs, const Candle &rhs)
              {
                  return lhs.timestamp < rhs.timestamp;
              });

    candles.erase(
        std::unique(candles.begin(), candles.end(),
                    [](const Candle &lhs, const Candle &rhs)
                    {
                        return lhs.timestamp == rhs.timestamp;
                    }),
        candles.end());
}

void PredictorEngine::trimHistory()
{
    if (m_candles.size() <= m_config.historyRetention)
        return;

    m_candles.erase(m_candles.begin(),
                    m_candles.end() - static_cast<std::ptrdiff_t>(m_config.historyRetention));
}

std::uint64_t PredictorEngine::latestAvailableUpperBoundMs(std::uint64_t nowTimestampMs) const
{
    const auto intervalMs = static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
    const auto delayMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(m_config.availabilityDelay).count());

    if (nowTimestampMs <= delayMs)
        return 0;

    const auto effectiveTimestampMs = nowTimestampMs - delayMs;
    return (effectiveTimestampMs / intervalMs) * intervalMs;
}

std::uint64_t PredictorEngine::nextFetchFromTimestampMs() const
{
    return m_candles.back().timestamp +
           static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
}

std::chrono::system_clock::time_point PredictorEngine::nextAvailabilityTime() const
{
    const auto intervalMs = static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
    const auto availabilityDelayMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(m_config.availabilityDelay).count());
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds(nextFetchFromTimestampMs() + intervalMs + availabilityDelayMs)};
}
