#pragma once

#include "cBaseWorker_V2.h"

#include <cstddef>
#include <mutex>
#include <string>

struct TrainingJobResult
{
    bool success{false};
    std::size_t candleCount{0};
    std::size_t sampleCount{0};
    std::string message;
};

struct TrainingProgress
{
    std::size_t candleCount{0};
    std::size_t sampleCount{0};
    std::string stage{"waiting"};
};

class TrainerWorker final : public cBaseWorker_V2
{
public:
    explicit TrainerWorker(std::string dataFile, std::string modelFile = {});

    const std::string &dataFile() const noexcept;
    const std::string &modelFile() const noexcept;
    TrainingJobResult result() const;
    TrainingProgress progress() const;

    static std::string buildDefaultModelPath(const std::string &dataFile);
    static std::string buildWorkerName(const std::string &dataFile);

protected:
    bool preRun() override;
    void run() override;

private:
    void setResult(TrainingJobResult result);
    void setProgress(std::string stage, std::size_t candleCount = 0, std::size_t sampleCount = 0);

    std::string m_dataFile;
    std::string m_modelFile;

    mutable std::mutex m_resultMutex;
    TrainingJobResult m_result;

    mutable std::mutex m_progressMutex;
    TrainingProgress m_progress;
};
