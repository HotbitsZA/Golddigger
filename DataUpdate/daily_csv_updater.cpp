#include "DataUpdate/daily_csv_updater.h"

#include "Utils/utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr std::uint64_t kDayMs = 24ull * 60ull * 60ull * 1000ull;

    std::string trim_copy(std::string value)
    {
        const auto notSpace = [](unsigned char ch)
        {
            return !std::isspace(ch);
        };

        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

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

    bool parse_date_components(const std::string &value, int &year, int &month, int &day)
    {
        int first = 0;
        int third = 0;
        char firstSeparator = '\0';
        char secondSeparator = '\0';
        if (std::sscanf(value.c_str(), "%d%c%d%c%d", &first, &firstSeparator, &month, &secondSeparator, &third) != 5)
            return false;

        if ((firstSeparator != '-') || (secondSeparator != '-'))
            return false;

        if (first >= 1000)
        {
            year = first;
            day = third;
        }
        else if (third >= 1000)
        {
            day = first;
            year = third;
        }
        else
        {
            return false;
        }

        if ((month < 1) || (month > 12))
            return false;

        if ((day < 1) || (day > days_in_month(year, month)))
            return false;

        return true;
    }

    bool parse_time_components(const std::string &value, int &hour, int &minute, int &second)
    {
        std::stringstream input(value);
        std::string token;
        std::vector<int> parts;

        while (std::getline(input, token, ':'))
        {
            token = trim_copy(token);
            if (token.empty())
                return false;

            if (!std::all_of(token.begin(), token.end(),
                             [](unsigned char ch)
                             {
                                 return std::isdigit(ch);
                             }))
                return false;

            parts.push_back(std::stoi(token));
        }

        if ((parts.size() != 2) && (parts.size() != 3))
            return false;

        hour = parts[0];
        minute = parts[1];
        second = (parts.size() == 3) ? parts[2] : 0;

        return (hour >= 0) && (hour < 24) &&
               (minute >= 0) && (minute < 60) &&
               (second >= 0) && (second < 60);
    }

    TimeframePatchResult patch_timeframe(
        const ICandleDataProvider &provider,
        const std::string &dataDirectory,
        const std::string &instrument,
        CandleTimeframe timeframe,
        std::uint64_t fromTimestampMs,
        std::uint64_t toTimestampMsExclusive)
    {
        const auto filePath = DailyCsvUpdater::defaultDataFilePath(dataDirectory, instrument, timeframe);

        CandleFetchRequest fetchRequest;
        fetchRequest.instrument = instrument;
        fetchRequest.timeframe = timeframe;
        fetchRequest.fromTimestampMs = fromTimestampMs;
        fetchRequest.toTimestampMs = toTimestampMsExclusive;
        fetchRequest.includeVolumes = true;

        auto fetchedCandles = provider.fetchCandles(fetchRequest);

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

        return TimeframePatchResult{
            timeframe,
            filePath,
            fetchedCandles.size(),
            mergeStats.addedCount,
            mergeStats.replacedCount,
            existingCandles.size(),
            wroteFile};
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

    return patchWindow(request, request.dayStartTimestampMs, request.dayStartTimestampMs + kDayMs, false);
}

DailyPatchResult DailyCsvUpdater::patchRange(
    const DailyPatchRequest &request,
    std::uint64_t rangeEndDayStartTimestampMs) const
{
    if (request.dayStartTimestampMs == 0)
        throw std::runtime_error("Range patch request is missing a UTC start day timestamp.");

    if (rangeEndDayStartTimestampMs < request.dayStartTimestampMs)
        throw std::runtime_error("Range patch end day must not be earlier than the start day.");

    return patchWindow(request, request.dayStartTimestampMs, rangeEndDayStartTimestampMs + kDayMs, false);
}

DailyPatchResult DailyCsvUpdater::patchWindow(
    const DailyPatchRequest &request,
    std::uint64_t windowStartTimestampMs,
    std::uint64_t windowEndTimestampMs,
    bool includeEndTimestamp) const
{
    if (windowEndTimestampMs < windowStartTimestampMs)
        throw std::runtime_error("Patch window end must not be earlier than the start.");

    DailyPatchResult result;
    result.instrument = request.instrument;
    result.dayStartTimestampMs = windowStartTimestampMs;
    result.patches.reserve(request.timeframes.size());

    for (const auto timeframe : request.timeframes)
    {
        const auto windowEndTimestampMsExclusive =
            windowEndTimestampMs +
            (includeEndTimestamp ? static_cast<std::uint64_t>(candle_interval(timeframe).count()) : 0ULL);

        result.patches.push_back(
            patch_timeframe(
                m_provider,
                request.dataDirectory,
                request.instrument,
                timeframe,
                windowStartTimestampMs,
                windowEndTimestampMsExclusive));
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
    bool hasTimeComponent = false;
    return parse_utc_date_or_datetime(value, dayStartTimestampMs, hasTimeComponent) && !hasTimeComponent;
}

bool parse_utc_date_or_datetime(const std::string &value, std::uint64_t &timestampMs, bool &hasTimeComponent)
{
    const auto trimmed = trim_copy(value);
    if (trimmed.empty())
        return false;

    std::string datePart = trimmed;
    std::string timePart;
    const auto separator = trimmed.find_first_of(" T");
    if (separator != std::string::npos)
    {
        datePart = trim_copy(trimmed.substr(0, separator));
        timePart = trim_copy(trimmed.substr(separator + 1));
    }

    int year = 0;
    int month = 0;
    int day = 0;
    if (!parse_date_components(datePart, year, month, day))
        return false;

    const auto daysSinceEpoch = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    if (daysSinceEpoch < 0)
        return false;

    int hour = 0;
    int minute = 0;
    int second = 0;
    hasTimeComponent = !timePart.empty();
    if (hasTimeComponent && !parse_time_components(timePart, hour, minute, second))
        return false;

    timestampMs = static_cast<std::uint64_t>(daysSinceEpoch) * kDayMs +
                  static_cast<std::uint64_t>(hour) * 60ull * 60ull * 1000ull +
                  static_cast<std::uint64_t>(minute) * 60ull * 1000ull +
                  static_cast<std::uint64_t>(second) * 1000ull;
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
