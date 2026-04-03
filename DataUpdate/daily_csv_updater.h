#pragma once

#include "MarketData/candle_data_provider.h"

#include <cstdint>
#include <string>
#include <vector>

struct TimeframePatchResult
{
    CandleTimeframe timeframe{CandleTimeframe::M15};
    std::string filePath;
    std::size_t fetchedCount{0};
    std::size_t addedCount{0};
    std::size_t replacedCount{0};
    std::size_t totalCount{0};
    bool wroteFile{false};
};

struct DailyPatchRequest
{
    std::string instrument{"xauusd"};
    std::string dataDirectory{"Data"};
    std::uint64_t dayStartTimestampMs{0};
    std::vector<CandleTimeframe> timeframes{
        CandleTimeframe::M15,
        CandleTimeframe::H1,
        CandleTimeframe::D1};
};

struct DailyPatchResult
{
    std::string instrument;
    std::uint64_t dayStartTimestampMs{0};
    std::vector<TimeframePatchResult> patches;
};

class DailyCsvUpdater
{
public:
    explicit DailyCsvUpdater(const ICandleDataProvider &provider);

    DailyPatchResult patchDay(const DailyPatchRequest &request) const;

    static std::string defaultDataFilePath(
        const std::string &dataDirectory,
        const std::string &instrument,
        CandleTimeframe timeframe);

private:
    const ICandleDataProvider &m_provider;
};

bool parse_utc_date(const std::string &value, std::uint64_t &dayStartTimestampMs);
std::uint64_t previous_utc_day_start_timestamp_ms();
std::string format_utc_date(std::uint64_t dayStartTimestampMs);
