#include "DataUpdate/daily_csv_updater.h"
#include "MarketData/dukascopy_cli_provider.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    void print_usage()
    {
        std::cout << "Usage: patcher.bin [--date YYYY-MM-DD] [--instrument SYMBOL]\n"
                  << "                   [--data-dir DIR] [--timeframes m15,h1,d1]\n"
                  << "                   [--dukascopy-command CMD]\n";
    }

    std::vector<CandleTimeframe> parse_timeframes(const std::string &value)
    {
        std::vector<CandleTimeframe> timeframes;
        std::unordered_set<std::string> seen;
        std::size_t start = 0;

        while (start <= value.size())
        {
            const auto separator = value.find(',', start);
            const auto token = value.substr(start, separator == std::string::npos ? std::string::npos : separator - start);
            if (token.empty())
                throw std::runtime_error("Timeframe list contains an empty value.");

            CandleTimeframe timeframe{};
            if (!parse_timeframe(token, timeframe))
                throw std::runtime_error("Unsupported timeframe in --timeframes: " + token);

            const auto key = to_string(timeframe);
            if (seen.insert(key).second)
                timeframes.push_back(timeframe);

            if (separator == std::string::npos)
                break;

            start = separator + 1;
        }

        return timeframes;
    }
}

int main(int argc, char *argv[])
{
    DailyPatchRequest request;
    std::string dukascopyCommand{"npx dukascopy-node"};

    if (const char *envCommand = std::getenv("DUKASCOPY_NODE_COMMAND"))
        dukascopyCommand = envCommand;

    request.dayStartTimestampMs = previous_utc_day_start_timestamp_ms();

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];

        auto require_value = [&](const char *option) -> const char *
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << option << '\n';
                std::exit(1);
            }

            return argv[++i];
        };

        if (argument == "--help")
        {
            print_usage();
            return 0;
        }
        if (argument == "--date")
        {
            if (!parse_utc_date(require_value("--date"), request.dayStartTimestampMs))
            {
                std::cerr << "Invalid date for --date. Use YYYY-MM-DD.\n";
                return 1;
            }
        }
        else if (argument == "--instrument")
        {
            request.instrument = require_value("--instrument");
        }
        else if (argument == "--data-dir")
        {
            request.dataDirectory = require_value("--data-dir");
        }
        else if (argument == "--timeframes")
        {
            request.timeframes = parse_timeframes(require_value("--timeframes"));
        }
        else if (argument == "--dukascopy-command")
        {
            dukascopyCommand = require_value("--dukascopy-command");
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            print_usage();
            return 1;
        }
    }

    try
    {
        DukascopyCliDataProvider provider(dukascopyCommand);
        DailyCsvUpdater updater(provider);
        const auto result = updater.patchDay(request);

        std::cout << "Patching " << result.instrument
                  << " for UTC day " << format_utc_date(result.dayStartTimestampMs) << '\n';

        for (const auto &patch : result.patches)
        {
            std::cout << "  " << to_string(patch.timeframe)
                      << " -> " << patch.filePath
                      << " | fetched=" << patch.fetchedCount
                      << " added=" << patch.addedCount
                      << " replaced=" << patch.replacedCount
                      << " total=" << patch.totalCount;

            if (!patch.wroteFile)
                std::cout << " | no file written";

            std::cout << '\n';
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Patching failed: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
