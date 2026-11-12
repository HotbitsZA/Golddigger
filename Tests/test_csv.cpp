#include "doctest.h"

#include "Utils/utils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    std::filesystem::path temp_csv_path(const std::string &tag)
    {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               ("golddigger_" + tag + "_" + std::to_string(unique) + ".csv");
    }

    void write_file(const std::filesystem::path &path, const std::string &contents)
    {
        std::ofstream out(path, std::ios::trunc);
        out << contents;
    }
}

TEST_CASE("read_gold_data skips malformed rows instead of aborting")
{
    const auto path = temp_csv_path("malformed");
    write_file(path,
               "timestamp,open,high,low,close,volume\n"
               "1641164400000,1828.604,1831.624,1828.544,1830.414,0.279\n"
               "not-a-timestamp,1,2,3,4,5\n"
               "1641165300000,1830.454,1830.674,1828.918,1829.238,0.1757\n"
               "1641166200000,1830.0,1831.0,1828.0,\n"
               "1641167100000,1829.0,1830.0,1827.0,1828.0,0.2\n");

    std::vector<Candle> candles;
    REQUIRE_NOTHROW(read_gold_data(path.string(), candles));

    const auto cleanup = [&] { std::filesystem::remove(path); };
    std::filesystem::remove(path);

    REQUIRE(candles.size() == 3);
    REQUIRE(candles[0].timestamp == 1641164400000ULL);
    REQUIRE(candles[2].close == 1828.0);
}

TEST_CASE("read_gold_data throws when the file is unusable")
{
    const auto path = temp_csv_path("empty");
    write_file(path, "timestamp,open,high,low,close,volume\n");

    std::vector<Candle> candles;
    REQUIRE_THROWS_AS(read_gold_data(path.string(), candles), std::runtime_error);
    std::filesystem::remove(path);
}

TEST_CASE("write_gold_data then read_gold_data round trips")
{
    const auto path = temp_csv_path("roundtrip");
    const std::vector<Candle> candles{
        Candle{1, 2.0, 3.0, 1.5, 2.5, 0.1},
        Candle{2, 2.5, 3.5, 2.0, 3.0, 0.2},
        Candle{3, 3.0, 4.0, 2.5, 3.5, 0.3},
    };

    write_gold_data(path.string(), candles);

    std::vector<Candle> read;
    REQUIRE_NOTHROW(read_gold_data(path.string(), read));
    std::filesystem::remove(path);

    REQUIRE(read.size() == candles.size());
    for (std::size_t i = 0; i < candles.size(); ++i)
    {
        REQUIRE(read[i].timestamp == candles[i].timestamp);
        REQUIRE(read[i].open == candles[i].open);
        REQUIRE(read[i].high == candles[i].high);
        REQUIRE(read[i].low == candles[i].low);
        REQUIRE(read[i].close == candles[i].close);
        REQUIRE(read[i].volume == candles[i].volume);
    }
}

TEST_CASE("merge_candles_by_timestamp adds and replaces by timestamp")
{
    const std::vector<Candle> existing{
        Candle{100, 1.0, 1.0, 1.0, 1.0, 0.0},
        Candle{200, 2.0, 2.0, 2.0, 2.0, 0.0},
    };
    const std::vector<Candle> patch{
        Candle{200, 9.0, 9.0, 9.0, 9.0, 0.0},   // replace
        Candle{300, 3.0, 3.0, 3.0, 3.0, 0.0},   // add
        Candle{100, 8.0, 8.0, 8.0, 8.0, 0.0},   // replace (order independent)
    };

    auto merged = existing;
    const auto stats = merge_candles_by_timestamp(merged, patch);

    REQUIRE(stats.addedCount == 1);
    REQUIRE(stats.replacedCount == 2);
    REQUIRE(merged.size() == 3);
    REQUIRE(merged[0].timestamp == 100);
    REQUIRE(merged[0].close == 8.0);
    REQUIRE(merged[1].timestamp == 200);
    REQUIRE(merged[1].close == 9.0);
    REQUIRE(merged[2].timestamp == 300);
}