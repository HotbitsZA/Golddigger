#pragma once

#include "Indicators/indicators.h"

#include <cstddef>
#include <string>
#include <vector>

struct CandleMergeStats
{
    std::size_t addedCount{0};
    std::size_t replacedCount{0};
};

void read_gold_data(const std::string &filename, std::vector<Candle> &candles);
void write_gold_data(const std::string &filename, const std::vector<Candle> &candles);
CandleMergeStats merge_candles_by_timestamp(std::vector<Candle> &candles, const std::vector<Candle> &patch);
