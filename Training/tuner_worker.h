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
    std::size_t tuningSampleCount{0};
    long foldCount{0};
    bool hasEpsilonRange{false};
    bool epsilonRangeWasAuto{true};
    double epsilonRangeMin{0.0};
    double epsilonRangeMax{0.0};
    bool hasGammaRange{false};
    bool gammaRangeWasAuto{true};
    double gammaRangeMin{0.0};
    double gammaRangeMax{0.0};
    std::size_t completedEvaluations{0};
    double mse{0.0};
    TrainingHyperparameters parameters;
    std::string message;
};

struct TuningProgress
{
    std::size_t candleCount{0};
    std::size_t sampleCount{0};
    std::size_t tuningSampleCount{0};
    long foldCount{0};
    bool hasEpsilonRange{false};
    bool epsilonRangeWasAuto{true};
    double epsilonRangeMin{0.0};
    double epsilonRangeMax{0.0};
    bool hasGammaRange{false};
    bool gammaRangeWasAuto{true};
    double gammaRangeMin{0.0};
    double gammaRangeMax{0.0};
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
    enum class RangeMode
    {
        Auto,
        Manual,
    };

    explicit TunerWorker(
        std::string dataFile,
        int maxFunctionCalls,
        std::size_t maxTuningSamples,
        long foldCount,
        std::string outputFile = {});

    void useAutoEpsilonRange() noexcept;
    void setManualEpsilonRange(double epsilonMin, double epsilonMax) noexcept;
    RangeMode epsilonRangeMode() const noexcept;
    double manualEpsilonMin() const noexcept;
    double manualEpsilonMax() const noexcept;
    void useAutoGammaRange() noexcept;
    void setManualGammaRange(double gammaMin, double gammaMax) noexcept;
    RangeMode gammaRangeMode() const noexcept;
    double manualGammaMin() const noexcept;
    double manualGammaMax() const noexcept;
    const std::string &dataFile() const noexcept;
    const std::string &outputFile() const noexcept;
    int maxFunctionCalls() const noexcept;
    std::size_t maxTuningSamples() const noexcept;
    long foldCount() const noexcept;
    TuningJobResult result() const;
    TuningProgress progress() const;

    static std::string buildDefaultOutputPath(const std::string &dataFile);
    static std::string buildWorkerName(const std::string &dataFile);

protected:
    bool preRun() override;
    void run() override;

private:
    void setResult(TuningJobResult result);
    void setStage(
        std::string stage,
        std::size_t candleCount = 0,
        std::size_t sampleCount = 0,
        std::size_t tuningSampleCount = 0,
        long foldCount = 0);
    void setEpsilonRange(double epsilonMin, double epsilonMax, bool wasAuto);
    void setGammaRange(double gammaMin, double gammaMax, bool wasAuto);
    void beginEvaluation(const TrainingHyperparameters &parameters);
    void completeEvaluation(double score, const TrainingHyperparameters &parameters);

    std::string m_dataFile;
    std::string m_outputFile;
    int m_maxFunctionCalls{0};
    std::size_t m_maxTuningSamples{0};
    long m_foldCount{0};
    RangeMode m_epsilonRangeMode{RangeMode::Auto};
    double m_manualEpsilonMin{0.0001};
    double m_manualEpsilonMax{0.1};
    RangeMode m_gammaRangeMode{RangeMode::Auto};
    double m_manualGammaMin{0.0001};
    double m_manualGammaMax{1.0};

    mutable std::mutex m_resultMutex;
    TuningJobResult m_result;

    mutable std::mutex m_progressMutex;
    TuningProgress m_progress;
};
