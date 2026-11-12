#include "doctest.h"

#include "MT5/mt5_protocol.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("request envelope carries protocol metadata")
{
    const auto envelope = mt5::build_request_envelope(
        mt5::Command::Ping, mt5::build_ping_payload(), "test-client", "req-1");

    REQUIRE(envelope.at("protocol").get<std::string>() == "golddigger.mt5.zmq");
    REQUIRE(envelope.at("version").get<int>() == 1);
    REQUIRE(envelope.at("type").get<std::string>() == "request");
    REQUIRE(envelope.at("request_id").get<std::string>() == "req-1");
    REQUIRE(envelope.at("client_id").get<std::string>() == "test-client");
    REQUIRE(envelope.at("command").get<std::string>() == "PING");
    REQUIRE(envelope.contains("sent_at_utc_ms"));

    const auto autoId = mt5::build_request_envelope(
        mt5::Command::Ping, mt5::build_ping_payload(), "test-client", {});
    REQUIRE_FALSE(autoId.at("request_id").get<std::string>().empty());
}

TEST_CASE("response envelope validation accepts OK and SUCCESS")
{
    const auto payload = nlohmann::json{{"heartbeat", "PING"}};
    const auto ok = nlohmann::json{
        {"protocol", "golddigger.mt5.zmq"},
        {"version", 1},
        {"type", "response"},
        {"request_id", "r1"},
        {"command", "PING"},
        {"status", "OK"},
        {"payload", payload}};
    REQUIRE_NOTHROW(mt5::validate_response_envelope(ok, mt5::Command::Ping, "r1"));

    auto success = ok;
    success["status"] = "SUCCESS";
    REQUIRE_NOTHROW(mt5::validate_response_envelope(success, mt5::Command::Ping, "r1"));
}

TEST_CASE("response envelope validation rejects mismatches")
{
    const auto ok = nlohmann::json{
        {"protocol", "golddigger.mt5.zmq"},
        {"version", 1},
        {"type", "response"},
        {"request_id", "r1"},
        {"command", "PING"},
        {"status", "OK"},
        {"payload", nlohmann::json::object()}};

    auto wrongProtocol = ok;
    wrongProtocol["protocol"] = "other";
    REQUIRE_THROWS_AS(mt5::validate_response_envelope(wrongProtocol, mt5::Command::Ping, "r1"), std::runtime_error);

    auto wrongCommand = ok;
    wrongCommand["command"] = "BUY";
    REQUIRE_THROWS_AS(mt5::validate_response_envelope(wrongCommand, mt5::Command::Ping, "r1"), std::runtime_error);

    auto wrongRequestId = ok;
    wrongRequestId["request_id"] = "r2";
    REQUIRE_THROWS_AS(mt5::validate_response_envelope(wrongRequestId, mt5::Command::Ping, "r1"), std::runtime_error);
}

TEST_CASE("response envelope validation surfaces error codes and messages")
{
    const auto error = nlohmann::json{
        {"protocol", "golddigger.mt5.zmq"},
        {"version", 1},
        {"type", "response"},
        {"request_id", "r1"},
        {"command", "PING"},
        {"status", "ERROR"},
        {"error", {{"code", "BUY_FAILED"}, {"message", "insufficient funds"}}}};

    bool caught = false;
    try
    {
        mt5::validate_response_envelope(error, mt5::Command::Ping, "r1");
    }
    catch (const std::exception &e)
    {
        caught = true;
        const std::string message = e.what();
        REQUIRE(message.find("BUY_FAILED") != std::string::npos);
        REQUIRE(message.find("insufficient funds") != std::string::npos);
    }
    REQUIRE(caught);
}

TEST_CASE("parse_candle_array scales second timestamps into milliseconds")
{
    const auto candles = mt5::parse_candle_array(nlohmann::json::array({
        {{"timestamp", 1776354300}, {"open", 1.0}, {"high", 2.0}, {"low", 0.5}, {"close", 1.5}},
        {{"timestamp", 1776354301}, {"open", 2.0}, {"high", 3.0}, {"low", 1.5}, {"close", 2.5}, {"volume", 7.0}},
    }));

    REQUIRE(candles.size() == 2);
    REQUIRE(candles[0].timestamp == 1776354300000ULL);
    REQUIRE(candles[1].timestamp == 1776354301000ULL);
    REQUIRE(candles[1].volume == 7.0);
}

TEST_CASE("parse_candle_array keeps millisecond timestamps unchanged")
{
    const auto candles = mt5::parse_candle_array(nlohmann::json::array({{
        {"timestamp", 1776354300000ULL},
        {"open", 1.0},
        {"high", 2.0},
        {"low", 0.5},
        {"close", 1.5},
    }}));
    REQUIRE(candles.size() == 1);
    REQUIRE(candles[0].timestamp == 1776354300000ULL);
}

TEST_CASE("parse_candle_array sorts and de-duplicates")
{
    // Millisecond-scale timestamps (>= 1e12) are not rescaled, so ordering and
    // de-duplication can be asserted precisely.
    const auto candles = mt5::parse_candle_array(nlohmann::json::array({
        {{"timestamp", 1776354300000ULL}, {"open", 3.0}, {"high", 3.0}, {"low", 3.0}, {"close", 3.0}},
        {{"timestamp", 1776354000000ULL}, {"open", 1.0}, {"high", 1.0}, {"low", 1.0}, {"close", 1.0}},
        {{"timestamp", 1776354300000ULL}, {"open", 9.0}, {"high", 9.0}, {"low", 9.0}, {"close", 9.0}},
        {{"timestamp", 1776354150000ULL}, {"open", 2.0}, {"high", 2.0}, {"low", 2.0}, {"close", 2.0}},
    }));

    REQUIRE(candles.size() == 3);
    REQUIRE(candles[0].timestamp == 1776354000000ULL);
    REQUIRE(candles[1].timestamp == 1776354150000ULL);
    REQUIRE(candles[2].timestamp == 1776354300000ULL);
}

TEST_CASE("trade payload omits unset optional levels")
{
    mt5::TradeRequest trade;
    trade.instrument = "xauusd";
    trade.volumeLots = 0.1;

    const auto noLevels = mt5::build_trade_payload(trade);
    REQUIRE_FALSE(noLevels.contains("stop_loss"));
    REQUIRE_FALSE(noLevels.contains("take_profit"));
    REQUIRE_FALSE(noLevels.contains("price"));

    trade.stopLoss = 4800.0;
    trade.takeProfit = 4850.0;
    const auto withLevels = mt5::build_trade_payload(trade);
    REQUIRE(withLevels.at("stop_loss").get<double>() == 4800.0);
    REQUIRE(withLevels.at("take_profit").get<double>() == 4850.0);
}

TEST_CASE("parse_command is case insensitive")
{
    mt5::Command command{};
    REQUIRE(mt5::parse_command("PING", command));
    REQUIRE(command == mt5::Command::Ping);
    REQUIRE(mt5::parse_command("get_live_data", command));
    REQUIRE(command == mt5::Command::GetLiveData);
    REQUIRE(mt5::parse_command("close_all", command));
    REQUIRE(command == mt5::Command::CloseAll);
    REQUIRE_FALSE(mt5::parse_command("NOT_A_COMMAND", command));
}

TEST_CASE("spread quote parsing computes fallback spread")
{
    const auto quote = mt5::parse_spread_quote(
        nlohmann::json{{"instrument", "xauusd"}, {"bid", 4800.0}, {"ask", 4800.4}});
    REQUIRE(quote.spread == doctest::Approx(0.4).epsilon(1e-12));
}