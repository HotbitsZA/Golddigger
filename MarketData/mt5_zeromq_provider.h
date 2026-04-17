#pragma once

#include "MT5/mt5_zeromq_client.h"
#include "MarketData/candle_data_provider.h"

#include <string>

class Mt5ZeroMqProvider final : public ICandleDataProvider
{
public:
    explicit Mt5ZeroMqProvider(mt5::ClientConfig config = {});

    std::vector<Candle> fetchCandles(const CandleFetchRequest &request) const override;
    std::string describeRequest(const CandleFetchRequest &request) const override;
    std::string providerName() const override;

    mt5::SpreadQuote fetchSpreadQuote(const std::string &instrument) const;

private:
    mt5::ZeroMqClient m_client;
};
