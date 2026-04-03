#include "MarketData/dukascopy_cli_provider.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>

namespace
{
    std::string shell_quote(const std::string &value)
    {
        std::string escaped{"'"};
        for (const char ch : value)
        {
            if (ch == '\'')
                escaped += "'\\''";
            else
                escaped += ch;
        }
        escaped += '\'';
        return escaped;
    }

    std::string format_cli_timestamp(std::uint64_t timestampMs)
    {
        return format_utc_timestamp(timestampMs).substr(0, 16);
    }

    std::string run_command(const std::string &command)
    {
        std::array<char, 4096> buffer{};
        std::string output;

        FILE *pipe = popen(command.c_str(), "r");
        if (pipe == nullptr)
            throw std::runtime_error("Failed to execute command: " + command);

        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
            output += buffer.data();

        const int status = pclose(pipe);
        if (status == -1)
            throw std::runtime_error("Failed to close command stream: " + command);

        if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
        {
            std::ostringstream error;
            error << "Command failed (" << status << "): " << command;
            if (!output.empty())
                error << '\n'
                      << output;
            throw std::runtime_error(error.str());
        }

        return output;
    }

    std::string extract_json_payload(const std::string &output)
    {
        const auto start = output.find('[');
        const auto end = output.rfind(']');

        if ((start == std::string::npos) || (end == std::string::npos) || (end < start))
            throw std::runtime_error("Could not find JSON candle payload in Dukascopy output.");

        return output.substr(start, end - start + 1);
    }

    std::vector<Candle> parse_candles(const std::string &payload)
    {
        try
        {
            const auto json = nlohmann::json::parse(payload);
            if (!json.is_array())
                throw std::runtime_error("Dukascopy JSON payload is not an array.");

            std::vector<Candle> candles;
            candles.reserve(json.size());

            for (const auto &item : json)
            {
                candles.push_back(Candle{
                    item.at("timestamp").get<std::uint64_t>(),
                    item.at("open").get<double>(),
                    item.at("high").get<double>(),
                    item.at("low").get<double>(),
                    item.at("close").get<double>(),
                    item.at("volume").get<double>()});
            }

            return candles;
        }
        catch (const nlohmann::json::exception &e)
        {
            throw std::runtime_error("Failed to parse Dukascopy JSON payload: " + std::string(e.what()));
        }
    }
}

DukascopyCliDataProvider::DukascopyCliDataProvider(std::string command)
    : m_command(std::move(command))
{
}

std::vector<Candle> DukascopyCliDataProvider::fetchCandles(const CandleFetchRequest &request) const
{
    std::ostringstream command;
    command << m_command
            << " -i " << shell_quote(request.instrument)
            << " -from " << shell_quote(format_cli_timestamp(request.fromTimestampMs))
            << " -to " << shell_quote(format_cli_timestamp(request.toTimestampMs))
            << " -t " << shell_quote(to_string(request.timeframe))
            << " -f json";

    if (request.includeVolumes)
        command << " --volumes";

    command << " 2>&1";

    const auto output = run_command(command.str());
    auto candles = parse_candles(extract_json_payload(output));

    std::sort(candles.begin(), candles.end(),
              [](const Candle &lhs, const Candle &rhs)
              {
                  return lhs.timestamp < rhs.timestamp;
              });

    return candles;
}
