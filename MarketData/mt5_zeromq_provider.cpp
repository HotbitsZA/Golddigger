#include "MarketData/mt5_zeromq_provider.h"

#include <sstream>

Mt5ZeroMqProvider::Mt5ZeroMqProvider(mt5::ClientConfig config)
    : m_client(std::move(config))
{
}

std::vector<Candle> Mt5ZeroMqProvider::fetchCandles(const CandleFetchRequest &request) const
{
    return m_client.getLiveData(request);
}

std::string Mt5ZeroMqProvider::describeRequest(const CandleFetchRequest &request) const
{
    std::ostringstream description;
    description << "MT5 GET_LIVE_DATA via " << m_client.describeEndpoint()
                << " instrument=" << request.instrument
                << " timeframe=" << to_string(request.timeframe)
                << " from=" << format_local_timestamp(request.fromTimestampMs)
                << " to=" << format_local_timestamp(request.toTimestampMs);
    return description.str();
}

std::string Mt5ZeroMqProvider::providerName() const
{
    return "MT5 ZeroMQ";
}

mt5::SpreadQuote Mt5ZeroMqProvider::fetchSpreadQuote(const std::string &instrument) const
{
    return m_client.getSpread(instrument);
}
