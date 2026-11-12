#pragma once

#include "MT5/mt5_protocol.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace mt5
{
    class ZeroMqClient
    {
    public:
        explicit ZeroMqClient(ClientConfig config = {});
        ~ZeroMqClient();

        ZeroMqClient(const ZeroMqClient &) = delete;
        ZeroMqClient &operator=(const ZeroMqClient &) = delete;

        std::string describeEndpoint() const;

        nlohmann::json ping() const;
        SpreadQuote getSpread(const std::string &instrument) const;
        std::vector<Candle> getLiveData(const CandleFetchRequest &request) const;
        TradeResult buy(const TradeRequest &request) const;
        TradeResult sell(const TradeRequest &request) const;
        CloseAllResult closeAll(const CloseAllRequest &request) const;

    private:
        nlohmann::json sendRequest(Command command, const nlohmann::json &payload) const;

        ClientConfig m_config;
        void *m_context{nullptr};
    };
}
