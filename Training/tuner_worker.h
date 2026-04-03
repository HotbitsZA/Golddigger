#pragma once

#include "Training/training_support.h"
#include "cBaseWorker_V2.h"

#include <cstddef>
#include <mutex>
#include <string>

struct TuningJobResult
{
    bool success{false};
    std::size_t candleCount{0};
    std::size_t sampleCount{0};
    std::size_t completedEvaluations{0};
    double mse{0.0};
    TrainingHyperparameters parameters;
    std::string message;
};

struct TuningProgress
{
    std::size_t candleCount{0};
    std::size_t sampleCount{0};
    std::size_t startedEvaluations{0};
    std::size_t completedEvaluations{0};
    bool evaluationRunning{false};
    bool hasCurrentParameters{false};
    TrainingHyperparameters currentParameters;
    cBaseWorker_V2::clock_type::time_point currentEvaluationStartedAt{};
    bool hasBestParameters{false};
    TrainingHyperparameters bestParameters;
    double bestMse{0.0};
    std::string stage{"waiting"};
};

class TunerWorker final : public cBaseWorker_V2
{
public:
    explicit TunerWorker(std::string dataFile, int maxFunctionCalls, std::string outputFile = {});

    const std::string &dataFile() const noexcept;
    const std::string &outputFile() const noexcept;
    int maxFunctionCalls() const noexcept;
    TuningJobResult result() const;
    TuningProgress progress() const;

    static std::string buildDefaultOutputPath(const std::string &dataFile);
    static std::string buildWorkerName(const std::string &dataFile);

protected:
    bool preRun() override;
    void run() override;

private:
    void setResult(TuningJobResult result);
    void setStage(std::string stage, std::size_t candleCount = 0, std::size_t sampleCount = 0);
    void beginEvaluation(const TrainingHyperparameters &parameters);
    void completeEvaluation(double score, const TrainingHyperparameters &parameters);

    std::string m_dataFile;
    std::string m_outputFile;
    int m_maxFunctionCalls{0};

    mutable std::mutex m_resultMutex;
    TuningJobResult m_result;

    mutable std::mutex m_progressMutex;
    TuningProgress m_progress;
};
