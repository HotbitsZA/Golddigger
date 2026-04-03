#pragma once

#include "MarketData/candle_data_provider.h"

#include <string>

class AlphaVantageFxProvider final : public ICandleDataProvider
{
public:
    struct Config
    {
        std::string apiKey;
        std::string baseUrl{"https://www.alphavantage.co/query"};
        std::string outputSize{"compact"};
    };

    explicit AlphaVantageFxProvider(Config config);

    std::vector<Candle> fetchCandles(const CandleFetchRequest &request) const override;

private:
    Config m_config;
};
