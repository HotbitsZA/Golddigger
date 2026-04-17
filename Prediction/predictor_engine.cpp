#include "Prediction/predictor_engine.h"

#include "Indicators/indicators.h"
#include "Utils/utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
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
    constexpr std::uint64_t kMillisecondsPerDay = 24ULL * 60ULL * 60ULL * 1000ULL;
    constexpr std::size_t kBootstrapExpansionMultiplier = 2;
    constexpr std::size_t kMaxBootstrapExpansionAttempts = 6;
    constexpr std::size_t kRecentOverlayLookbackCandles = 4;
    constexpr std::size_t kM15PredictionHorizonCandles = 6;
    constexpr std::size_t kH1AndD1PredictionHorizonCandles = 4;

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

    std::string format_local_interval(std::uint64_t startTimestampMs, std::uint64_t endTimestampMs)
    {
        return format_local_timestamp(startTimestampMs) + " -> " + format_local_timestamp(endTimestampMs);
    }

    std::string format_utc_interval(std::uint64_t startTimestampMs, std::uint64_t endTimestampMs)
    {
        return format_utc_timestamp(startTimestampMs) + " -> " + format_utc_timestamp(endTimestampMs);
    }

    std::string lower_copy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::tolower(ch));
                       });
        return value;
    }

    std::string default_cached_data_path(const PredictorConfig &config)
    {
        return (std::filesystem::path("Data") /
                (lower_copy(config.instrument) + "-" + to_string(config.timeframe) + "-bid.csv"))
            .string();
    }

    void keep_last_candles(std::vector<Candle> &candles, std::size_t count)
    {
        if (candles.size() <= count)
            return;

        candles.erase(candles.begin(), candles.end() - static_cast<std::ptrdiff_t>(count));
    }

    std::uint64_t utc_day_start_ms(std::uint64_t timestampMs)
    {
        return (timestampMs / kMillisecondsPerDay) * kMillisecondsPerDay;
    }

    std::uint64_t recent_overlay_from_ms(
        CandleTimeframe timeframe,
        std::uint64_t upperBoundMs,
        std::uint64_t intervalMs)
    {
        if (timeframe == CandleTimeframe::M15)
            return utc_day_start_ms(upperBoundMs);

        const auto lookbackMs = intervalMs * static_cast<std::uint64_t>(kRecentOverlayLookbackCandles);
        return (upperBoundMs > lookbackMs) ? (upperBoundMs - lookbackMs) : 0ULL;
    }

    std::size_t prediction_horizon_candles(CandleTimeframe timeframe)
    {
        return (timeframe == CandleTimeframe::M15) ? kM15PredictionHorizonCandles
                                                   : kH1AndD1PredictionHorizonCandles;
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
            const auto intervalMs = static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
            const auto nextFromTimestampMs = nextFetchFromTimestampMs();
            const auto nextToTimestampMs = nextFromTimestampMs + intervalMs;
            const auto latestAvailableTimestampMs = latestAvailableUpperBoundMs(current_timestamp_ms());
            if (latestAvailableTimestampMs <= nextFromTimestampMs)
            {
                const auto retryAfterTimestampMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        (std::chrono::system_clock::now() + m_config.pollInterval).time_since_epoch())
                        .count());
                std::cout << "Next candle "
                          << format_local_interval(nextFromTimestampMs, nextToTimestampMs)
                          << " local is not available yet. Latest safe boundary is "
                          << format_local_timestamp(latestAvailableTimestampMs)
                          << " local (" << format_utc_timestamp(latestAvailableTimestampMs) << " UTC)"
                          << ". Retrying after " << format_local_timestamp(retryAfterTimestampMs)
                          << " local.\n";
                std::this_thread::sleep_for(m_config.pollInterval);
                continue;
            }

            const auto requestToTimestampMs = latestAvailableTimestampMs;
            const auto requestFromTimestampMs =
                recent_overlay_from_ms(m_config.timeframe, requestToTimestampMs, intervalMs);

            const CandleFetchRequest request{
                m_config.instrument,
                m_config.timeframe,
                requestFromTimestampMs,
                requestToTimestampMs,
                true};

            std::vector<Candle> fetchedCandles;
            std::cout << "Requesting live candles with command: "
                      << m_provider->describeRequest(request) << '\n';
            try
            {
                fetchedCandles = m_provider->fetchCandles(request);
                normalizeCandles(fetchedCandles);
                std::cout << "Live candle request succeeded: fetched "
                          << fetchedCandles.size() << " candle(s) for "
                          << format_local_interval(requestFromTimestampMs, requestToTimestampMs)
                          << " local.\n";
            }
            catch (const std::exception &e)
            {
                std::cerr << "Live candle request failed for "
                          << m_config.instrument << ' ' << to_string(m_config.timeframe)
                          << ": " << e.what() << '\n';
                std::this_thread::sleep_for(m_config.pollInterval);
                continue;
            }

            std::size_t appendedCandles = 0;
            for (const auto &candle : fetchedCandles)
            {
                if (!m_candles.empty() && (candle.timestamp <= m_candles.back().timestamp))
                    continue;

                m_candles.push_back(candle);
                trimHistory();
                receivedNewCandle = true;
                ++appendedCandles;

                if (!emitPrediction())
                    return 1;

                ++predictionCount;
                if ((m_config.maxPredictions != 0) && (predictionCount >= m_config.maxPredictions))
                    return 0;
            }

            if (!receivedNewCandle)
            {
                if (!fetchedCandles.empty())
                {
                    const auto newestFetchedStartMs = fetchedCandles.back().timestamp;
                    const auto newestFetchedEndMs = newestFetchedStartMs + intervalMs;
                    std::cout << "Live candle request returned no newer candle. Newest fetched candle was "
                              << format_local_interval(newestFetchedStartMs, newestFetchedEndMs)
                              << " local. Retrying in "
                              << std::chrono::duration_cast<std::chrono::seconds>(m_config.pollInterval).count()
                              << "s.\n";
                }
                else
                {
                    std::cout << "Live candle request returned no candles. Retrying in "
                              << std::chrono::duration_cast<std::chrono::seconds>(m_config.pollInterval).count()
                              << "s.\n";
                }
                std::this_thread::sleep_for(m_config.pollInterval);
            }
            else
            {
                std::cout << "Appended " << appendedCandles << " new candle(s) from live fetch.\n";
            }
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
    bool usedCachedHistory = false;
    bool overlayApplied = false;
    try
    {
        const auto intervalMs = static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
        const auto upperBoundMs = latestAvailableUpperBoundMs(current_timestamp_ms());
        const auto requiredCandles = requiredHistoryCandles();
        const auto bootstrapTargetCandles =
            std::max<std::size_t>({m_config.historyRetention, m_config.bootstrapCandles, requiredCandles});

        if (upperBoundMs == 0)
        {
            std::cerr << "No completed candles are available yet for "
                      << m_config.instrument << ' ' << to_string(m_config.timeframe)
                      << ".\n";
            return false;
        }

        m_candles.clear();
        const auto cachedDataPath = default_cached_data_path(m_config);
        if (std::filesystem::exists(cachedDataPath))
        {
            std::vector<Candle> cachedCandles;
            read_gold_data(cachedDataPath, cachedCandles);
            cachedCandles.erase(
                std::remove_if(cachedCandles.begin(), cachedCandles.end(),
                               [upperBoundMs](const Candle &candle)
                               {
                                   return candle.timestamp >= upperBoundMs;
                               }),
                cachedCandles.end());
            normalizeCandles(cachedCandles);
            keep_last_candles(cachedCandles, bootstrapTargetCandles);
            m_candles = std::move(cachedCandles);
            usedCachedHistory = !m_candles.empty();
        }

        const CandleFetchRequest recentOverlayRequest{
            m_config.instrument,
            m_config.timeframe,
            recent_overlay_from_ms(m_config.timeframe, upperBoundMs, intervalMs),
            upperBoundMs,
            true};

        try
        {
            auto recentCandles = m_provider->fetchCandles(recentOverlayRequest);
            if (!recentCandles.empty())
            {
                m_candles.insert(m_candles.end(), recentCandles.begin(), recentCandles.end());
                normalizeCandles(m_candles);
                overlayApplied = true;
            }
        }
        catch (const std::exception &e)
        {
            if (m_candles.size() < requiredCandles)
                throw;

            std::cerr << "Warning: recent " << m_provider->providerName()
                      << " overlay failed during bootstrap, continuing with cached data: "
                      << e.what() << '\n';
        }

        if (m_candles.size() < requiredCandles)
        {
            auto lookbackCandles = std::max(m_config.bootstrapCandles, requiredCandles * 2);
            std::cout << "Cached history is insufficient; requesting additional live history from "
                      << m_provider->providerName() << "...\n";
            for (std::size_t attempt = 0; attempt < kMaxBootstrapExpansionAttempts; ++attempt)
            {
                const auto lookbackMs = intervalMs * static_cast<std::uint64_t>(lookbackCandles);
                const CandleFetchRequest request{
                    m_config.instrument,
                    m_config.timeframe,
                    (upperBoundMs > lookbackMs) ? (upperBoundMs - lookbackMs) : 0,
                    upperBoundMs,
                    true};

                auto fetchedCandles = m_provider->fetchCandles(request);
                normalizeCandles(fetchedCandles);
                if (!fetchedCandles.empty())
                {
                    if (m_candles.empty())
                        m_candles = std::move(fetchedCandles);
                    else
                    {
                        m_candles.insert(m_candles.end(), fetchedCandles.begin(), fetchedCandles.end());
                        normalizeCandles(m_candles);
                    }
                }

                if (m_candles.size() >= requiredCandles)
                    break;

                if (request.fromTimestampMs == 0)
                    break;

                lookbackCandles *= kBootstrapExpansionMultiplier;
            }
        }

        keep_last_candles(m_candles, bootstrapTargetCandles);
        trimHistory();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error bootstrapping candles for "
                  << m_config.instrument << ' ' << to_string(m_config.timeframe)
                  << ": " << e.what() << '\n';
        return false;
    }

    const auto requiredCandles = requiredHistoryCandles();
    if (m_candles.size() < requiredCandles)
    {
        std::cerr << "Not enough candles returned from provider. Need at least "
                  << requiredCandles << ", received " << m_candles.size() << ".\n";
        return false;
    }

    const auto intervalMs = static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
    const auto latestCandleStartMs = m_candles.back().timestamp;
    const auto latestCandleEndMs = latestCandleStartMs + intervalMs;
    std::string sourceSummary;
    if (usedCachedHistory && overlayApplied)
        sourceSummary = " (cache + " + m_provider->providerName() + " overlay)";
    else if (usedCachedHistory)
        sourceSummary = " (cache)";
    else if (overlayApplied)
        sourceSummary = " (" + m_provider->providerName() + ")";
    std::cout << "Loaded " << m_candles.size() << " candles for "
              << m_config.instrument << ' ' << to_string(m_config.timeframe)
              << sourceSummary
              << ". Latest closed candle: "
              << format_local_interval(latestCandleStartMs, latestCandleEndMs)
              << " local (" << format_utc_interval(latestCandleStartMs, latestCandleEndMs)
              << " UTC)"
              << " (required history: " << requiredCandles << " candles)\n";

    return true;
}

bool PredictorEngine::emitPrediction() const
{
    const auto requiredCandles = requiredHistoryCandles();
    if (m_candles.size() < requiredCandles)
    {
        std::cerr << "Cannot predict yet; only " << m_candles.size()
                  << " candles are available. Need at least " << requiredCandles << ".\n";
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
    const double predictedExtremePrice = m_candles.back().close * std::exp(predictedLogReturn);
    const double predictedChange = m_candles.back().close * (std::exp(predictedLogReturn) - 1.0);
    const double predictedPercentChange = (std::exp(predictedLogReturn) - 1.0) * 100.0;
    const auto intervalMs = static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
    const auto currentCandleStartMs = m_candles.back().timestamp;
    const auto currentCandleEndMs = currentCandleStartMs + intervalMs;
    const auto horizonCandles = prediction_horizon_candles(m_config.timeframe);
    const auto predictionHorizonEndMs =
        currentCandleEndMs + (intervalMs * static_cast<std::uint64_t>(horizonCandles));

    std::cout << '['
              << format_local_interval(currentCandleStartMs, currentCandleEndMs)
              << " local | "
              << format_utc_interval(currentCandleStartMs, currentCandleEndMs)
              << " UTC] "
              << "close=" << m_candles.back().close
              << " predicted_horizon_extreme_price=" << predictedExtremePrice
              << " predicted_horizon_log_return=" << predictedLogReturn
              << " predicted_horizon_change_pct=" << predictedPercentChange << "%\n";

    std::cout << "Prediction horizon: next " << horizonCandles << " candle(s) -> "
              << format_local_interval(currentCandleEndMs, predictionHorizonEndMs)
              << " local (" << format_utc_interval(currentCandleEndMs, predictionHorizonEndMs) << " UTC)\n";

    // ACTION: If the predicted horizon move is greater than the estimated spread, ACTION is STRONG BUY, if it's less than negative estimated spread, ACTION is STRONG SELL, otherwise HOLD.
    const std::string action = (predictedChange > m_config.estimatedSpread) ? "STRONG BUY" : ((predictedChange < -m_config.estimatedSpread) ? "STRONG SELL" : "HOLD");
    std::cout << "ACTION: " << action << '\n';
    if (action != "HOLD")
    {
        const bool isLong = (action == "STRONG BUY");
        std::cout << "Exit plan: "
                  << (isLong ? "take profit if price reaches or exceeds " : "take profit if price reaches or falls below ")
                  << predictedExtremePrice << " before "
                  << format_local_timestamp(predictionHorizonEndMs)
                  << " local (" << format_utc_timestamp(predictionHorizonEndMs) << " UTC)"
                  << ". Otherwise exit at the horizon deadline.\n";
        std::cout << "Exit deadline: "
                  << format_local_interval(currentCandleEndMs, predictionHorizonEndMs)
                  << " local (" << format_utc_interval(currentCandleEndMs, predictionHorizonEndMs) << " UTC)\n";
    }
    const auto nextCandleTimestampMs = nextFetchFromTimestampMs();
    const auto nextCandleEndTimestampMs = nextCandleTimestampMs + intervalMs;
    const auto nextAvailabilityTimestampMs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       nextAvailabilityTime().time_since_epoch())
                                       .count());
    std::cout << "Next expected candle: "
              << format_local_interval(nextCandleTimestampMs, nextCandleEndTimestampMs)
              << " local (" << format_utc_interval(nextCandleTimestampMs, nextCandleEndTimestampMs) << " UTC)"
              << " | next request after "
              << format_local_timestamp(nextAvailabilityTimestampMs)
              << " local (" << format_utc_timestamp(nextAvailabilityTimestampMs) << " UTC)\n\n";
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

std::size_t PredictorEngine::requiredHistoryCandles() const noexcept
{
    const auto indicatorMinimum = std::max<std::size_t>(
        {static_cast<std::size_t>(kSma50Period),
         static_cast<std::size_t>(kAdxWindow),
         static_cast<std::size_t>(kReturnFeatureCount + 1)});
    return std::max(m_config.minimumCandles, indicatorMinimum);
}

std::uint64_t PredictorEngine::latestAvailableUpperBoundMs(std::uint64_t nowTimestampMs) const
{
    const auto intervalMs = static_cast<std::uint64_t>(candle_interval(m_config.timeframe).count());
    const auto delayMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(m_config.availabilityDelay).count());

    if ((nowTimestampMs <= delayMs) || (intervalMs == 0))
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
    const auto publicationIntervalMs = static_cast<std::uint64_t>(
        m_provider->livePublicationInterval(m_config.timeframe).count());
    const auto availabilityDelayMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(m_config.availabilityDelay).count());
    const auto nextCandleEndMs = nextFetchFromTimestampMs() + intervalMs;
    const auto freshnessIntervalMs = (publicationIntervalMs == 0) ? intervalMs : publicationIntervalMs;
    const auto publicationBoundaryMs =
        ((nextCandleEndMs + freshnessIntervalMs - 1) / freshnessIntervalMs) * freshnessIntervalMs;

    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds(publicationBoundaryMs + availabilityDelayMs)};
}
