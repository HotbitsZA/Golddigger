#pragma once

#include "MarketData/candle_data_provider.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mt5
{
    inline constexpr int kProtocolVersion = 1;
    inline constexpr char kProtocolName[] = "golddigger.mt5.zmq";

    enum class Command : std::uint8_t
    {
        Ping,
        GetSpread,
        GetLiveData,
        Buy,
        Sell,
        CloseAll
    };

    struct ClientConfig
    {
        std::string endpoint{"tcp://127.0.0.1:5555"};
        std::string clientId{"golddigger"};
        int sendTimeoutMs{2000};
        int receiveTimeoutMs{5000};
    };

    struct SpreadQuote
    {
        std::string instrument;
        double bid{0.0};
        double ask{0.0};
        double spread{0.0};
        double spreadPoints{0.0};
        std::uint64_t timestampMs{0};
    };

    struct TradeRequest
    {
        std::string instrument{"xauusd"};
        double volumeLots{0.0};
        std::optional<double> stopLoss;
        std::optional<double> takeProfit;
        std::optional<double> price;
        int deviationPoints{20};
        std::uint64_t magic{0};
        std::string comment{"golddigger"};
    };

    struct TradeResult
    {
        bool accepted{false};
        std::optional<long long> orderTicket;
        std::optional<long long> dealTicket;
        std::optional<long long> positionTicket;
        std::optional<double> fillPrice;
        std::string message;
        std::string externalCode;
    };

    struct CloseAllRequest
    {
        std::optional<std::string> instrument;
        std::optional<std::uint64_t> magic;
    };

    struct CloseAllResult
    {
        std::size_t closedCount{0};
        std::string message;
    };

    std::string to_string(Command command);
    bool parse_command(const std::string &value, Command &command);
    std::string generate_request_id();

    nlohmann::json build_request_envelope(
        Command command,
        const nlohmann::json &payload,
        const std::string &clientId,
        const std::string &requestId = {});
    void validate_response_envelope(
        const nlohmann::json &response,
        Command expectedCommand,
        const std::string &expectedRequestId);

    nlohmann::json build_ping_payload();
    nlohmann::json build_spread_payload(const std::string &instrument);
    nlohmann::json build_live_data_payload(const CandleFetchRequest &request);
    nlohmann::json build_trade_payload(const TradeRequest &request);
    nlohmann::json build_close_all_payload(const CloseAllRequest &request);

    std::uint64_t parse_timestamp_ms(const nlohmann::json &value, const char *fieldName);
    std::vector<Candle> parse_candle_array(const nlohmann::json &candlesJson);
    SpreadQuote parse_spread_quote(const nlohmann::json &payload);
    TradeResult parse_trade_result(const nlohmann::json &payload);
    CloseAllResult parse_close_all_result(const nlohmann::json &payload);
}
