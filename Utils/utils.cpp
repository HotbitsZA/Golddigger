#include "utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <stdexcept>
#include <sstream>

void read_gold_data(const std::string &filename, std::vector<Candle> &candles)
{
    std::vector<Candle> data;
    std::ifstream file(filename);
    std::string line;
    std::getline(file, line); // Skip header

    while (std::getline(file, line))
    {
        std::stringstream ss(line);
        std::string val;
        Candle c;
        std::getline(ss, val, ',');
        c.timestamp = std::stoull(val);
        std::getline(ss, val, ',');
        c.open = std::stod(val);
        std::getline(ss, val, ',');
        c.high = std::stod(val);
        std::getline(ss, val, ',');
        c.low = std::stod(val);
        std::getline(ss, val, ',');
        c.close = std::stod(val);
        std::getline(ss, val, ',');
        c.volume = std::stod(val);
        data.push_back(c);
    }
    candles.clear();
    candles.swap(data);
}

void write_gold_data(const std::string &filename, const std::vector<Candle> &candles)
{
    const std::filesystem::path path(filename);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    std::ofstream file(filename, std::ios::trunc);
    if (!file)
        throw std::runtime_error("Failed to open candle file for writing: " + filename);

    file << "timestamp,open,high,low,close,volume\n";
    file << std::setprecision(15);
    for (const auto &candle : candles)
    {
        file << candle.timestamp << ','
             << candle.open << ','
             << candle.high << ','
             << candle.low << ','
             << candle.close << ','
             << candle.volume << '\n';
    }
}

CandleMergeStats merge_candles_by_timestamp(std::vector<Candle> &candles, const std::vector<Candle> &patch)
{
    std::map<std::uint64_t, Candle> merged;
    for (const auto &candle : candles)
        merged[candle.timestamp] = candle;

    CandleMergeStats stats;
    for (const auto &candle : patch)
    {
        const auto [it, inserted] = merged.insert_or_assign(candle.timestamp, candle);
        (void)it;
        if (inserted)
            ++stats.addedCount;
        else
            ++stats.replacedCount;
    }

    candles.clear();
    candles.reserve(merged.size());
    for (const auto &[timestamp, candle] : merged)
    {
        (void)timestamp;
        candles.push_back(candle);
    }

    return stats;
}
