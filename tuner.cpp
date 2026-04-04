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
    constexpr std::size_t kDefaultTuningSamples = 5000;
    constexpr long kDefaultFoldCount = 3;
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
        bool epsilonRangeAnnounced{false};
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
        std::cout << "Usage: tuner.bin [--max-calls N] [--progress-seconds N] [--tuning-samples N] [--folds N] [--epsilon-range auto|MIN:MAX] [data.csv[=output.dat] ...]\n";
    }

    TuningRequest parse_tuning_argument(const std::string &argument)
    {
        const auto separator = argument.find('=');
        if (separator == std::string::npos)
            return {argument, {}};

        return {argument.substr(0, separator), argument.substr(separator + 1)};
    }

    bool parse_range_spec(const std::string &value, double &rangeMin, double &rangeMax)
    {
        const auto separator = value.find(':');
        if (separator == std::string::npos)
            return false;

        rangeMin = std::stod(value.substr(0, separator));
        rangeMax = std::stod(value.substr(separator + 1));
        return true;
    }
}

int main(int argc, char *argv[])
{
    install_termination_signal_handlers();

    int maxFunctionCalls = kDefaultMaxFunctionCalls;
    auto progressInterval = std::chrono::seconds(kDefaultProgressIntervalSeconds);
    std::size_t maxTuningSamples = kDefaultTuningSamples;
    long foldCount = kDefaultFoldCount;
    bool autoEpsilonRange = true;
    double manualEpsilonMin = 0.0001;
    double manualEpsilonMax = 0.1;
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

        if (argument == "--tuning-samples")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for --tuning-samples\n";
                return 1;
            }

            const auto parsedValue = std::stoll(argv[++i]);
            if (parsedValue < 0)
            {
                std::cerr << "--tuning-samples must be 0 or greater\n";
                return 1;
            }

            maxTuningSamples = static_cast<std::size_t>(parsedValue);
            continue;
        }

        if (argument == "--folds")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for --folds\n";
                return 1;
            }

            foldCount = std::stol(argv[++i]);
            if (foldCount < 2)
            {
                std::cerr << "--folds must be at least 2\n";
                return 1;
            }

            continue;
        }

        if (argument == "--epsilon-range")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for --epsilon-range\n";
                return 1;
            }

            const std::string value = argv[++i];
            if (value == "auto")
            {
                autoEpsilonRange = true;
                continue;
            }

            if (!parse_range_spec(value, manualEpsilonMin, manualEpsilonMax))
            {
                std::cerr << "--epsilon-range must be 'auto' or 'min:max'\n";
                return 1;
            }

            if (!(manualEpsilonMin > 0.0) || !(manualEpsilonMax > manualEpsilonMin))
            {
                std::cerr << "--epsilon-range manual values must satisfy 0 < min < max\n";
                return 1;
            }

            autoEpsilonRange = false;
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
                          ? std::make_unique<TunerWorker>(request.dataFile, maxFunctionCalls, maxTuningSamples, foldCount)
                          : std::make_unique<TunerWorker>(request.dataFile, maxFunctionCalls, maxTuningSamples, foldCount, request.outputFile);

        if (autoEpsilonRange)
            worker->useAutoEpsilonRange();
        else
            worker->setManualEpsilonRange(manualEpsilonMin, manualEpsilonMax);

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
                  << " (max calls: " << worker.worker->maxFunctionCalls()
                  << ", tuning samples: ";
        if (worker.worker->maxTuningSamples() == 0)
            std::cout << "all";
        else
            std::cout << worker.worker->maxTuningSamples();
        std::cout << ", folds: " << worker.worker->foldCount()
                  << ", eps-ins range: ";
        if (worker.worker->epsilonRangeMode() == TunerWorker::EpsilonRangeMode::Auto)
            std::cout << "auto";
        else
            std::cout << worker.worker->manualEpsilonMin() << ':' << worker.worker->manualEpsilonMax();
        std::cout << ")\n";

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
            const auto progress = worker.worker->progress();

            if (!worker.epsilonRangeAnnounced && progress.hasEpsilonRange)
            {
                std::cout << '[' << worker.worker->name() << "] ";
                if (progress.epsilonRangeWasAuto)
                {
                    std::cout << "Derived epsilon-insensitivity range from label distribution: ["
                              << progress.epsilonRangeMin << ", "
                              << progress.epsilonRangeMax << "]\n";
                }
                else
                {
                    std::cout << "Using manual epsilon-insensitivity range: ["
                              << progress.epsilonRangeMin << ", "
                              << progress.epsilonRangeMax << "]\n";
                }
                worker.epsilonRangeAnnounced = true;
            }

            if (now >= worker.nextProgressAt)
            {
                std::cout << '[' << worker.worker->name() << "] "
                          << "Still running after " << format_elapsed(now - worker.startedAt)
                          << " | stage: " << progress.stage;

                if (progress.sampleCount > 0)
                {
                    std::cout << " | " << progress.sampleCount << " total samples from "
                              << progress.candleCount << " candles";
                }

                if (progress.tuningSampleCount > 0)
                {
                    std::cout << " | tuning " << progress.tuningSampleCount;
                    if (progress.sampleCount > 0 && progress.tuningSampleCount != progress.sampleCount)
                        std::cout << "/" << progress.sampleCount;
                    std::cout << " samples";
                    if (progress.foldCount > 0)
                        std::cout << " across " << progress.foldCount << " folds";
                }

                if (progress.hasEpsilonRange)
                {
                    std::cout << " | eps-ins range=[" << progress.epsilonRangeMin
                              << ", " << progress.epsilonRangeMax << "]";
                }

                if (progress.startedEvaluations > 0)
                {
                    std::cout << " | started=" << progress.startedEvaluations
                              << " completed=" << progress.completedEvaluations;
                }

                if (progress.evaluationRunning && progress.hasCurrentParameters)
                {
                    std::cout << " | current C=" << progress.currentParameters.c
                              << " eps-ins=" << progress.currentParameters.epsilon
                              << " gamma=" << progress.currentParameters.gamma
                              << " | current eval running for "
                              << format_elapsed(now - progress.currentEvaluationStartedAt);
                }

                if (progress.hasBestParameters)
                {
                    std::cout << " | best mse=" << progress.bestMse
                              << " | C=" << progress.bestParameters.c
                              << " eps-ins=" << progress.bestParameters.epsilon
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
                      << " (" << result.sampleCount << " total samples from "
                      << result.candleCount << " candles";
            if (result.tuningSampleCount > 0)
            {
                std::cout << ", tuned on " << result.tuningSampleCount << " samples";
                if (result.foldCount > 0)
                    std::cout << " across " << result.foldCount << " folds";
            }
            if (result.hasEpsilonRange)
            {
                std::cout << ", eps-ins range=[" << result.epsilonRangeMin
                          << ", " << result.epsilonRangeMax << "]";
            }
            std::cout
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
