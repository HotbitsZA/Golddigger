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
        std::cout << "Usage: patcher.bin [--date YYYY-MM-DD|DD-MM-YYYY]\n"
                  << "                   [--from YYYY-MM-DD[ HH:MM[:SS]]|DD-MM-YYYY[ HH:MM[:SS]]\n"
                  << "                    --to YYYY-MM-DD[ HH:MM[:SS]]|DD-MM-YYYY[ HH:MM[:SS]]]\n"
                  << "                   [--instrument SYMBOL]\n"
                  << "                   [--data-dir DIR] [--timeframes m15,h1,d1]\n"
                  << "                   [--dukascopy-command CMD] [--dukascopy-debug-dir DIR]\n";
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
    std::string dukascopyDebugDirectory;
    bool hasSingleDate = false;
    bool hasRangeStart = false;
    bool hasRangeEnd = false;
    bool rangeStartHasTime = false;
    bool rangeEndHasTime = false;
    std::uint64_t rangeStartTimestampMs = 0;
    std::uint64_t rangeEndTimestampMs = 0;

    if (const char *envCommand = std::getenv("DUKASCOPY_NODE_COMMAND"))
        dukascopyCommand = envCommand;

    const auto defaultDayStartTimestampMs = previous_utc_day_start_timestamp_ms();
    request.dayStartTimestampMs = defaultDayStartTimestampMs;
    rangeStartTimestampMs = defaultDayStartTimestampMs;
    rangeEndTimestampMs = defaultDayStartTimestampMs;

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
                std::cerr << "Invalid date for --date. Use YYYY-MM-DD or DD-MM-YYYY.\n";
                return 1;
            }

            hasSingleDate = true;
            continue;
        }
        else if (argument == "--from")
        {
            if (!parse_utc_date_or_datetime(require_value("--from"), rangeStartTimestampMs, rangeStartHasTime))
            {
                std::cerr << "Invalid value for --from. Use YYYY-MM-DD[ HH:MM[:SS]] or DD-MM-YYYY[ HH:MM[:SS]].\n";
                return 1;
            }

            hasRangeStart = true;
            continue;
        }
        else if (argument == "--to")
        {
            if (!parse_utc_date_or_datetime(require_value("--to"), rangeEndTimestampMs, rangeEndHasTime))
            {
                std::cerr << "Invalid value for --to. Use YYYY-MM-DD[ HH:MM[:SS]] or DD-MM-YYYY[ HH:MM[:SS]].\n";
                return 1;
            }

            hasRangeEnd = true;
            continue;
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
        else if (argument == "--dukascopy-debug-dir")
        {
            dukascopyDebugDirectory = require_value("--dukascopy-debug-dir");
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            print_usage();
            return 1;
        }
    }

    if (hasSingleDate && (hasRangeStart || hasRangeEnd))
    {
        std::cerr << "Use either --date or --from/--to, not both.\n";
        return 1;
    }

    if (hasRangeStart != hasRangeEnd)
    {
        std::cerr << "--from and --to must be provided together.\n";
        return 1;
    }

    if (hasSingleDate)
    {
        rangeStartTimestampMs = request.dayStartTimestampMs;
        rangeEndTimestampMs = request.dayStartTimestampMs;
    }
    else if (hasRangeStart && hasRangeEnd)
    {
        if (rangeEndTimestampMs < rangeStartTimestampMs)
        {
            std::cerr << "--to must be the same day as or after --from.\n";
            return 1;
        }
    }

    try
    {
        DukascopyCliDataProvider provider(dukascopyCommand, dukascopyDebugDirectory);
        DailyCsvUpdater updater(provider);
        const bool rangeUsesExactTimes = rangeStartHasTime || rangeEndHasTime;
        if (!rangeUsesExactTimes && (rangeStartTimestampMs == rangeEndTimestampMs))
        {
            request.dayStartTimestampMs = rangeStartTimestampMs;
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
        else
        {
            DailyPatchResult result;
            if (rangeUsesExactTimes)
            {
                request.dayStartTimestampMs = rangeStartTimestampMs;
                result = updater.patchWindow(request, rangeStartTimestampMs, rangeEndTimestampMs, true);

                std::cout << "Summary for UTC window "
                          << format_utc_timestamp(rangeStartTimestampMs)
                          << " to "
                          << format_utc_timestamp(rangeEndTimestampMs)
                          << '\n';
            }
            else
            {
                request.dayStartTimestampMs = rangeStartTimestampMs;
                result = updater.patchRange(request, rangeEndTimestampMs);

                std::cout << "Summary for UTC days "
                          << format_utc_date(rangeStartTimestampMs)
                          << " to "
                          << format_utc_date(rangeEndTimestampMs)
                          << '\n';
            }

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
    }
    catch (const std::exception &e)
    {
        std::cerr << "Patching failed: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
