#pragma once

#include "Indicators/indicators.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

enum class CandleTimeframe : std::uint8_t
{
    M15,
    H1,
    D1
};

struct CandleFetchRequest
{
    std::string instrument{"xauusd"};
    CandleTimeframe timeframe{CandleTimeframe::M15};
    std::uint64_t fromTimestampMs{0};
    std::uint64_t toTimestampMs{0};
    bool includeVolumes{true};
};

std::string to_string(CandleTimeframe timeframe);
bool parse_timeframe(const std::string &value, CandleTimeframe &timeframe);
std::chrono::milliseconds candle_interval(CandleTimeframe timeframe);
std::string format_local_timestamp(std::uint64_t timestampMs);
std::string format_utc_timestamp(std::uint64_t timestampMs);

class ICandleDataProvider
{
public:
    virtual ~ICandleDataProvider() = default;

    virtual std::vector<Candle> fetchCandles(const CandleFetchRequest &request) const = 0;

    virtual std::string describeRequest(const CandleFetchRequest &request) const
    {
        return "fetch "
            + request.instrument + ' ' + to_string(request.timeframe)
            + " from " + format_local_timestamp(request.fromTimestampMs)
            + " to " + format_local_timestamp(request.toTimestampMs);
    }

    virtual std::chrono::milliseconds livePublicationInterval(CandleTimeframe timeframe) const
    {
        return candle_interval(timeframe);
    }
};
