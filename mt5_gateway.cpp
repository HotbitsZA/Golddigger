#include "MT5/mt5_zeromq_client.h"
#include "MarketData/candle_data_provider.h"

#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace
{
    void print_usage()
    {
        std::cout << "Usage: mt5_gateway [--endpoint tcp://HOST:PORT] [--client-id ID]\n"
                  << "                   [--send-timeout-ms N] [--receive-timeout-ms N]\n"
                  << "                   COMMAND [options]\n\n"
                  << "Commands:\n"
                  << "  PING\n"
                  << "  GET_SPREAD --instrument SYMBOL\n"
                  << "  GET_LIVE_DATA --instrument SYMBOL --timeframe m15|h1|d1 --from-local \"YYYY-MM-DD HH:MM\" --to-local \"YYYY-MM-DD HH:MM\"\n"
                  << "  BUY --instrument SYMBOL --volume LOTS [--sl PRICE] [--tp PRICE] [--price PRICE] [--deviation-points N] [--magic N] [--comment TEXT]\n"
                  << "  SELL --instrument SYMBOL --volume LOTS [--sl PRICE] [--tp PRICE] [--price PRICE] [--deviation-points N] [--magic N] [--comment TEXT]\n"
                  << "  CLOSE_ALL [--instrument SYMBOL] [--magic N]\n";
    }

    std::uint64_t parse_local_timestamp_ms(const std::string &value)
    {
        for (const char *format : {"%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M"})
        {
            std::tm tm{};
            std::istringstream input(value);
            input >> std::get_time(&tm, format);
            if (input.fail())
                continue;

            tm.tm_isdst = -1;
            const auto seconds = std::mktime(&tm);
            if (seconds >= 0)
                return static_cast<std::uint64_t>(seconds) * 1000ULL;
        }

        throw std::runtime_error("Failed to parse local datetime: " + value);
    }

    const char *require_value(int &index, int argc, char *argv[], const char *option)
    {
        if (index + 1 >= argc)
            throw std::runtime_error(std::string("Missing value for ") + option);
        return argv[++index];
    }

    void print_candles(const std::vector<Candle> &candles)
    {
        std::cout << "Fetched " << candles.size() << " candle(s)\n";
        for (const auto &candle : candles)
        {
            std::cout << format_local_timestamp(candle.timestamp)
                      << " | O=" << candle.open
                      << " H=" << candle.high
                      << " L=" << candle.low
                      << " C=" << candle.close
                      << " V=" << candle.volume << '\n';
        }
    }
}

int main(int argc, char *argv[])
{
    try
    {
        mt5::ClientConfig config;
        std::optional<std::string> commandName;
        std::string instrument{"xauusd"};
        CandleTimeframe timeframe{CandleTimeframe::M15};
        std::optional<std::uint64_t> fromTimestampMs;
        std::optional<std::uint64_t> toTimestampMs;
        mt5::TradeRequest tradeRequest;
        mt5::CloseAllRequest closeAllRequest;

        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if (argument == "--help")
            {
                print_usage();
                return 0;
            }

            if (argument == "--endpoint")
                config.endpoint = require_value(i, argc, argv, "--endpoint");
            else if (argument == "--client-id")
                config.clientId = require_value(i, argc, argv, "--client-id");
            else if (argument == "--send-timeout-ms")
                config.sendTimeoutMs = std::stoi(require_value(i, argc, argv, "--send-timeout-ms"));
            else if (argument == "--receive-timeout-ms")
                config.receiveTimeoutMs = std::stoi(require_value(i, argc, argv, "--receive-timeout-ms"));
            else if ((argument == "PING") || (argument == "GET_SPREAD") || (argument == "GET_LIVE_DATA") ||
                     (argument == "BUY") || (argument == "SELL") || (argument == "CLOSE_ALL"))
                commandName = argument;
            else if (argument == "--instrument")
            {
                instrument = require_value(i, argc, argv, "--instrument");
                tradeRequest.instrument = instrument;
                closeAllRequest.instrument = instrument;
            }
            else if (argument == "--timeframe")
            {
                if (!parse_timeframe(require_value(i, argc, argv, "--timeframe"), timeframe))
                    throw std::runtime_error("Unsupported timeframe. Use m15, h1, or d1.");
            }
            else if (argument == "--from-local")
                fromTimestampMs = parse_local_timestamp_ms(require_value(i, argc, argv, "--from-local"));
            else if (argument == "--to-local")
                toTimestampMs = parse_local_timestamp_ms(require_value(i, argc, argv, "--to-local"));
            else if (argument == "--volume")
                tradeRequest.volumeLots = std::stod(require_value(i, argc, argv, "--volume"));
            else if (argument == "--sl")
                tradeRequest.stopLoss = std::stod(require_value(i, argc, argv, "--sl"));
            else if (argument == "--tp")
                tradeRequest.takeProfit = std::stod(require_value(i, argc, argv, "--tp"));
            else if (argument == "--price")
                tradeRequest.price = std::stod(require_value(i, argc, argv, "--price"));
            else if (argument == "--deviation-points")
                tradeRequest.deviationPoints = std::stoi(require_value(i, argc, argv, "--deviation-points"));
            else if (argument == "--magic")
            {
                const auto magic = static_cast<std::uint64_t>(std::stoull(require_value(i, argc, argv, "--magic")));
                tradeRequest.magic = magic;
                closeAllRequest.magic = magic;
            }
            else if (argument == "--comment")
                tradeRequest.comment = require_value(i, argc, argv, "--comment");
            else
                throw std::runtime_error("Unknown argument: " + argument);
        }

        if (!commandName.has_value())
        {
            print_usage();
            return 1;
        }

        tradeRequest.instrument = instrument;
        mt5::ZeroMqClient client(config);

        if (*commandName == "PING")
        {
            std::cout << client.ping().dump(2) << '\n';
            return 0;
        }

        if (*commandName == "GET_SPREAD")
        {
            const auto quote = client.getSpread(instrument);
            std::cout << "Instrument: " << quote.instrument << '\n'
                      << "Bid: " << quote.bid << '\n'
                      << "Ask: " << quote.ask << '\n'
                      << "Spread: " << quote.spread << '\n'
                      << "Spread points: " << quote.spreadPoints << '\n';
            return 0;
        }

        if (*commandName == "GET_LIVE_DATA")
        {
            if (!fromTimestampMs.has_value() || !toTimestampMs.has_value())
                throw std::runtime_error("GET_LIVE_DATA requires --from-local and --to-local.");

            const CandleFetchRequest request{
                instrument,
                timeframe,
                *fromTimestampMs,
                *toTimestampMs,
                true};
            print_candles(client.getLiveData(request));
            return 0;
        }

        if ((*commandName == "BUY") || (*commandName == "SELL"))
        {
            if (tradeRequest.volumeLots <= 0.0)
                throw std::runtime_error("BUY and SELL require --volume > 0.");

            const auto result = (*commandName == "BUY") ? client.buy(tradeRequest) : client.sell(tradeRequest);
            std::cout << "Accepted: " << (result.accepted ? "true" : "false") << '\n';
            if (result.orderTicket.has_value())
                std::cout << "Order ticket: " << *result.orderTicket << '\n';
            if (result.dealTicket.has_value())
                std::cout << "Deal ticket: " << *result.dealTicket << '\n';
            if (result.positionTicket.has_value())
                std::cout << "Position ticket: " << *result.positionTicket << '\n';
            if (result.fillPrice.has_value())
                std::cout << "Fill price: " << *result.fillPrice << '\n';
            if (!result.externalCode.empty())
                std::cout << "External code: " << result.externalCode << '\n';
            if (!result.message.empty())
                std::cout << "Message: " << result.message << '\n';
            return 0;
        }

        const auto result = client.closeAll(closeAllRequest);
        std::cout << "Closed count: " << result.closedCount << '\n';
        if (!result.message.empty())
            std::cout << "Message: " << result.message << '\n';
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
