#include "MarketData/candle_data_provider.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace
{
    std::string lower_copy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::tolower(ch));
                       });
        return value;
    }

    std::string format_timestamp(std::uint64_t timestampMs, std::tm *(*converter)(const std::time_t *))
    {
        const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
        const auto tm = *converter(&seconds);

        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }
}

std::string to_string(CandleTimeframe timeframe)
{
    switch (timeframe)
    {
    case CandleTimeframe::M15:
        return "m15";
    case CandleTimeframe::H1:
        return "h1";
    case CandleTimeframe::D1:
        return "d1";
    }

    return "m15";
}

bool parse_timeframe(const std::string &value, CandleTimeframe &timeframe)
{
    const auto normalized = lower_copy(value);
    if (normalized == "m15")
    {
        timeframe = CandleTimeframe::M15;
        return true;
    }

    if ((normalized == "h1") || (normalized == "1h"))
    {
        timeframe = CandleTimeframe::H1;
        return true;
    }

    if ((normalized == "d1") || (normalized == "1d") || (normalized == "daily"))
    {
        timeframe = CandleTimeframe::D1;
        return true;
    }

    return false;
}

std::chrono::milliseconds candle_interval(CandleTimeframe timeframe)
{
    switch (timeframe)
    {
    case CandleTimeframe::M15:
        return std::chrono::minutes(15);
    case CandleTimeframe::H1:
        return std::chrono::hours(1);
    case CandleTimeframe::D1:
        return std::chrono::hours(24);
    }

    return std::chrono::minutes(15);
}

std::string format_local_timestamp(std::uint64_t timestampMs)
{
    return format_timestamp(timestampMs, &std::localtime);
}

std::string format_utc_timestamp(std::uint64_t timestampMs)
{
    return format_timestamp(timestampMs, &std::gmtime);
}
