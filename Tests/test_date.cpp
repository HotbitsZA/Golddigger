#include "doctest.h"

#include "MarketData/candle_data_provider.h"
#include "DataUpdate/daily_csv_updater.h"

#include <chrono>
#include <cstdint>
#include <string>

TEST_CASE("parse_utc_date round trips a day start")
{
    std::uint64_t dayStart = 0;
    REQUIRE(parse_utc_date("2026-03-31", dayStart));
    REQUIRE(dayStart % 86400000ULL == 0);
    REQUIRE(format_utc_date(dayStart) == "2026-03-31");
    REQUIRE(format_utc_timestamp(dayStart) == "2026-03-31 00:00:00");
}

TEST_CASE("parse_utc_date accepts DD-MM-YYYY")
{
    std::uint64_t dayStartDashYmd = 0;
    std::uint64_t dayStartDmy = 0;
    REQUIRE(parse_utc_date("2026-03-31", dayStartDashYmd));
    REQUIRE(parse_utc_date("31-03-2026", dayStartDmy));
    REQUIRE(dayStartDashYmd == dayStartDmy);
}

TEST_CASE("parse_utc_date rejects invalid calendar dates")
{
    std::uint64_t timestamp = 0;
    // 2026 is not a leap year.
    REQUIRE_FALSE(parse_utc_date("2026-02-30", timestamp));
    REQUIRE_FALSE(parse_utc_date("2026-13-01", timestamp));
    REQUIRE_FALSE(parse_utc_date("2026-00-10", timestamp));
    REQUIRE_FALSE(parse_utc_date("not-a-date", timestamp));
    // A date with a time component is not a bare UTC day.
    REQUIRE_FALSE(parse_utc_date("2026-03-31 12:00", timestamp));
}

TEST_CASE("parse_utc_date_or_datetime round trips datetimes")
{
    std::uint64_t timestamp = 0;
    bool hasTime = false;

    REQUIRE(parse_utc_date_or_datetime("2026-03-31", timestamp, hasTime));
    REQUIRE_FALSE(hasTime);

    REQUIRE(parse_utc_date_or_datetime("2026-03-31 23:45", timestamp, hasTime));
    REQUIRE(hasTime);
    REQUIRE(format_utc_timestamp(timestamp) == "2026-03-31 23:45:00");

    REQUIRE(parse_utc_date_or_datetime("31-03-2026 23:45:30", timestamp, hasTime));
    REQUIRE(hasTime);
    REQUIRE(format_utc_timestamp(timestamp) == "2026-03-31 23:45:30");
}

TEST_CASE("previous_utc_day_start_timestamp_ms is a whole UTC day below now")
{
    const auto nowMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    const auto yesterday = previous_utc_day_start_timestamp_ms();
    REQUIRE(yesterday % 86400000ULL == 0);
    REQUIRE(yesterday < nowMs);
    const auto todayStart = (nowMs / 86400000ULL) * 86400000ULL;
    REQUIRE(yesterday == todayStart - 86400000ULL);
}

TEST_CASE("candle_interval maps timeframes to durations")
{
    REQUIRE(candle_interval(CandleTimeframe::M15) == std::chrono::minutes(15));
    REQUIRE(candle_interval(CandleTimeframe::H1) == std::chrono::hours(1));
    REQUIRE(candle_interval(CandleTimeframe::D1) == std::chrono::hours(24));
}