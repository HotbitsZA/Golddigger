#include "MT5/mt5_protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace
{
    std::string upper_copy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::toupper(ch));
                       });
        return value;
    }

    std::uint64_t now_timestamp_ms()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    std::string command_error_context(const char *fieldName)
    {
        return std::string("MT5 ZeroMQ protocol field ") + fieldName;
    }

    double required_number(const nlohmann::json &json, const char *fieldName)
    {
        if (!json.contains(fieldName))
            throw std::runtime_error(command_error_context(fieldName) + " is missing.");

        const auto &value = json.at(fieldName);
        if (value.is_number())
            return value.get<double>();
        if (value.is_string())
            return std::stod(value.get<std::string>());

        throw std::runtime_error(command_error_context(fieldName) + " must be numeric.");
    }

    std::optional<long long> optional_integer(const nlohmann::json &json, const char *fieldName)
    {
        if (!json.contains(fieldName) || json.at(fieldName).is_null())
            return std::nullopt;

        const auto &value = json.at(fieldName);
        if (value.is_number_integer() || value.is_number_unsigned())
            return value.get<long long>();
        if (value.is_string())
            return std::stoll(value.get<std::string>());

        throw std::runtime_error(command_error_context(fieldName) + " must be an integer.");
    }

    std::optional<double> optional_number(const nlohmann::json &json, const char *fieldName)
    {
        if (!json.contains(fieldName) || json.at(fieldName).is_null())
            return std::nullopt;

        const auto &value = json.at(fieldName);
        if (value.is_number())
            return value.get<double>();
        if (value.is_string())
            return std::stod(value.get<std::string>());

        throw std::runtime_error(command_error_context(fieldName) + " must be numeric.");
    }
}

namespace mt5
{
    std::string to_string(Command command)
    {
        switch (command)
        {
        case Command::Ping:
            return "PING";
        case Command::GetSpread:
            return "GET_SPREAD";
        case Command::GetLiveData:
            return "GET_LIVE_DATA";
        case Command::Buy:
            return "BUY";
        case Command::Sell:
            return "SELL";
        case Command::CloseAll:
            return "CLOSE_ALL";
        }

        throw std::runtime_error("Unknown MT5 command.");
    }

    bool parse_command(const std::string &value, Command &command)
    {
        const auto normalized = upper_copy(value);
        if (normalized == "PING")
            command = Command::Ping;
        else if (normalized == "GET_SPREAD")
            command = Command::GetSpread;
        else if (normalized == "GET_LIVE_DATA")
            command = Command::GetLiveData;
        else if (normalized == "BUY")
            command = Command::Buy;
        else if (normalized == "SELL")
            command = Command::Sell;
        else if (normalized == "CLOSE_ALL")
            command = Command::CloseAll;
        else
            return false;

        return true;
    }

    std::string generate_request_id()
    {
        static std::atomic<std::uint64_t> counter{0};
        std::ostringstream value;
        value << "gd-" << now_timestamp_ms() << '-' << counter.fetch_add(1, std::memory_order_relaxed);
        return value.str();
    }

    nlohmann::json build_request_envelope(
        Command command,
        const nlohmann::json &payload,
        const std::string &clientId,
        const std::string &requestId)
    {
        return nlohmann::json{
            {"protocol", kProtocolName},
            {"version", kProtocolVersion},
            {"type", "request"},
            {"request_id", requestId.empty() ? generate_request_id() : requestId},
            {"client_id", clientId},
            {"sent_at_utc_ms", now_timestamp_ms()},
            {"command", to_string(command)},
            {"payload", payload}};
    }

    void validate_response_envelope(
        const nlohmann::json &response,
        Command expectedCommand,
        const std::string &expectedRequestId)
    {
        if (!response.is_object())
            throw std::runtime_error("MT5 ZeroMQ response must be a JSON object.");

        if (response.value("protocol", "") != kProtocolName)
            throw std::runtime_error("MT5 ZeroMQ response protocol mismatch.");

        if (response.value("version", 0) != kProtocolVersion)
            throw std::runtime_error("MT5 ZeroMQ response version mismatch.");

        if (response.value("type", "") != "response")
            throw std::runtime_error("MT5 ZeroMQ response type mismatch.");

        if (response.value("command", "") != to_string(expectedCommand))
            throw std::runtime_error("MT5 ZeroMQ response command mismatch.");

        if (!expectedRequestId.empty() && (response.value("request_id", "") != expectedRequestId))
            throw std::runtime_error("MT5 ZeroMQ response request_id mismatch.");

        const auto status = upper_copy(response.value("status", "ERROR"));
        if ((status == "OK") || (status == "SUCCESS"))
            return;

        std::ostringstream error;
        error << "MT5 ZeroMQ request failed";
        if (response.contains("error") && response.at("error").is_object())
        {
            const auto &errorJson = response.at("error");
            const auto code = errorJson.value("code", "");
            const auto message = errorJson.value("message", "");
            if (!code.empty())
                error << " [" << code << ']';
            if (!message.empty())
                error << ": " << message;
        }
        else if (response.contains("message"))
        {
            error << ": " << response.at("message").get<std::string>();
        }

        throw std::runtime_error(error.str());
    }

    nlohmann::json build_ping_payload()
    {
        return nlohmann::json{
            {"heartbeat", "PING"}};
    }

    nlohmann::json build_spread_payload(const std::string &instrument)
    {
        return nlohmann::json{
            {"instrument", instrument}};
    }

    nlohmann::json build_live_data_payload(const CandleFetchRequest &request)
    {
        return nlohmann::json{
            {"instrument", request.instrument},
            {"timeframe", to_string(request.timeframe)},
            {"from_timestamp_ms", request.fromTimestampMs},
            {"to_timestamp_ms", request.toTimestampMs},
            {"include_volumes", request.includeVolumes}};
    }

    nlohmann::json build_trade_payload(const TradeRequest &request)
    {
        nlohmann::json payload{
            {"instrument", request.instrument},
            {"volume_lots", request.volumeLots},
            {"deviation_points", request.deviationPoints},
            {"magic", request.magic},
            {"comment", request.comment}};

        if (request.stopLoss.has_value())
            payload["stop_loss"] = *request.stopLoss;
        if (request.takeProfit.has_value())
            payload["take_profit"] = *request.takeProfit;
        if (request.price.has_value())
            payload["price"] = *request.price;

        return payload;
    }

    nlohmann::json build_close_all_payload(const CloseAllRequest &request)
    {
        nlohmann::json payload = nlohmann::json::object();
        if (request.instrument.has_value())
            payload["instrument"] = *request.instrument;
        if (request.magic.has_value())
            payload["magic"] = *request.magic;
        return payload;
    }

    std::uint64_t parse_timestamp_ms(const nlohmann::json &value, const char *fieldName)
    {
        std::uint64_t timestampMs = 0;
        if (value.is_number_integer() || value.is_number_unsigned())
            timestampMs = value.get<std::uint64_t>();
        else if (value.is_string())
            timestampMs = std::stoull(value.get<std::string>());
        else
            throw std::runtime_error(command_error_context(fieldName) + " must be an integer timestamp.");

        if (timestampMs < 1000000000000ULL)
            timestampMs *= 1000ULL;

        return timestampMs;
    }

    std::vector<Candle> parse_candle_array(const nlohmann::json &candlesJson)
    {
        if (!candlesJson.is_array())
            throw std::runtime_error("MT5 candle payload must be an array.");

        std::vector<Candle> candles;
        candles.reserve(candlesJson.size());

        for (const auto &entry : candlesJson)
        {
            if (!entry.is_object())
                throw std::runtime_error("MT5 candle entries must be objects.");

            const auto timestampKey = entry.contains("timestamp_ms") ? "timestamp_ms" : "timestamp";
            candles.push_back(Candle{
                parse_timestamp_ms(entry.at(timestampKey), timestampKey),
                required_number(entry, "open"),
                required_number(entry, "high"),
                required_number(entry, "low"),
                required_number(entry, "close"),
                entry.contains("volume") ? required_number(entry, "volume") : 0.0});
        }

        std::sort(candles.begin(), candles.end(),
                  [](const Candle &lhs, const Candle &rhs)
                  {
                      return lhs.timestamp < rhs.timestamp;
                  });
        candles.erase(
            std::unique(candles.begin(), candles.end(),
                        [](const Candle &lhs, const Candle &rhs)
                        {
                            return lhs.timestamp == rhs.timestamp;
                        }),
            candles.end());

        return candles;
    }

    SpreadQuote parse_spread_quote(const nlohmann::json &payload)
    {
        if (!payload.is_object())
            throw std::runtime_error("MT5 spread payload must be an object.");

        SpreadQuote quote;
        quote.instrument = payload.value("instrument", "");
        quote.bid = required_number(payload, "bid");
        quote.ask = required_number(payload, "ask");
        quote.spread = payload.contains("spread")
                           ? required_number(payload, "spread")
                           : (quote.ask - quote.bid);
        quote.spreadPoints = payload.contains("spread_points")
                                 ? required_number(payload, "spread_points")
                                 : quote.spread;
        if (payload.contains("timestamp_ms"))
            quote.timestampMs = parse_timestamp_ms(payload.at("timestamp_ms"), "timestamp_ms");

        return quote;
    }

    TradeResult parse_trade_result(const nlohmann::json &payload)
    {
        if (!payload.is_object())
            throw std::runtime_error("MT5 trade payload must be an object.");

        TradeResult result;
        result.accepted = payload.value("accepted", true);
        result.orderTicket = optional_integer(payload, "order_ticket");
        result.dealTicket = optional_integer(payload, "deal_ticket");
        result.positionTicket = optional_integer(payload, "position_ticket");
        result.fillPrice = optional_number(payload, "fill_price");
        result.message = payload.value("message", "");
        result.externalCode = payload.value("external_code", "");
        return result;
    }

    CloseAllResult parse_close_all_result(const nlohmann::json &payload)
    {
        if (!payload.is_object())
            throw std::runtime_error("MT5 close-all payload must be an object.");

        CloseAllResult result;
        result.closedCount = payload.value("closed_count", 0U);
        result.message = payload.value("message", "");
        return result;
    }
}
