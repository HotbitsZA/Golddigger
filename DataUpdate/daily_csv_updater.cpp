#include "DataUpdate/daily_csv_updater.h"

#include "Utils/utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr std::uint64_t kDayMs = 24ull * 60ull * 60ull * 1000ull;

    std::string lower_copy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::tolower(ch));
                       });
        return value;
    }

    bool is_leap_year(int year)
    {
        return ((year % 4) == 0) && (((year % 100) != 0) || ((year % 400) == 0));
    }

    int days_in_month(int year, int month)
    {
        static const int kDaysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (month == 2)
            return is_leap_year(year) ? 29 : 28;

        return kDaysPerMonth[month - 1];
    }

    std::int64_t days_from_civil(int year, unsigned month, unsigned day)
    {
        year -= month <= 2;
        const int era = (year >= 0 ? year : year - 399) / 400;
        const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
        const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
        return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
    }

    std::string build_file_name(const std::string &instrument, CandleTimeframe timeframe)
    {
        return lower_copy(instrument) + "-" + to_string(timeframe) + "-bid.csv";
    }
}

DailyCsvUpdater::DailyCsvUpdater(const ICandleDataProvider &provider)
    : m_provider(provider)
{
}

DailyPatchResult DailyCsvUpdater::patchDay(const DailyPatchRequest &request) const
{
    if (request.dayStartTimestampMs == 0)
        throw std::runtime_error("Daily patch request is missing a UTC day start timestamp.");

    DailyPatchResult result;
    result.instrument = request.instrument;
    result.dayStartTimestampMs = request.dayStartTimestampMs;
    result.patches.reserve(request.timeframes.size());

    for (const auto timeframe : request.timeframes)
    {
        const auto filePath = defaultDataFilePath(request.dataDirectory, request.instrument, timeframe);
        const auto interval = static_cast<std::uint64_t>(candle_interval(timeframe).count());

        CandleFetchRequest fetchRequest;
        fetchRequest.instrument = request.instrument;
        fetchRequest.timeframe = timeframe;
        fetchRequest.fromTimestampMs = request.dayStartTimestampMs;
        fetchRequest.toTimestampMs = request.dayStartTimestampMs + kDayMs - interval;
        fetchRequest.includeVolumes = true;

        auto fetchedCandles = m_provider.fetchCandles(fetchRequest);

        std::vector<Candle> existingCandles;
        const bool fileExists = std::filesystem::exists(filePath);
        if (fileExists)
            read_gold_data(filePath, existingCandles);

        auto mergeStats = merge_candles_by_timestamp(existingCandles, fetchedCandles);

        bool wroteFile = false;
        if (fileExists || !fetchedCandles.empty())
        {
            write_gold_data(filePath, existingCandles);
            wroteFile = true;
        }

        result.patches.push_back(TimeframePatchResult{
            timeframe,
            filePath,
            fetchedCandles.size(),
            mergeStats.addedCount,
            mergeStats.replacedCount,
            existingCandles.size(),
            wroteFile});
    }

    return result;
}

std::string DailyCsvUpdater::defaultDataFilePath(
    const std::string &dataDirectory,
    const std::string &instrument,
    CandleTimeframe timeframe)
{
    return (std::filesystem::path(dataDirectory) / build_file_name(instrument, timeframe)).string();
}

bool parse_utc_date(const std::string &value, std::uint64_t &dayStartTimestampMs)
{
    int year = 0;
    int month = 0;
    int day = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
        return false;

    if ((month < 1) || (month > 12))
        return false;

    if ((day < 1) || (day > days_in_month(year, month)))
        return false;

    const auto daysSinceEpoch = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    if (daysSinceEpoch < 0)
        return false;

    dayStartTimestampMs = static_cast<std::uint64_t>(daysSinceEpoch) * kDayMs;
    return true;
}

std::uint64_t previous_utc_day_start_timestamp_ms()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    const auto todayStartMs = (nowMs / kDayMs) * kDayMs;
    return todayStartMs - kDayMs;
}

std::string format_utc_date(std::uint64_t dayStartTimestampMs)
{
    const std::time_t seconds = static_cast<std::time_t>(dayStartTimestampMs / 1000);
    const auto tm = *std::gmtime(&seconds);

    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
    return buffer;
}
