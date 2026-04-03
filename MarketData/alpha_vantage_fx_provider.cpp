#include "MarketData/alpha_vantage_fx_provider.h"

#include "cHTTPClient.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
    std::string upper_alnum_copy(const std::string &value)
    {
        std::string normalized;
        normalized.reserve(value.size());

        for (const unsigned char ch : value)
        {
            if (std::isalnum(ch))
                normalized += static_cast<char>(std::toupper(ch));
        }

        return normalized;
    }

    std::pair<std::string, std::string> split_fx_pair(const std::string &instrument)
    {
        const auto normalized = upper_alnum_copy(instrument);
        if (normalized.size() != 6)
            throw std::runtime_error("Alpha Vantage FX provider expects a 6-character pair such as XAUUSD.");

        return {normalized.substr(0, 3), normalized.substr(3, 3)};
    }

    std::string alpha_vantage_interval(CandleTimeframe timeframe)
    {
        switch (timeframe)
        {
        case CandleTimeframe::M15:
            return "15min";
        case CandleTimeframe::H1:
            return "60min";
        case CandleTimeframe::D1:
            break;
        }

        throw std::runtime_error("Alpha Vantage FX_INTRADAY currently supports m15 and h1 timeframes only.");
    }

    std::uint64_t parse_utc_timestamp_ms(const std::string &timestamp)
    {
        std::tm tm{};
        std::istringstream input(timestamp);
        input >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (input.fail())
            throw std::runtime_error("Failed to parse Alpha Vantage timestamp: " + timestamp);

        tm.tm_isdst = 0;
        const auto seconds = timegm(&tm);
        if (seconds < 0)
            throw std::runtime_error("Alpha Vantage timestamp conversion failed: " + timestamp);

        return static_cast<std::uint64_t>(seconds) * 1000ULL;
    }

    std::string build_query_url(const AlphaVantageFxProvider::Config &config,
                                const CandleFetchRequest &request)
    {
        const auto [fromSymbol, toSymbol] = split_fx_pair(request.instrument);
        const auto interval = alpha_vantage_interval(request.timeframe);

        std::ostringstream url;
        url << config.baseUrl
            << "?function=FX_INTRADAY"
            << "&from_symbol=" << fromSymbol
            << "&to_symbol=" << toSymbol
            << "&interval=" << interval
            << "&outputsize=" << config.outputSize
            << "&datatype=json"
            << "&apikey=" << config.apiKey;
        return url.str();
    }

    std::vector<Candle> parse_alpha_vantage_response(const std::string &body,
                                                     const CandleFetchRequest &request)
    {
        const auto json = nlohmann::json::parse(body);

        if (json.contains("Error Message"))
            throw std::runtime_error("Alpha Vantage error: " + json.at("Error Message").get<std::string>());

        if (json.contains("Information"))
            throw std::runtime_error("Alpha Vantage information: " + json.at("Information").get<std::string>());

        if (json.contains("Note"))
            throw std::runtime_error("Alpha Vantage note: " + json.at("Note").get<std::string>());

        const auto seriesKey = "Time Series FX (" + alpha_vantage_interval(request.timeframe) + ")";
        if (!json.contains(seriesKey))
            throw std::runtime_error("Alpha Vantage response does not contain " + seriesKey + ".");

        std::vector<Candle> candles;
        for (const auto &[timestamp, values] : json.at(seriesKey).items())
        {
            const auto timestampMs = parse_utc_timestamp_ms(timestamp);
            if ((timestampMs < request.fromTimestampMs) || (timestampMs >= request.toTimestampMs))
                continue;

            candles.push_back(Candle{
                timestampMs,
                std::stod(values.at("1. open").get<std::string>()),
                std::stod(values.at("2. high").get<std::string>()),
                std::stod(values.at("3. low").get<std::string>()),
                std::stod(values.at("4. close").get<std::string>()),
                0.0});
        }

        std::sort(candles.begin(), candles.end(),
                  [](const Candle &lhs, const Candle &rhs)
                  {
                      return lhs.timestamp < rhs.timestamp;
                  });

        return candles;
    }
}

AlphaVantageFxProvider::AlphaVantageFxProvider(Config config)
    : m_config(std::move(config))
{
}

std::vector<Candle> AlphaVantageFxProvider::fetchCandles(const CandleFetchRequest &request) const
{
    if (m_config.apiKey.empty())
        throw std::runtime_error("Alpha Vantage provider requires an API key.");

    cHTTPClient client;
    cHTTPClient::st_Request httpRequest;
    httpRequest.url = build_query_url(m_config, request);
    httpRequest.accept = "application/json";
    httpRequest.userAgent = "Golddigger/AlphaVantageFxProvider";

    const auto response = client.perform(httpRequest);
    if (!response.ok())
    {
        std::ostringstream error;
        error << "Alpha Vantage HTTP request failed with status " << response.statusCode;
        if (!response.errorMessage.empty())
            error << ": " << response.errorMessage;
        throw std::runtime_error(error.str());
    }

    return parse_alpha_vantage_response(response.body, request);
}
