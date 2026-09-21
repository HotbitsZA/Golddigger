#include "Training/training_support.h"

#include "Features/feature_builder.h"
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

    std::size_t label_horizon_candles(const std::string &dataFile)
    {
        const auto timeframeTag = infer_timeframe_tag(dataFile);
        if (timeframeTag == "m15")
            return feature_builder::kM15HorizonCandles;

        return feature_builder::kDefaultHorizonCandles;
    }

    double get_max_horizon_move(
        const std::vector<Candle> &candles,
        std::size_t entryIndex,
        std::size_t horizonCandles)
    {
        const double entryClose = candles[entryIndex].close;
        double highestHigh = candles[entryIndex + 1].high;
        double lowestLow = candles[entryIndex + 1].low;

        for (std::size_t offset = 1; offset <= horizonCandles; ++offset)
        {
            const Candle &futureCandle = candles[entryIndex + offset];
            highestHigh = std::max(highestHigh, futureCandle.high);
            lowestLow = std::min(lowestLow, futureCandle.low);
        }

        const double maxBullishMove = std::log(highestHigh / entryClose);
        const double maxBearishMove = std::log(lowestLow / entryClose);
        return (std::abs(maxBullishMove) >= std::abs(maxBearishMove))
                   ? maxBullishMove
                   : maxBearishMove;
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

    const auto horizonCandles = label_horizon_candles(dataFile);
    const auto minCandlesForTraining = feature_builder::kFeatureStartIndex + horizonCandles + 1;
    if (candles.size() < minCandlesForTraining)
        throw std::runtime_error(
            "Not enough candles in " + dataFile + ". Need at least " + std::to_string(minCandlesForTraining) + '.');

    dataset.samples.reserve(candles.size());
    dataset.labels.reserve(candles.size());

    for (std::size_t i = feature_builder::kFeatureStartIndex; (i + horizonCandles) < candles.size(); ++i)
    {
        dataset.samples.push_back(feature_builder::build_feature_sample(candles, i));
        dataset.labels.push_back(get_max_horizon_move(candles, i, horizonCandles));
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
