#include "MarketData/dukascopy_cli_provider.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>

namespace
{
    namespace fs = std::filesystem;

    std::vector<Candle> parse_candles(const std::string &payload);

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

    bool command_uses_explicit_utc(std::string command)
    {
        std::transform(command.begin(), command.end(), command.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::tolower(ch));
                       });

        return (command.find("-tz utc") != std::string::npos) ||
               (command.find("--time-zone utc") != std::string::npos) ||
               (command.find("-utc 0") != std::string::npos) ||
               (command.find("--utc-offset 0") != std::string::npos);
    }

    std::string format_cli_timestamp(std::uint64_t timestampMs, bool useUtc)
    {
        // dukascopy-node accepts naive date strings. When the command is configured with
        // explicit UTC flags we can pass UTC wall-clock timestamps directly; otherwise
        // we keep the previous local-time workaround for compatibility with patcher.
        return (useUtc ? format_utc_timestamp(timestampMs) : format_local_timestamp(timestampMs)).substr(0, 16);
    }

    std::string build_command_text(const std::string &baseCommand, const CandleFetchRequest &request)
    {
        const bool useUtcInputTimestamps = command_uses_explicit_utc(baseCommand);
        std::ostringstream command;
        command << baseCommand
                << " -i " << shell_quote(request.instrument)
                << " -from " << shell_quote(format_cli_timestamp(request.fromTimestampMs, useUtcInputTimestamps))
                << " -to " << shell_quote(format_cli_timestamp(request.toTimestampMs, useUtcInputTimestamps))
                << " -t " << shell_quote(to_string(request.timeframe))
                << " -f csv";

        if (request.includeVolumes)
            command << " --volumes";

        command << " 2>&1";
        return command.str();
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

    bool looks_like_node_heap_oom(const std::string &message)
    {
        return (message.find("heap out of memory") != std::string::npos) ||
               (message.find("Allocation failed - JavaScript heap out of memory") != std::string::npos);
    }

    bool has_explicit_node_heap_limit(const std::string &command)
    {
        return (command.find("NODE_OPTIONS") != std::string::npos) ||
               (command.find("max-old-space-size") != std::string::npos);
    }

    std::string with_node_heap_limit(const std::string &command, std::size_t megabytes)
    {
        return "NODE_OPTIONS=--max-old-space-size=" + std::to_string(megabytes) + ' ' + command;
    }

    std::string extract_json_payload(const std::string &output)
    {
        const auto start = output.find('[');
        const auto end = output.rfind(']');

        if ((start == std::string::npos) || (end == std::string::npos) || (end < start))
            throw std::runtime_error("Could not find JSON candle payload in Dukascopy output.");

        return output.substr(start, end - start + 1);
    }

    std::string trim_copy(std::string value)
    {
        const auto notSpace = [](unsigned char ch)
        {
            return !std::isspace(ch);
        };

        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    std::vector<std::string> split_csv_fields(const std::string &line)
    {
        std::vector<std::string> fields;
        std::stringstream input(line);
        std::string field;

        while (std::getline(input, field, ','))
            fields.push_back(trim_copy(field));

        return fields;
    }

    std::string lower_copy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::tolower(ch));
                       });
        return value;
    }

    bool looks_like_csv_header(const std::vector<std::string> &fields)
    {
        if (fields.size() < 5)
            return false;

        const auto first = lower_copy(fields[0]);
        return ((first == "timestamp") || (first == "date") || (first == "time")) &&
               (lower_copy(fields[1]) == "open") &&
               (lower_copy(fields[2]) == "high") &&
               (lower_copy(fields[3]) == "low") &&
               (lower_copy(fields[4]) == "close");
    }

    std::optional<std::uint64_t> parse_csv_timestamp_ms(const std::string &value)
    {
        const auto trimmed = trim_copy(value);
        if (trimmed.empty())
            return std::nullopt;

        const bool isNumeric = std::all_of(trimmed.begin(), trimmed.end(),
                                           [](unsigned char ch)
                                           {
                                               return std::isdigit(ch);
                                           });
        if (isNumeric)
            return static_cast<std::uint64_t>(std::stoull(trimmed));

        for (const char *format : {"%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y.%m.%d %H:%M:%S", "%Y.%m.%d %H:%M"})
        {
            std::tm tm{};
            std::istringstream input(trimmed);
            input >> std::get_time(&tm, format);
            if (input.fail())
                continue;

            tm.tm_isdst = 0;
            const auto seconds = timegm(&tm);
            if (seconds >= 0)
                return static_cast<std::uint64_t>(seconds) * 1000ULL;
        }

        return std::nullopt;
    }

    std::optional<Candle> parse_csv_candle_row(const std::vector<std::string> &fields)
    {
        if (fields.size() < 5)
            return std::nullopt;

        const auto timestampMs = parse_csv_timestamp_ms(fields[0]);
        if (!timestampMs.has_value())
            return std::nullopt;

        Candle candle{};
        candle.timestamp = *timestampMs;
        candle.open = std::stod(fields[1]);
        candle.high = std::stod(fields[2]);
        candle.low = std::stod(fields[3]);
        candle.close = std::stod(fields[4]);
        candle.volume = (fields.size() > 5) ? std::stod(fields[5]) : 0.0;
        return candle;
    }

    std::vector<Candle> parse_csv_candles(const std::string &output)
    {
        std::vector<Candle> candles;
        std::istringstream input(output);
        std::string line;
        bool sawHeader = false;

        while (std::getline(input, line))
        {
            if (!line.empty() && (line.back() == '\r'))
                line.pop_back();

            const auto trimmed = trim_copy(line);
            if (trimmed.empty())
                continue;

            const auto fields = split_csv_fields(trimmed);
            if (fields.empty())
                continue;

            if (looks_like_csv_header(fields))
            {
                sawHeader = true;
                continue;
            }

            try
            {
                const auto candle = parse_csv_candle_row(fields);
                if (candle.has_value())
                    candles.push_back(*candle);
            }
            catch (const std::exception &)
            {
                continue;
            }
        }

        if (!candles.empty() || sawHeader)
            return candles;

        throw std::runtime_error("Could not find JSON or CSV candle payload in Dukascopy output.");
    }

    std::string sanitize_filename_component(std::string value)
    {
        for (char &ch : value)
        {
            const bool keep =
                ((ch >= 'a') && (ch <= 'z')) ||
                ((ch >= 'A') && (ch <= 'Z')) ||
                ((ch >= '0') && (ch <= '9')) ||
                (ch == '-') ||
                (ch == '_');
            if (!keep)
                ch = '_';
        }

        return value;
    }

    std::optional<fs::path> extract_saved_file_path(const std::string &output)
    {
        const std::string marker = "File saved:";
        const auto markerPosition = output.find(marker);
        if (markerPosition == std::string::npos)
            return std::nullopt;

        auto start = markerPosition + marker.size();
        while ((start < output.size()) && std::isspace(static_cast<unsigned char>(output[start])))
            ++start;

        auto end = output.find('\n', start);
        if (end == std::string::npos)
            end = output.size();

        auto line = trim_copy(output.substr(start, end - start));
        const auto bytesMarker = line.rfind(" (");
        if (bytesMarker != std::string::npos)
            line = trim_copy(line.substr(0, bytesMarker));

        if (line.empty())
            return std::nullopt;

        return fs::path(line);
    }

    std::string debug_file_stem(const CandleFetchRequest &request)
    {
        std::ostringstream stem;
        stem << sanitize_filename_component(request.instrument)
             << '_' << to_string(request.timeframe)
             << '_' << request.fromTimestampMs
             << '_' << request.toTimestampMs;
        return stem.str();
    }

    void write_text_file(const fs::path &path, const std::string &content)
    {
        std::ofstream output(path, std::ios::trunc);
        if (!output)
            throw std::runtime_error("Failed to write debug file: " + path.string());

        output << content;
    }

    fs::path write_debug_output(
        const std::string &debugDirectory,
        const CandleFetchRequest &request,
        const std::string &command,
        const std::string &output)
    {
        const fs::path directory(debugDirectory);
        fs::create_directories(directory);

        const auto stem = debug_file_stem(request);
        write_text_file(directory / (stem + ".command.txt"), command);
        write_text_file(directory / (stem + ".output.txt"), output);

        return directory;
    }

    std::string read_text_file(const fs::path &path)
    {
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("Failed to read Dukascopy saved file: " + path.string());

        std::ostringstream content;
        content << input.rdbuf();
        return content.str();
    }

    bool is_effectively_empty_payload(const std::string &payload)
    {
        return trim_copy(payload).empty();
    }

    std::vector<Candle> parse_any_candles(const std::string &payload)
    {
        try
        {
            return parse_candles(extract_json_payload(payload));
        }
        catch (const std::exception &jsonError)
        {
            try
            {
                return parse_csv_candles(payload);
            }
            catch (const std::exception &csvError)
            {
                throw std::runtime_error(
                    std::string(jsonError.what()) + " Fallback CSV parse also failed: " + csvError.what());
            }
        }
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

DukascopyCliDataProvider::DukascopyCliDataProvider(std::string command, std::string debugDirectory)
    : m_command(std::move(command)),
      m_debugDirectory(std::move(debugDirectory))
{
}

std::chrono::milliseconds DukascopyCliDataProvider::livePublicationInterval(CandleTimeframe timeframe) const
{
    if (timeframe == CandleTimeframe::M15)
        return std::chrono::hours{1};

    return candle_interval(timeframe);
}

std::string DukascopyCliDataProvider::describeRequest(const CandleFetchRequest &request) const
{
    return build_command_text(m_command, request);
}

std::vector<Candle> DukascopyCliDataProvider::fetchCandles(const CandleFetchRequest &request) const
{
    const auto commandText = build_command_text(m_command, request);
    std::string output;
    try
    {
        output = run_command(commandText);
    }
    catch (const std::exception &commandError)
    {
        if (!looks_like_node_heap_oom(commandError.what()) || has_explicit_node_heap_limit(commandText))
            throw;

        output = run_command(with_node_heap_limit(commandText, 4096));
    }
    std::optional<fs::path> debugPath;
    if (!m_debugDirectory.empty())
        debugPath = write_debug_output(m_debugDirectory, request, commandText, output);

    std::optional<fs::path> savedFilePath = extract_saved_file_path(output);
    std::vector<Candle> candles;
    try
    {
        candles = parse_any_candles(output);
    }
    catch (const std::exception &outputParseError)
    {
        try
        {
            if (!savedFilePath.has_value())
                throw;

            auto payloadPath = *savedFilePath;
            if (payloadPath.is_relative())
                payloadPath = fs::current_path() / payloadPath;

            const auto savedPayload = read_text_file(payloadPath);
            if (is_effectively_empty_payload(savedPayload))
                candles.clear();
            else
                candles = parse_any_candles(savedPayload);
        }
        catch (const std::exception &savedFileError)
        {
            std::ostringstream error;
            error << outputParseError.what();
            if (savedFilePath.has_value())
            {
                error << " Saved file parse also failed: " << savedFileError.what();
                error << " Saved file path: " << savedFilePath->string() << '.';
            }
            if (debugPath.has_value())
                error << " Raw Dukascopy command/output saved under " << debugPath->string() << '.';
            throw std::runtime_error(error.str());
        }
    }

    std::sort(candles.begin(), candles.end(),
              [](const Candle &lhs, const Candle &rhs)
              {
                  return lhs.timestamp < rhs.timestamp;
              });

    return candles;
}
