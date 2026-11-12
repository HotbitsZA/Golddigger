#pragma once

#include "MarketData/candle_data_provider.h"

#include <string>

class DukascopyCliDataProvider final : public ICandleDataProvider
{
public:
    explicit DukascopyCliDataProvider(
        std::string command = "npx dukascopy-node",
        std::string debugDirectory = {});

    std::vector<Candle> fetchCandles(const CandleFetchRequest &request) const override;
    std::string describeRequest(const CandleFetchRequest &request) const override;
    std::string providerName() const override { return "Dukascopy"; }
    std::chrono::milliseconds livePublicationInterval(CandleTimeframe timeframe) const override;

private:
    std::string m_command;
    std::string m_debugDirectory;
};
