
#include "Training/trainer_worker.h"
#include "Utils/process_signals.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    using clock_type = std::chrono::steady_clock;

    constexpr auto kPollInterval = std::chrono::milliseconds(100);
    constexpr auto kProgressInterval = std::chrono::minutes(10);
    constexpr const char *kDefaultDataFile = "Data/xauusd-m15-bid.csv";
    constexpr const char *kDefaultModelFile = "Models/gold_digger_m15.dat";

    struct TrainingRequest
    {
        std::string dataFile;
        std::string modelFile;
    };

    struct RunningWorker
    {
        std::unique_ptr<TrainerWorker> worker;
        clock_type::time_point startedAt{};
        clock_type::time_point nextProgressAt{};
        clock_type::time_point finishedAt{};
        bool hasFinished{false};
    };

    TrainingRequest parseTrainingArgument(const std::string &argument)
    {
        const auto separator = argument.find('=');
        if (separator == std::string::npos)
            return {argument, {}};

        return {argument.substr(0, separator), argument.substr(separator + 1)};
    }

    std::string format_elapsed(clock_type::duration elapsed)
    {
        const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        const auto hours = totalSeconds / 3600;
        const auto minutes = (totalSeconds % 3600) / 60;
        const auto seconds = totalSeconds % 60;

        std::ostringstream out;
        if (hours > 0)
            out << hours << "h ";

        out << std::setw(2) << std::setfill('0') << minutes << "m "
            << std::setw(2) << std::setfill('0') << seconds << "s";
        return out.str();
    }
}

int main(int argc, char *argv[])
{
    install_termination_signal_handlers();

    // Just a little test, to read training data find out the max_label value.
    // double max_label = 0.0;
    // uint64_t isNaNcount = 0;
    // uint64_t isNaNlabelCount = 0;
    // std::string dataFile{kDefaultDataFile};

    // auto dataset = load_training_dataset(dataFile);

    // for(const auto& row : dataset.samples)
    // {
    //     for(long i = 0; i < row.size(); ++i)
    //     {
    //         if(std::isnan(row(i)))
    //             isNaNcount++;
    //     }
    // }

    // for (const auto &label : dataset.labels)
    // {
    //     if(std::isnan(label))
    //         isNaNlabelCount++;
    //     if (std::abs(label) > max_label)
    //         max_label = std::abs(label);
    // }
    // std::cout << "Max label magnitude: " << max_label << "vs Epsilon: " << 0.0025 << std::endl;
    // std::cout << "Number of NaN values in features: " << isNaNcount << std::endl;
    // std::cout << "Number of NaN values in labels: " << isNaNlabelCount << std::endl;
    // return 0;

    std::vector<RunningWorker> workers;
    std::unordered_set<std::string> modelOutputs;

    if (argc <= 1)
    {
        RunningWorker worker;
        worker.worker = std::make_unique<TrainerWorker>(kDefaultDataFile, kDefaultModelFile);
        modelOutputs.insert(worker.worker->modelFile());
        workers.push_back(std::move(worker));
    }
    else
    {
        workers.reserve(static_cast<std::size_t>(argc - 1));
        for (int i = 1; i < argc; ++i)
        {
            const auto request = parseTrainingArgument(argv[i]);
            auto worker = request.modelFile.empty()
                              ? std::make_unique<TrainerWorker>(request.dataFile)
                              : std::make_unique<TrainerWorker>(request.dataFile, request.modelFile);

            if (!modelOutputs.insert(worker->modelFile()).second)
            {
                std::cerr << "Duplicate model output path: " << worker->modelFile() << '\n';
                return 1;
            }

            RunningWorker runningWorker;
            runningWorker.worker = std::move(worker);
            workers.push_back(std::move(runningWorker));
        }
    }

    for (auto &worker : workers)
    {
        std::cout << "Starting " << worker.worker->name()
                  << " using " << worker.worker->dataFile()
                  << " -> " << worker.worker->modelFile() << '\n';

        if (!worker.worker->startThread(cBaseWorker_V2::duration_type::zero()))
        {
            std::cerr << "Failed to start " << worker.worker->name() << '\n';
            return 1;
        }

        worker.startedAt = clock_type::now();
        worker.nextProgressAt = worker.startedAt + kProgressInterval;
    }

    bool waitingForWorkers = true;
    bool shutdownRequested = false;
    while (waitingForWorkers)
    {
        waitingForWorkers = false;
        const auto now = clock_type::now();

        if (termination_signal_received() && !shutdownRequested)
        {
            shutdownRequested = true;
            std::cout << "Received " << termination_signal_name(termination_signal_number())
                      << ", requesting trainer workers to stop...\n";
            for (auto &worker : workers)
                worker.worker->requestStop();
        }

        for (auto &worker : workers)
        {
            if (worker.worker->stopped())
            {
                if (!worker.hasFinished)
                {
                    worker.finishedAt = now;
                    worker.hasFinished = true;
                }

                continue;
            }

            waitingForWorkers = true;

            if (now >= worker.nextProgressAt)
            {
                const auto progress = worker.worker->progress();
                std::cout << '[' << worker.worker->name() << "] "
                          << "Still running after " << format_elapsed(now - worker.startedAt)
                          << " | stage: " << progress.stage;

                if (progress.sampleCount > 0)
                {
                    std::cout << " | " << progress.sampleCount << " samples from "
                              << progress.candleCount << " candles";
                }

                std::cout << '\n';
                worker.nextProgressAt = now + kProgressInterval;
            }
        }

        if (waitingForWorkers)
            std::this_thread::sleep_for(kPollInterval);
    }

    int exitCode = 0;
    for (const auto &worker : workers)
    {
        const auto result = worker.worker->result();
        const auto finishedAt = worker.hasFinished ? worker.finishedAt : clock_type::now();
        const auto elapsed = format_elapsed(finishedAt - worker.startedAt);
        if (result.success)
        {
            std::cout << '[' << worker.worker->name() << "] "
                      << result.message
                      << " (" << result.sampleCount << " samples from "
                      << result.candleCount << " candles"
                      << ", completed in " << elapsed << ")\n";
        }
        else
        {
            std::cerr << '[' << worker.worker->name() << "] "
                      << result.message
                      << " (after " << elapsed << ")\n";
            exitCode = 1;
        }

        if (const auto exception = worker.worker->lastUnhandledException())
        {
            try
            {
                std::rethrow_exception(exception);
            }
            catch (const std::exception &e)
            {
                std::cerr << '[' << worker.worker->name() << "] unhandled worker exception: "
                          << e.what() << '\n';
            }
            catch (...)
            {
                std::cerr << '[' << worker.worker->name()
                          << "] unhandled worker exception: unknown error\n";
            }

            exitCode = 1;
        }
    }

    if (termination_signal_received())
        return 128 + termination_signal_number();

    return exitCode;
}
