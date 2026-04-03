#include "Training/trainer_worker.h"

#include "Training/model_types.h"
#include "Training/training_support.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    std::string stem_or_default(const std::string &path, const std::string &fallback)
    {
        const auto stem = fs::path(path).stem().string();
        return stem.empty() ? fallback : stem;
    }
}

TrainerWorker::TrainerWorker(std::string dataFile, std::string modelFile)
    : cBaseWorker_V2(buildWorkerName(dataFile)),
      m_dataFile(std::move(dataFile)),
      m_modelFile(modelFile.empty() ? buildDefaultModelPath(m_dataFile) : std::move(modelFile))
{
}

const std::string &TrainerWorker::dataFile() const noexcept
{
    return m_dataFile;
}

const std::string &TrainerWorker::modelFile() const noexcept
{
    return m_modelFile;
}

TrainingJobResult TrainerWorker::result() const
{
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_result;
}

TrainingProgress TrainerWorker::progress() const
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    return m_progress;
}

std::string TrainerWorker::buildDefaultModelPath(const std::string &dataFile)
{
    return (fs::path("Models") / (stem_or_default(dataFile, "gold_digger") + ".dat")).string();
}

std::string TrainerWorker::buildWorkerName(const std::string &dataFile)
{
    return "trainer-" + stem_or_default(dataFile, "worker");
}

bool TrainerWorker::preRun()
{
    TrainingJobResult result;
    setProgress("validating inputs");

    if (m_dataFile.empty())
    {
        result.message = "No training data file was provided.";
        setProgress("failed");
        setResult(std::move(result));
        return false;
    }

    if (!fs::exists(m_dataFile))
    {
        result.message = "Training data file not found: " + m_dataFile;
        setProgress("failed");
        setResult(std::move(result));
        return false;
    }

    const auto modelDirectory = fs::path(m_modelFile).parent_path();
    if (!modelDirectory.empty())
    {
        std::error_code ec;
        fs::create_directories(modelDirectory, ec);
        if (ec)
        {
            result.message = "Failed to create model directory for " + m_modelFile + ": " + ec.message();
            setProgress("failed");
            setResult(std::move(result));
            return false;
        }
    }

    result.message = "Ready to train from " + m_dataFile;
    setProgress("ready");
    setResult(std::move(result));
    return true;
}

void TrainerWorker::run()
{
    TrainingJobResult result;
    result.message = "Training model from " + m_dataFile;
    setProgress("loading dataset");
    setResult(result);

    try
    {
        updateHeartbeat();

        const auto dataset = load_training_dataset(m_dataFile);
        result.candleCount = dataset.candleCount;
        result.sampleCount = dataset.samples.size();
        setProgress("normalizing samples", result.candleCount, result.sampleCount);

        if (!continueRunning())
        {
            result.message = "Training stopped before the model fit began for " + m_dataFile;
            setProgress("stopped", result.candleCount, result.sampleCount);
            setResult(std::move(result));
            return;
        }

        auto samples = dataset.samples;
        const auto parameters = load_tuner_parameters_for_data_file(m_dataFile);

        dlib::vector_normalizer<sample_type> normalizer;
        normalizer.train(samples);
        for (auto &sample : samples)
            sample = normalizer(sample);

        setProgress("fitting SVR model", result.candleCount, result.sampleCount);

        dlib::svr_trainer<kernel_type> trainer;
        trainer.set_kernel(kernel_type(parameters.gamma));
        trainer.set_c(parameters.c);
        trainer.set_epsilon(parameters.epsilon);
        trainer.set_cache_size(2000); // 2G, Increase cache size to speed up training on larger datasets, but be mindful of memory usage. 
        trainer.set_epsilon_insensitivity(0.00001); // Internal solver precision

        // Checking the samples after normalization for NaN values, which can cause dlib to throw exceptions during training.
        for (const auto& sample : samples)        {
            for (long i = 0; i < sample.size(); ++i)            {
                if (std::isnan(sample(i)))                {
                    throw std::runtime_error("NaN value found in training samples after normalization, likely due to an issue with the input data. Please check the dataset for invalid values.");
                }
            }
        }

        std::cout << "Sample 0: " << dlib::trans(samples[0]) << std::endl;
        std::cout << "Sample 100: " << dlib::trans(samples[99]) << std::endl;
        std::set<double> unique_labels(dataset.labels.begin(), dataset.labels.end());
        std::cout << "Unique labels count: " << unique_labels.size() << std::endl;

        dlib::decision_function<kernel_type> decisionFunction = trainer.train(samples, dataset.labels);
        // Immediately check the size here
        long sv_count = decisionFunction.basis_vectors.size();
        std::cout << "SV Count immediately after train: " << sv_count << std::endl;

        if (!continueRunning())
        {
            result.message = "Training stopped before model serialization for " + m_dataFile;
            setProgress("stopped", result.candleCount, result.sampleCount);
            setResult(std::move(result));
            return;
        }

        setProgress("serializing model", result.candleCount, result.sampleCount);

        result.success = true;
        result.message = "Model trained and saved as " + m_modelFile +
                         " using C=" + std::to_string(parameters.c) +
                         ", epsilon=" + std::to_string(parameters.epsilon) +
                         ", gamma=" + std::to_string(parameters.gamma) +
                         ", Support Vectors=" + std::to_string(decisionFunction.basis_vectors.size()); // Nearly equal to the number of samples=overfitting, < 100=underfitting.

        dlib::serialize(m_modelFile) << decisionFunction << normalizer;
        updateHeartbeat();
        setProgress("completed", result.candleCount, result.sampleCount);
        setResult(std::move(result));
    }
    catch (const std::exception &e)
    {
        result.message = "Training failed for " + m_dataFile + ": " + e.what();
        setProgress("failed", result.candleCount, result.sampleCount);
        setResult(std::move(result));
    }
}

void TrainerWorker::setResult(TrainingJobResult result)
{
    std::lock_guard<std::mutex> lock(m_resultMutex);
    m_result = std::move(result);
}

void TrainerWorker::setProgress(std::string stage, std::size_t candleCount, std::size_t sampleCount)
{
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.stage = std::move(stage);
    m_progress.candleCount = candleCount;
    m_progress.sampleCount = sampleCount;
}
