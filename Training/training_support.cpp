#include "Training/training_support.h"

#include "Utils/utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace
{
    constexpr std::size_t kMinCandlesForTraining = 51;
    constexpr std::size_t kTrainingStartIndex = 50;
    constexpr int kReturnFeatureCount = 5;
    constexpr int kRsiPeriod = 14;
    constexpr int kSma20Period = 20;
    constexpr int kSma50Period = 50;
    constexpr int kAdxPeriod = 14;
    constexpr int kAdxWindow = 28;
    constexpr std::int64_t kHoursPerDay = 24;
    constexpr std::int64_t kDaysPerWeek = 7;

    std::string lower_copy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::tolower(ch));
                       });
        return value;
    }

    std::string stem_or_default(const std::string &path, const std::string &fallback)
    {
        const auto stem = fs::path(path).stem().string();
        return stem.empty() ? fallback : stem;
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

    std::string infer_timeframe_tag(const std::string &dataFile)
    {
        const auto normalized = lower_copy(stem_or_default(dataFile, "default"));
        for (const auto &tag : {"m15", "h1", "d1"})
        {
            if (normalized.find(tag) != std::string::npos)
                return tag;
        }

        return normalized;
    }
}

TrainingHyperparameters default_training_hyperparameters()
{
    return {};
}

TrainingDataset load_training_dataset(const std::string &dataFile)
{
    std::vector<Candle> candles;
    read_gold_data(dataFile, candles);

    TrainingDataset dataset;
    dataset.candleCount = candles.size();

    if (candles.size() < kMinCandlesForTraining)
        throw std::runtime_error("Not enough candles in " + dataFile + ". Need at least 51.");

    dataset.samples.reserve(candles.size());
    dataset.labels.reserve(candles.size());

    for (std::size_t i = kTrainingStartIndex; i < candles.size() - 1; ++i)
    {
        sample_type sample;

        for (int j = 0; j < kReturnFeatureCount; ++j)
            sample(j) = std::log(candles[i - j].close / candles[i - j - 1].close);

        std::vector<double> closes;
        closes.reserve(kSma50Period + 1);
        for (std::size_t k = i - kSma50Period; k <= i; ++k)
            closes.push_back(candles[k].close);

        sample(5) = get_rsi(closes, kRsiPeriod);

        std::vector<Candle> adxCandles(candles.begin() + static_cast<std::ptrdiff_t>(i - kAdxWindow),
                                       candles.begin() + static_cast<std::ptrdiff_t>(i));
        sample(6) = calculate_adx(adxCandles, kAdxPeriod);

        const double rawAtr = get_atr(adxCandles, kAdxPeriod);
        sample(7) = (rawAtr > 0.0) ? (rawAtr / candles[i].close) : 0.0;

        const double rawSma20 = get_sma(closes, kSma20Period);
        sample(8) = (rawSma20 > 0.0) ? ((candles[i].close - rawSma20) / candles[i].close) : 0.0;

        const double rawSma50 = get_sma(closes, kSma50Period);
        sample(9) = (rawSma50 > 0.0) ? ((candles[i].close - rawSma50) / candles[i].close) : 0.0;
        sample(10) = (rawSma50 > 0.0) ? (rawSma20 - rawSma50) / candles[i].close : 0.0;

        const auto [hourOfDay, dayOfWeek] = cyclical_time_features(candles[i].timestamp);
        const double hourAngle = (2 * M_PI * hourOfDay) / 24.0;
        const double dayAngle = (2 * M_PI * dayOfWeek) / 7.0;
        sample(11) = std::sin(hourAngle);
        sample(12) = std::cos(hourAngle);
        sample(13) = std::sin(dayAngle);
        sample(14) = std::cos(dayAngle);

        dataset.samples.push_back(sample);
        dataset.labels.push_back(std::log(candles[i + 1].close / candles[i].close));
    }

    if (dataset.samples.empty())
        throw std::runtime_error("No training samples could be generated from " + dataFile + ".");

    return dataset;
}

std::string build_default_tuner_path(const std::string &dataFile)
{
    return (fs::path("Models") / ("tuner_" + infer_timeframe_tag(dataFile) + ".dat")).string();
}

TrainingHyperparameters load_tuner_parameters(const std::string &path)
{
    TrainingHyperparameters parameters = default_training_hyperparameters();
    dlib::deserialize(path) >> parameters;
    return parameters;
}

TrainingHyperparameters load_tuner_parameters_for_data_file(const std::string &dataFile)
{
    const auto path = build_default_tuner_path(dataFile);
    if (!fs::exists(path))
    {
        const auto legacyPath = (fs::path("Models") / ("tunner_" + infer_timeframe_tag(dataFile) + ".dat")).string();
        if (!fs::exists(legacyPath))
            return default_training_hyperparameters();

        return load_tuner_parameters(legacyPath);
    }

    return load_tuner_parameters(path);
}

void save_tuner_parameters(const std::string &path, const TrainingHyperparameters &parameters)
{
    const auto directory = fs::path(path).parent_path();
    if (!directory.empty())
        fs::create_directories(directory);

    dlib::serialize(path) << parameters;
}
