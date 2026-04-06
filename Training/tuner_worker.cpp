#include "Training/tuner_worker.h"

#include <dlib/global_optimization.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    constexpr long kDefaultFoldCount = 3;
    constexpr double kSolverEpsilon = 0.001;
    constexpr long kMinFoldCount = 2;
    constexpr double kMinAutoEpsilon = 1e-6;
    constexpr double kMaxAutoEpsilon = 0.1;
    constexpr std::size_t kGammaReferenceSampleCount = 400;
    constexpr double kMinAutoGamma = 1e-5;
    constexpr double kMaxAutoGamma = 10.0;
    constexpr double kMinGammaDistance = 1e-6;

    struct EpsilonRange
    {
        double min{0.0001};
        double max{0.1};
    };

    struct GammaRange
    {
        double min{0.0001};
        double max{1.0};
    };

    std::string stem_or_default(const std::string &path, const std::string &fallback)
    {
        const auto stem = fs::path(path).stem().string();
        return stem.empty() ? fallback : stem;
    }

    [[noreturn]] void throw_if_stopped()
    {
        throw std::runtime_error("Tuning interrupted by stop request.");
    }

    TrainingDataset build_evenly_spaced_subset(const TrainingDataset &dataset, std::size_t maxSamples)
    {
        if (maxSamples == 0 || dataset.samples.size() <= maxSamples)
            return dataset;

        TrainingDataset subset;
        subset.candleCount = dataset.candleCount;
        subset.samples.reserve(maxSamples);
        subset.labels.reserve(maxSamples);

        const std::size_t inputCount = dataset.samples.size();
        const std::size_t denominator = maxSamples - 1;
        for (std::size_t i = 0; i < maxSamples; ++i)
        {
            const std::size_t index =
                (i == denominator) ? (inputCount - 1) : ((i * (inputCount - 1)) / denominator);
            subset.samples.push_back(dataset.samples[index]);
            subset.labels.push_back(dataset.labels[index]);
        }

        return subset;
    }

    std::vector<sample_type> build_evenly_spaced_sample_subset(const std::vector<sample_type> &samples, std::size_t maxSamples)
    {
        if (maxSamples == 0 || samples.size() <= maxSamples)
            return samples;

        std::vector<sample_type> subset;
        subset.reserve(maxSamples);

        const std::size_t inputCount = samples.size();
        const std::size_t denominator = maxSamples - 1;
        for (std::size_t i = 0; i < maxSamples; ++i)
        {
            const std::size_t index =
                (i == denominator) ? (inputCount - 1) : ((i * (inputCount - 1)) / denominator);
            subset.push_back(samples[index]);
        }

        return subset;
    }

    double quantile_sorted(const std::vector<double> &sortedValues, double quantile)
    {
        if (sortedValues.empty())
            return 0.0;

        quantile = std::clamp(quantile, 0.0, 1.0);
        const double position = quantile * static_cast<double>(sortedValues.size() - 1);
        const auto lowerIndex = static_cast<std::size_t>(std::floor(position));
        const auto upperIndex = static_cast<std::size_t>(std::ceil(position));

        if (lowerIndex == upperIndex)
            return sortedValues[lowerIndex];

        const double weight = position - static_cast<double>(lowerIndex);
        return sortedValues[lowerIndex] * (1.0 - weight) + sortedValues[upperIndex] * weight;
    }

    EpsilonRange derive_epsilon_range_from_labels(const std::vector<double> &labels)
    {
        if (labels.empty())
            return {};

        std::vector<double> absoluteLabels;
        absoluteLabels.reserve(labels.size());
        for (double label : labels)
            absoluteLabels.push_back(std::abs(label));

        std::sort(absoluteLabels.begin(), absoluteLabels.end());

        const double q10 = quantile_sorted(absoluteLabels, 0.10);
        const double q50 = quantile_sorted(absoluteLabels, 0.50);
        const double q90 = quantile_sorted(absoluteLabels, 0.90);

        const double reference = std::max({q10, q50, kMinAutoEpsilon});
        double epsilonMin = std::max(kMinAutoEpsilon, std::min(q10 * 0.5, reference * 0.5));
        double epsilonMax = std::max({epsilonMin * 20.0, q90, reference * 8.0});
        epsilonMax = std::min(epsilonMax, kMaxAutoEpsilon);

        if (epsilonMax <= epsilonMin)
            epsilonMax = std::min(kMaxAutoEpsilon, epsilonMin * 20.0);

        if (epsilonMax <= epsilonMin)
            epsilonMax = epsilonMin * 2.0;

        return {epsilonMin, epsilonMax};
    }

    GammaRange derive_gamma_range_from_samples(const std::vector<sample_type> &samples)
    {
        if (samples.size() < 2)
            return {};

        const auto referenceSamples =
            build_evenly_spaced_sample_subset(samples, std::min<std::size_t>(samples.size(), kGammaReferenceSampleCount));

        std::vector<double> squaredDistances;
        squaredDistances.reserve((referenceSamples.size() * (referenceSamples.size() - 1)) / 2);

        for (std::size_t i = 0; i < referenceSamples.size(); ++i)
        {
            for (std::size_t j = i + 1; j < referenceSamples.size(); ++j)
            {
                double distanceSquared = 0.0;
                for (long k = 0; k < referenceSamples[i].size(); ++k)
                {
                    const double diff = referenceSamples[i](k) - referenceSamples[j](k);
                    distanceSquared += diff * diff;
                }

                if (distanceSquared > 0.0)
                    squaredDistances.push_back(distanceSquared);
            }
        }

        if (squaredDistances.empty())
            return {};

        std::sort(squaredDistances.begin(), squaredDistances.end());

        const double q50 = quantile_sorted(squaredDistances, 0.50);
        const double centerGamma = 1.0 / std::max(q50, kMinGammaDistance);
        double gammaMin = std::max(kMinAutoGamma, centerGamma / 100.0);
        double gammaMax = std::min(kMaxAutoGamma, centerGamma * 100.0);

        if (gammaMax <= gammaMin)
            gammaMax = std::min(kMaxAutoGamma, gammaMin * 100.0);

        if (gammaMax <= gammaMin)
            gammaMax = gammaMin * 2.0;

        return {gammaMin, gammaMax};
    }
}

TunerWorker::TunerWorker(
    std::string dataFile,
    int maxFunctionCalls,
    std::size_t maxTuningSamples,
    long foldCount,
    std::string outputFile)
    : cBaseWorker_V2(buildWorkerName(dataFile)),
      m_dataFile(std::move(dataFile)),
      m_outputFile(outputFile.empty() ? buildDefaultOutputPath(m_dataFile) : std::move(outputFile)),
      m_maxFunctionCalls(maxFunctionCalls),
      m_maxTuningSamples(maxTuningSamples == 0 ? 0 : maxTuningSamples),
      m_foldCount(foldCount <= 0 ? kDefaultFoldCount : foldCount)
{
}

void TunerWorker::useAutoEpsilonRange() noexcept
{
    m_epsilonRangeMode = RangeMode::Auto;
}

void TunerWorker::setManualEpsilonRange(double epsilonMin, double epsilonMax) noexcept
{
    m_epsilonRangeMode = RangeMode::Manual;
    m_manualEpsilonMin = epsilonMin;
    m_manualEpsilonMax = epsilonMax;
}

TunerWorker::RangeMode TunerWorker::epsilonRangeMode() const noexcept
{
    return m_epsilonRangeMode;
}

double TunerWorker::manualEpsilonMin() const noexcept
{
    return m_manualEpsilonMin;
}

double TunerWorker::manualEpsilonMax() const noexcept
{
    return m_manualEpsilonMax;
}

void TunerWorker::useAutoGammaRange() noexcept
{
    m_gammaRangeMode = RangeMode::Auto;
}

void TunerWorker::setManualGammaRange(double gammaMin, double gammaMax) noexcept
{
    m_gammaRangeMode = RangeMode::Manual;
    m_manualGammaMin = gammaMin;
    m_manualGammaMax = gammaMax;
}

TunerWorker::RangeMode TunerWorker::gammaRangeMode() const noexcept
{
    return m_gammaRangeMode;
}

double TunerWorker::manualGammaMin() const noexcept
{
    return m_manualGammaMin;
}

double TunerWorker::manualGammaMax() const noexcept
{
    return m_manualGammaMax;
}

const std::string &TunerWorker::dataFile() const noexcept
{
    return m_dataFile;
}

const std::string &TunerWorker::outputFile() const noexcept
{
    return m_outputFile;
}

int TunerWorker::maxFunctionCalls() const noexcept
{
    return m_maxFunctionCalls;
}

std::size_t TunerWorker::maxTuningSamples() const noexcept
{
    return m_maxTuningSamples;
}

long TunerWorker::foldCount() const noexcept
{
    return m_foldCount;
}

TuningJobResult TunerWorker::result() const
{
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_result;
}

TuningProgress TunerWorker::progress() const
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    return m_progress;
}

std::string TunerWorker::buildDefaultOutputPath(const std::string &dataFile)
{
    return build_default_tuner_path(dataFile);
}

std::string TunerWorker::buildWorkerName(const std::string &dataFile)
{
    return "tuner-" + stem_or_default(dataFile, "worker");
}

bool TunerWorker::preRun()
{
    TuningJobResult result;
    setStage("validating inputs");

    if (m_dataFile.empty())
    {
        result.message = "No tuning data file was provided.";
        setStage("failed");
        setResult(std::move(result));
        return false;
    }

    if (!fs::exists(m_dataFile))
    {
        result.message = "Tuning data file not found: " + m_dataFile;
        setStage("failed");
        setResult(std::move(result));
        return false;
    }

    if (m_maxFunctionCalls <= 0)
    {
        result.message = "Maximum function calls must be greater than 0.";
        setStage("failed");
        setResult(std::move(result));
        return false;
    }

    if (m_foldCount < kMinFoldCount)
    {
        result.message = "Fold count must be at least 2.";
        setStage("failed");
        setResult(std::move(result));
        return false;
    }

    if (m_maxTuningSamples == 1)
    {
        result.message = "Maximum tuning samples must be 0 or at least 2.";
        setStage("failed");
        setResult(std::move(result));
        return false;
    }

    if (m_epsilonRangeMode == RangeMode::Manual)
    {
        if (!(m_manualEpsilonMin > 0.0) || !(m_manualEpsilonMax > m_manualEpsilonMin))
        {
            result.message = "Manual epsilon range must satisfy 0 < min < max.";
            setStage("failed");
            setResult(std::move(result));
            return false;
        }
    }

    if (m_gammaRangeMode == RangeMode::Manual)
    {
        if (!(m_manualGammaMin > 0.0) || !(m_manualGammaMax > m_manualGammaMin))
        {
            result.message = "Manual gamma range must satisfy 0 < min < max.";
            setStage("failed");
            setResult(std::move(result));
            return false;
        }
    }

    const auto outputDirectory = fs::path(m_outputFile).parent_path();
    if (!outputDirectory.empty())
    {
        std::error_code ec;
        fs::create_directories(outputDirectory, ec);
        if (ec)
        {
            result.message = "Failed to create tuner output directory for " + m_outputFile + ": " + ec.message();
            setStage("failed");
            setResult(std::move(result));
            return false;
        }
    }

    result.message = "Ready to tune from " + m_dataFile;
    setStage("ready");
    setResult(std::move(result));
    return true;
}

void TunerWorker::run()
{
    TuningJobResult result;
    result.message = "Tuning hyperparameters from " + m_dataFile;
    setStage("loading dataset");
    setResult(result);

    try
    {
        updateHeartbeat();

        const auto dataset = load_training_dataset(m_dataFile);
        result.candleCount = dataset.candleCount;
        result.sampleCount = dataset.samples.size();

        const auto tuningDataset = build_evenly_spaced_subset(dataset, m_maxTuningSamples);
        result.tuningSampleCount = tuningDataset.samples.size();

        const auto folds = std::min<long>(m_foldCount, static_cast<long>(tuningDataset.samples.size()));
        result.foldCount = folds;

        const auto epsilonRange =
            (m_epsilonRangeMode == RangeMode::Auto)
                ? derive_epsilon_range_from_labels(dataset.labels)
                : EpsilonRange{m_manualEpsilonMin, m_manualEpsilonMax};

        result.hasEpsilonRange = true;
        result.epsilonRangeWasAuto = (m_epsilonRangeMode == RangeMode::Auto);
        result.epsilonRangeMin = epsilonRange.min;
        result.epsilonRangeMax = epsilonRange.max;
        setEpsilonRange(epsilonRange.min, epsilonRange.max, result.epsilonRangeWasAuto);

        setStage(
            "normalizing samples",
            result.candleCount,
            result.sampleCount,
            result.tuningSampleCount,
            result.foldCount);

        if (!continueRunning())
        {
            result.message = "Tuning stopped before normalization completed for " + m_dataFile;
            setStage("stopped", result.candleCount, result.sampleCount, result.tuningSampleCount, result.foldCount);
            setResult(std::move(result));
            return;
        }

        auto samples = tuningDataset.samples;

        dlib::vector_normalizer<sample_type> normalizer;
        normalizer.train(samples);
        for (auto &sample : samples)
            sample = normalizer(sample);

        const auto gammaRange =
            (m_gammaRangeMode == RangeMode::Auto)
                ? derive_gamma_range_from_samples(samples)
                : GammaRange{m_manualGammaMin, m_manualGammaMax};

        result.hasGammaRange = true;
        result.gammaRangeWasAuto = (m_gammaRangeMode == RangeMode::Auto);
        result.gammaRangeMin = gammaRange.min;
        result.gammaRangeMax = gammaRange.max;
        setGammaRange(gammaRange.min, gammaRange.max, result.gammaRangeWasAuto);

        if (folds < 2)
            throw std::runtime_error("Not enough samples for cross validation in " + m_dataFile);

        setStage(
            "optimizing hyperparameters",
            result.candleCount,
            result.sampleCount,
            result.tuningSampleCount,
            result.foldCount);

        auto crossValidationScore = [&](double C, double epsilon, double gamma)
        {
            if (!continueRunning())
                throw_if_stopped();

            const TrainingHyperparameters parameters{C, epsilon, gamma};
            beginEvaluation(parameters);

            dlib::svr_trainer<kernel_type> trainer;
            trainer.set_epsilon(kSolverEpsilon);
            trainer.set_cache_size(2000);
            trainer.set_c(C);
            trainer.set_epsilon_insensitivity(epsilon);
            trainer.set_kernel(kernel_type(gamma));

            const auto crossValidationResult = dlib::cross_validate_regression_trainer(
                trainer,
                samples,
                tuningDataset.labels,
                folds);

            const double score = -crossValidationResult(0);
            completeEvaluation(score, parameters);
            updateHeartbeat();

            if (!continueRunning())
                throw_if_stopped();

            return score;
        };

        const auto optimizationResult = dlib::find_max_global(
            crossValidationScore,
            {0.01, epsilonRange.min, gammaRange.min},
            {1000.0, epsilonRange.max, gammaRange.max},
            dlib::max_function_calls(m_maxFunctionCalls));

        if (!continueRunning())
        {
            result.message = "Tuning stopped before saving results for " + m_dataFile;
            setStage("stopped", result.candleCount, result.sampleCount, result.tuningSampleCount, result.foldCount);
            setResult(std::move(result));
            return;
        }

        const TrainingHyperparameters parameters{
            optimizationResult.x(0),
            optimizationResult.x(1),
            optimizationResult.x(2)};

        setStage("serializing results", result.candleCount, result.sampleCount, result.tuningSampleCount, result.foldCount);
        save_tuner_parameters(m_outputFile, parameters);

        const auto progressSnapshot = progress();
        result.success = true;
        result.completedEvaluations = progressSnapshot.completedEvaluations;
        result.parameters = parameters;
        result.mse = -optimizationResult.y;
        result.message = "Tuned and saved as " + m_outputFile +
                         " using C=" + std::to_string(parameters.c) +
                         ", epsilon_insensitivity=" + std::to_string(parameters.epsilon) +
                         ", gamma=" + std::to_string(parameters.gamma) +
                         ", mse=" + std::to_string(result.mse) +
                         ", epsilon_range=[" + std::to_string(result.epsilonRangeMin) +
                         ", " + std::to_string(result.epsilonRangeMax) + "]" +
                         ", gamma_range=[" + std::to_string(result.gammaRangeMin) +
                         ", " + std::to_string(result.gammaRangeMax) + "]";

        updateHeartbeat();
        setStage("completed", result.candleCount, result.sampleCount, result.tuningSampleCount, result.foldCount);
        setResult(std::move(result));
    }
    catch (const std::exception &e)
    {
        const auto progressSnapshot = progress();
        result.completedEvaluations = progressSnapshot.completedEvaluations;
        if (stopRequested())
        {
            result.message = "Tuning stopped for " + m_dataFile +
                             " after " + std::to_string(result.completedEvaluations) + " completed evaluations.";
            setStage("stopped", result.candleCount, result.sampleCount, result.tuningSampleCount, result.foldCount);
        }
        else
        {
            result.message = "Tuning failed for " + m_dataFile + ": " + e.what();
            setStage("failed", result.candleCount, result.sampleCount, result.tuningSampleCount, result.foldCount);
        }

        setResult(std::move(result));
    }
}

void TunerWorker::setResult(TuningJobResult result)
{
    std::lock_guard<std::mutex> lock(m_resultMutex);
    m_result = std::move(result);
}

void TunerWorker::setStage(
    std::string stage,
    std::size_t candleCount,
    std::size_t sampleCount,
    std::size_t tuningSampleCount,
    long foldCount)
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.stage = std::move(stage);
    m_progress.candleCount = candleCount;
    m_progress.sampleCount = sampleCount;
    m_progress.tuningSampleCount = tuningSampleCount;
    m_progress.foldCount = foldCount;
}

void TunerWorker::setEpsilonRange(double epsilonMin, double epsilonMax, bool wasAuto)
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.hasEpsilonRange = true;
    m_progress.epsilonRangeWasAuto = wasAuto;
    m_progress.epsilonRangeMin = epsilonMin;
    m_progress.epsilonRangeMax = epsilonMax;
}

void TunerWorker::setGammaRange(double gammaMin, double gammaMax, bool wasAuto)
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.hasGammaRange = true;
    m_progress.gammaRangeWasAuto = wasAuto;
    m_progress.gammaRangeMin = gammaMin;
    m_progress.gammaRangeMax = gammaMax;
}

void TunerWorker::beginEvaluation(const TrainingHyperparameters &parameters)
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    ++m_progress.startedEvaluations;
    m_progress.evaluationRunning = true;
    m_progress.hasCurrentParameters = true;
    m_progress.currentParameters = parameters;
    m_progress.currentEvaluationStartedAt = clock_type::now();
}

void TunerWorker::completeEvaluation(double score, const TrainingHyperparameters &parameters)
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    ++m_progress.completedEvaluations;
    m_progress.evaluationRunning = false;
    if ((!m_progress.hasBestParameters) || (score > -m_progress.bestMse))
    {
        m_progress.hasBestParameters = true;
        m_progress.bestParameters = parameters;
        m_progress.bestMse = -score;
    }
}
