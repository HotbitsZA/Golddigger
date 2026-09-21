#include "Features/feature_builder.h"
#include "Training/model_types.h"
#include "Training/training_support.h"

#include <dlib/svm.h>

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr double kSvrSolverEpsilon = 0.001;
    constexpr const char *kDefaultDataFile = "Data/xauusd-m15-bid.csv";
    constexpr std::size_t kDefaultFirstTrainSamples = 5000;
    constexpr std::size_t kDefaultStepSamples = 2000;
    constexpr std::size_t kDefaultFinalTestSamples = 5000;
    constexpr double kDefaultActionThreshold = 0.4;

    struct Options
    {
        std::string dataFile{kDefaultDataFile};
        std::optional<std::string> tunerFile;
        std::size_t firstTrainSamples{kDefaultFirstTrainSamples};
        std::size_t stepSamples{kDefaultStepSamples};
        std::size_t finalTestSamples{kDefaultFinalTestSamples};
        std::size_t maxTrainSamples{0};
        double actionThreshold{kDefaultActionThreshold};
    };

    struct FoldStats
    {
        std::size_t validationCount{0};
        long supportVectorCount{0};
        double mse{0.0};
        double mae{0.0};
        double signHitRate{0.0};
        double signalRate{0.0};
        double signalHitRate{0.0};
        std::size_t signalCount{0};
    };

    void print_usage()
    {
        std::cout
            << "Usage: validate.bin [--data data.csv] [--tuner tuner.dat]\n"
            << "                  [--first-train N] [--step N] [--final-test N]\n"
            << "                  [--max-train N] [--action-threshold F]\n"
            << "\n"
            << "Runs an expanding-window walk-forward evaluation.\n"
            << "each fold: train on the oldest samples, validate on the next N samples,\n"
            << "then grow the training window by N and repeat. A final model is trained\n"
            << "on everything except the newest --final-test samples and scored on them.\n";
    }

    Options parse_options(int argc, char *argv[])
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            auto nextValue = [&](const char *flag) -> std::string
            {
                if (i + 1 >= argc)
                    throw std::runtime_error(std::string("Missing value for ") + flag);
                return argv[++i];
            };

            if (argument == "--help")
            {
                print_usage();
                std::exit(0);
            }
            else if (argument == "--data")
            {
                options.dataFile = nextValue("--data");
            }
            else if (argument == "--tuner")
            {
                options.tunerFile = nextValue("--tuner");
            }
            else if (argument == "--first-train")
            {
                options.firstTrainSamples = static_cast<std::size_t>(std::stoll(nextValue("--first-train")));
            }
            else if (argument == "--step")
            {
                options.stepSamples = static_cast<std::size_t>(std::stoll(nextValue("--step")));
            }
            else if (argument == "--final-test")
            {
                options.finalTestSamples = static_cast<std::size_t>(std::stoll(nextValue("--final-test")));
            }
            else if (argument == "--max-train")
            {
                options.maxTrainSamples = static_cast<std::size_t>(std::stoll(nextValue("--max-train")));
            }
            else if (argument == "--action-threshold")
            {
                options.actionThreshold = std::stod(nextValue("--action-threshold"));
            }
            else
            {
                throw std::runtime_error("Unknown argument: " + argument);
            }
        }

        if (options.firstTrainSamples == 0 || options.stepSamples == 0)
            throw std::runtime_error("--first-train and --step must be greater than 0");
        if (options.actionThreshold < 0.0)
            throw std::runtime_error("--action-threshold must be non-negative");

        return options;
    }

    dlib::svr_trainer<kernel_type> make_trainer(const TrainingHyperparameters &parameters)
    {
        dlib::svr_trainer<kernel_type> trainer;
        trainer.set_kernel(kernel_type(parameters.gamma));
        trainer.set_c(parameters.c);
        trainer.set_epsilon(kSvrSolverEpsilon);
        trainer.set_cache_size(2000);
        trainer.set_epsilon_insensitivity(parameters.epsilon);
        return trainer;
    }

    FoldStats score_fold(
        const dlib::svr_trainer<kernel_type> &trainer,
        const std::vector<sample_type> &trainSamples,
        const std::vector<double> &trainLabels,
        const std::vector<sample_type> &validationSamples,
        const std::vector<double> &validationLabels,
        double actionThreshold)
    {
        auto normalizedTrain = trainSamples;

        dlib::vector_normalizer<sample_type> normalizer;
        normalizer.train(normalizedTrain);
        for (auto &sample : normalizedTrain)
            sample = normalizer(sample);

        for (const auto &sample : normalizedTrain)
            for (long i = 0; i < sample.size(); ++i)
                if (std::isnan(sample(i)))
                    throw std::runtime_error("NaN value found in training samples after normalization");

        const auto decisionFunction = trainer.train(normalizedTrain, trainLabels);

        FoldStats stats;
        stats.supportVectorCount = decisionFunction.basis_vectors.size();
        stats.validationCount = validationSamples.size();

        double squaredErrorSum = 0.0;
        double absoluteErrorSum = 0.0;
        std::size_t signMatches = 0;
        std::size_t signalMatches = 0;

        for (std::size_t i = 0; i < validationSamples.size(); ++i)
        {
            const double prediction = decisionFunction(normalizer(validationSamples[i]));
            const double label = validationLabels[i];
            const double error = prediction - label;

            squaredErrorSum += error * error;
            absoluteErrorSum += std::abs(error);
            if ((prediction >= 0.0) == (label >= 0.0))
                ++signMatches;

            if (std::abs(prediction) >= actionThreshold)
            {
                ++stats.signalCount;
                if ((prediction >= 0.0) == (label >= 0.0))
                    ++signalMatches;
            }
        }

        if (stats.validationCount == 0)
            return stats;

        const auto count = static_cast<double>(stats.validationCount);
        stats.mse = squaredErrorSum / count;
        stats.mae = absoluteErrorSum / count;
        stats.signHitRate = static_cast<double>(signMatches) / count;
        stats.signalRate = static_cast<double>(stats.signalCount) / count;
        stats.signalHitRate = stats.signalCount > 0
                                  ? static_cast<double>(signalMatches) / static_cast<double>(stats.signalCount)
                                  : 0.0;
        return stats;
    }

    void print_stats_header()
    {
        std::cout << "fold | train       | valid   | sv | mse       | mae       | sign-hit | signal | signal-hit\n";
    }

    void print_stats_row(std::size_t fold, std::size_t trainEnd, const FoldStats &stats)
    {
        std::cout << std::setw(4) << fold << " | " << std::setw(11) << trainEnd
                  << " | " << std::setw(7) << stats.validationCount
                  << " | " << std::setw(2) << stats.supportVectorCount
                  << " | " << std::setprecision(4) << std::setw(10) << stats.mse
                  << " | " << std::setw(8) << stats.mae
                  << " | " << std::setprecision(3) << std::setw(9) << stats.signHitRate
                  << " | " << std::setw(7) << stats.signalRate
                  << " | " << std::setw(10) << stats.signalHitRate << '\n';
    }
}

int main(int argc, char *argv[])
{
    try
    {
        const auto options = parse_options(argc, argv);

        if (options.firstTrainSamples < feature_builder::kFeatureStartIndex + 1)
            std::cerr << "Note: --first-train is smaller than the feature warm-up window.\n";

        const auto dataset = load_training_dataset(options.dataFile);

        const auto parameters = options.tunerFile
                                    ? load_tuner_parameters(*options.tunerFile)
                                    : load_tuner_parameters_for_data_file(options.dataFile);

        std::cout << "Walk-forward validation on " << options.dataFile
                  << " (" << dataset.candleCount << " candles, " << dataset.samples.size()
                  << " samples), tuner C=" << parameters.c
                  << " eps-ins=" << parameters.epsilon
                  << " gamma=" << parameters.gamma
                  << ", threshold=" << options.actionThreshold << "\n";

        const auto &allSamples = dataset.samples;
        const auto &allLabels = dataset.labels;
        const std::size_t total = allSamples.size();

        if (total < options.firstTrainSamples + options.stepSamples + options.finalTestSamples)
            throw std::runtime_error(
                "Sample count " + std::to_string(total) + " is too small for --first-train " +
                std::to_string(options.firstTrainSamples) + " + --step " +
                std::to_string(options.stepSamples) + " + --final-test " +
                std::to_string(options.finalTestSamples));

        const std::size_t usable = total - options.finalTestSamples;
        const auto trainer = make_trainer(parameters);

        const auto takeTrainWindow = [&](std::size_t trainEnd) -> std::vector<sample_type>
        {
            const std::size_t start = options.maxTrainSamples > 0 && trainEnd > options.maxTrainSamples
                                          ? trainEnd - options.maxTrainSamples
                                          : 0;
            return std::vector<sample_type>(allSamples.begin() + static_cast<std::ptrdiff_t>(start),
                                            allSamples.begin() + static_cast<std::ptrdiff_t>(trainEnd));
        };

        print_stats_header();

        std::size_t fold = 0;
        std::vector<FoldStats> foldStats;
        for (std::size_t trainEnd = options.firstTrainSamples; trainEnd + options.stepSamples <= usable;
             trainEnd += options.stepSamples, ++fold)
        {
            const auto validationEnd = trainEnd + options.stepSamples;

            const auto trainSamples = takeTrainWindow(trainEnd);
            const std::vector<double> trainLabels(allLabels.begin(), allLabels.begin() + static_cast<std::ptrdiff_t>(trainEnd));
            const std::vector<sample_type> validationSamples(
                allSamples.begin() + static_cast<std::ptrdiff_t>(trainEnd),
                allSamples.begin() + static_cast<std::ptrdiff_t>(validationEnd));
            const std::vector<double> validationLabels(
                allLabels.begin() + static_cast<std::ptrdiff_t>(trainEnd),
                allLabels.begin() + static_cast<std::ptrdiff_t>(validationEnd));

            const auto stats = score_fold(trainer, trainSamples, trainLabels,
                                          validationSamples, validationLabels,
                                          options.actionThreshold);
            print_stats_row(fold, trainEnd, stats);
            foldStats.push_back(stats);
        }

        if (foldStats.empty())
        {
            std::cerr << "No walk-forward folds fit the data. Increase the dataset or lower "
                         "--first-train/--step.\n";
            return 1;
        }

        const auto finalSamples = takeTrainWindow(usable);
        std::vector<double> finalLabels(allLabels.begin(), allLabels.begin() + static_cast<std::ptrdiff_t>(usable));
        const std::vector<sample_type> finalValidationSamples(
            allSamples.begin() + static_cast<std::ptrdiff_t>(usable), allSamples.end());
        const std::vector<double> finalValidationLabels(
            allLabels.begin() + static_cast<std::ptrdiff_t>(usable), allLabels.end());

        const auto finalStats = score_fold(trainer, finalSamples, finalLabels,
                                           finalValidationSamples, finalValidationLabels,
                                           options.actionThreshold);
        print_stats_row(fold, usable, finalStats);

        double meanMse = 0.0;
        double meanSignHitRate = 0.0;
        double meanSignalRate = 0.0;
        double meanSignalHitRate = 0.0;
        for (const auto &stats : foldStats)
        {
            meanMse += stats.mse / static_cast<double>(foldStats.size());
            meanSignHitRate += stats.signHitRate / static_cast<double>(foldStats.size());
            meanSignalRate += stats.signalRate / static_cast<double>(foldStats.size());
            meanSignalHitRate += stats.signalHitRate / static_cast<double>(foldStats.size());
        }

        std::cout << std::setprecision(4)
                  << "folds=" << foldStats.size()
                  << " walk-forward mse=" << meanMse
                  << " sign-hit=" << std::setprecision(3) << meanSignHitRate
                  << " signal-rate=" << meanSignalRate
                  << " signal-hit=" << meanSignalHitRate
                  << " | final-test mse=" << finalStats.mse
                  << " sign-hit=" << finalStats.signHitRate
                  << " signal-rate=" << finalStats.signalRate
                  << " signal-hit=" << finalStats.signalHitRate << '\n';
    }
    catch (const std::exception &e)
    {
        std::cerr << "validate.bin: " << e.what() << '\n';
        return 1;
    }

    return 0;
}