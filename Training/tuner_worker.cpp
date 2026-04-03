#include "Training/tuner_worker.h"

#include <dlib/global_optimization.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    std::string stem_or_default(const std::string &path, const std::string &fallback)
    {
        const auto stem = fs::path(path).stem().string();
        return stem.empty() ? fallback : stem;
    }

    [[noreturn]] void throw_if_stopped()
    {
        throw std::runtime_error("Tuning interrupted by stop request.");
    }
}

TunerWorker::TunerWorker(std::string dataFile, int maxFunctionCalls, std::string outputFile)
    : cBaseWorker_V2(buildWorkerName(dataFile)),
      m_dataFile(std::move(dataFile)),
      m_outputFile(outputFile.empty() ? buildDefaultOutputPath(m_dataFile) : std::move(outputFile)),
      m_maxFunctionCalls(maxFunctionCalls)
{
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
        setStage("normalizing samples", result.candleCount, result.sampleCount);

        if (!continueRunning())
        {
            result.message = "Tuning stopped before normalization completed for " + m_dataFile;
            setStage("stopped", result.candleCount, result.sampleCount);
            setResult(std::move(result));
            return;
        }

        auto samples = dataset.samples;

        dlib::vector_normalizer<sample_type> normalizer;
        normalizer.train(samples);
        for (auto &sample : samples)
            sample = normalizer(sample);

        const auto folds = std::min<long>(5, static_cast<long>(samples.size()));
        if (folds < 2)
            throw std::runtime_error("Not enough samples for cross validation in " + m_dataFile);

        setStage("optimizing hyperparameters", result.candleCount, result.sampleCount);

        auto crossValidationScore = [&](double C, double epsilon, double gamma)
        {
            if (!continueRunning())
                throw_if_stopped();

            const TrainingHyperparameters parameters{C, epsilon, gamma};
            beginEvaluation(parameters);

            dlib::svr_trainer<kernel_type> trainer;
            trainer.set_epsilon_insensitivity(0.00001);
            trainer.set_cache_size(2000);
            trainer.set_c(C);
            trainer.set_epsilon(epsilon);
            trainer.set_kernel(kernel_type(gamma));

            const auto crossValidationResult = dlib::cross_validate_regression_trainer(
                trainer,
                samples,
                dataset.labels,
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
            {0.01, 0.0001, 0.0001},
            {1000.0, 0.1, 1.0},
            dlib::max_function_calls(m_maxFunctionCalls));

        if (!continueRunning())
        {
            result.message = "Tuning stopped before saving results for " + m_dataFile;
            setStage("stopped", result.candleCount, result.sampleCount);
            setResult(std::move(result));
            return;
        }

        const TrainingHyperparameters parameters{
            optimizationResult.x(0),
            optimizationResult.x(1),
            optimizationResult.x(2)};

        setStage("serializing results", result.candleCount, result.sampleCount);
        save_tuner_parameters(m_outputFile, parameters);

        const auto progressSnapshot = progress();
        result.success = true;
        result.completedEvaluations = progressSnapshot.completedEvaluations;
        result.parameters = parameters;
        result.mse = -optimizationResult.y;
        result.message = "Tuned and saved as " + m_outputFile +
                         " using C=" + std::to_string(parameters.c) +
                         ", epsilon=" + std::to_string(parameters.epsilon) +
                         ", gamma=" + std::to_string(parameters.gamma) +
                         ", mse=" + std::to_string(result.mse);

        updateHeartbeat();
        setStage("completed", result.candleCount, result.sampleCount);
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
            setStage("stopped", result.candleCount, result.sampleCount);
        }
        else
        {
            result.message = "Tuning failed for " + m_dataFile + ": " + e.what();
            setStage("failed", result.candleCount, result.sampleCount);
        }

        setResult(std::move(result));
    }
}

void TunerWorker::setResult(TuningJobResult result)
{
    std::lock_guard<std::mutex> lock(m_resultMutex);
    m_result = std::move(result);
}

void TunerWorker::setStage(std::string stage, std::size_t candleCount, std::size_t sampleCount)
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.stage = std::move(stage);
    m_progress.candleCount = candleCount;
    m_progress.sampleCount = sampleCount;
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
