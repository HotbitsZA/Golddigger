#include "Training/tuner_worker.h"
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
    constexpr int kDefaultMaxFunctionCalls = 50;
    constexpr int kDefaultProgressIntervalSeconds = 10 * 60;
    constexpr const char *kDefaultDataFile = "Data/xauusd-m15-bid.csv";
    constexpr const char *kDefaultOutputFile = "Models/tuner_m15.dat";

    struct TuningRequest
    {
        std::string dataFile;
        std::string outputFile;
    };

    struct RunningWorker
    {
        std::unique_ptr<TunerWorker> worker;
        clock_type::time_point startedAt{};
        clock_type::time_point nextProgressAt{};
        clock_type::time_point finishedAt{};
        bool hasFinished{false};
    };

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

    void print_usage()
    {
        std::cout << "Usage: tuner.bin [--max-calls N] [--progress-seconds N] [data.csv[=output.dat] ...]\n";
    }

    TuningRequest parse_tuning_argument(const std::string &argument)
    {
        const auto separator = argument.find('=');
        if (separator == std::string::npos)
            return {argument, {}};

        return {argument.substr(0, separator), argument.substr(separator + 1)};
    }
}

int main(int argc, char *argv[])
{
    install_termination_signal_handlers();

    int maxFunctionCalls = kDefaultMaxFunctionCalls;
    auto progressInterval = std::chrono::seconds(kDefaultProgressIntervalSeconds);
    std::vector<RunningWorker> workers;
    std::unordered_set<std::string> outputFiles;

    std::vector<TuningRequest> requests;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--help")
        {
            print_usage();
            return 0;
        }

        if (argument == "--max-calls")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for --max-calls\n";
                return 1;
            }

            maxFunctionCalls = std::stoi(argv[++i]);
            continue;
        }

        if (argument == "--progress-seconds")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for --progress-seconds\n";
                return 1;
            }

            progressInterval = std::chrono::seconds(std::stoi(argv[++i]));
            if (progressInterval.count() <= 0)
            {
                std::cerr << "--progress-seconds must be greater than 0\n";
                return 1;
            }

            continue;
        }

        requests.push_back(parse_tuning_argument(argument));
    }

    if (requests.empty())
        requests.push_back({kDefaultDataFile, kDefaultOutputFile});

    workers.reserve(requests.size());
    for (const auto &request : requests)
    {
        auto worker = request.outputFile.empty()
                          ? std::make_unique<TunerWorker>(request.dataFile, maxFunctionCalls)
                          : std::make_unique<TunerWorker>(request.dataFile, maxFunctionCalls, request.outputFile);

        if (!outputFiles.insert(worker->outputFile()).second)
        {
            std::cerr << "Duplicate tuner output path: " << worker->outputFile() << '\n';
            return 1;
        }

        RunningWorker runningWorker;
        runningWorker.worker = std::move(worker);
        workers.push_back(std::move(runningWorker));
    }

    for (auto &worker : workers)
    {
        std::cout << "Starting " << worker.worker->name()
                  << " using " << worker.worker->dataFile()
                  << " -> " << worker.worker->outputFile()
                  << " (max calls: " << worker.worker->maxFunctionCalls() << ")\n";

        if (!worker.worker->startThread(cBaseWorker_V2::duration_type::zero()))
        {
            std::cerr << "Failed to start " << worker.worker->name() << '\n';
            return 1;
        }

        worker.startedAt = clock_type::now();
        worker.nextProgressAt = worker.startedAt + progressInterval;
    }

    bool shutdownRequested = false;
    bool waitingForWorkers = true;
    while (waitingForWorkers)
    {
        waitingForWorkers = false;
        const auto now = clock_type::now();

        if (termination_signal_received() && !shutdownRequested)
        {
            shutdownRequested = true;
            std::cout << "Received " << termination_signal_name(termination_signal_number())
                      << ", requesting tuner workers to stop...\n";
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

                if (progress.startedEvaluations > 0)
                {
                    std::cout << " | started=" << progress.startedEvaluations
                              << " completed=" << progress.completedEvaluations;
                }

                if (progress.evaluationRunning && progress.hasCurrentParameters)
                {
                    std::cout << " | current C=" << progress.currentParameters.c
                              << " epsilon=" << progress.currentParameters.epsilon
                              << " gamma=" << progress.currentParameters.gamma
                              << " | current eval running for "
                              << format_elapsed(now - progress.currentEvaluationStartedAt);
                }

                if (progress.hasBestParameters)
                {
                    std::cout << " | best mse=" << progress.bestMse
                              << " | C=" << progress.bestParameters.c
                              << " epsilon=" << progress.bestParameters.epsilon
                              << " gamma=" << progress.bestParameters.gamma;
                }

                std::cout << '\n';
                worker.nextProgressAt = now + progressInterval;
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
                      << ", " << result.completedEvaluations << " completed evaluations"
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
