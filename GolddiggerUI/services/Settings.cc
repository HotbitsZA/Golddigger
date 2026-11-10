#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace golddiggerui
{
  namespace fs = std::filesystem;

  namespace
  {
    std::string lowerCopy(std::string value)
    {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                     { return static_cast<char>(std::tolower(ch)); });
      return value;
    }

    std::string envValue(const char *name)
    {
      const char *value = std::getenv(name);
      return value != nullptr ? std::string(value) : std::string();
    }

    bool oneOf(const std::string &value, std::initializer_list<const char *> allowed)
    {
      for (const char *candidate : allowed)
      {
        if (value == candidate)
          return true;
      }
      return false;
    }

    void requireOneOf(const std::string &value,
                      std::initializer_list<const char *> allowed,
                      const char *field)
    {
      if (!oneOf(value, allowed))
        throw std::invalid_argument(std::string("'") + field + "' has an unsupported value: '" + value + "'");
    }

    /// Read a number from a JSON value, accepting numeric strings as the
    /// browser may send them.
    double readNumber(const nlohmann::json &patch, const char *field, double current)
    {
      if (!patch.contains(field))
        return current;
      const auto &value = patch[field];
      if (value.is_number())
        return value.get<double>();
      if (value.is_boolean())
        return value.get<bool>() ? 1.0 : 0.0;
      if (value.is_string())
      {
        const std::string text = value.get<std::string>();
        try
        {
          std::size_t used = 0;
          const double parsed = std::stod(text, &used);
          if (used != text.size())
            throw std::invalid_argument("trailing characters");
          return parsed;
        }
        catch (const std::exception &)
        {
          throw std::invalid_argument(std::string("'") + field + "' is not a number");
        }
      }
      throw std::invalid_argument(std::string("'") + field + "' is not a number");
    }

    int readInt(const nlohmann::json &patch, const char *field, int current)
    {
      if (!patch.contains(field))
        return current;
      const double value = readNumber(patch, field, static_cast<double>(current));
      if (!std::isfinite(value))
        throw std::invalid_argument(std::string("'") + field + "' has to be finite");
      return static_cast<int>(std::lround(value));
    }

    bool readBool(const nlohmann::json &patch, const char *field, bool current)
    {
      if (!patch.contains(field))
        return current;
      const auto &value = patch[field];
      if (value.is_boolean())
        return value.get<bool>();
      if (value.is_number())
        return value.get<double>() != 0.0;
      if (value.is_string())
      {
        const std::string text = lowerCopy(value.get<std::string>());
        if (text == "true" || text == "1" || text == "yes" || text == "on")
          return true;
        if (text == "false" || text == "0" || text == "no" || text == "off")
          return false;
      }
      throw std::invalid_argument(std::string("'") + field + "' is not a boolean");
    }

    std::string readString(const nlohmann::json &patch, const char *field, const std::string &current)
    {
      if (!patch.contains(field))
        return current;
      const auto &value = patch[field];
      if (value.is_string())
        return value.get<std::string>();
      if (value.is_null())
        return std::string();
      throw std::invalid_argument(std::string("'") + field + "' is not a string");
    }

    std::vector<std::string> readTimeframes(const nlohmann::json &patch,
                                            const std::vector<std::string> &current)
    {
      if (!patch.contains("autoTimeframes"))
        return current;
      const auto &value = patch["autoTimeframes"];
      if (!value.is_array())
        throw std::invalid_argument("'autoTimeframes' has to be an array");

      std::vector<std::string> result;
      for (const auto &entry : value)
      {
        if (!entry.is_string())
          throw std::invalid_argument("'autoTimeframes' entries have to be strings");
        const std::string timeframe = lowerCopy(entry.get<std::string>());
        requireOneOf(timeframe, {"m15", "h1", "d1"}, "autoTimeframes");
        if (std::find(result.begin(), result.end(), timeframe) == result.end())
          result.push_back(timeframe);
      }
      return result;
    }
  } // namespace

  Settings Settings::defaults()
  {
    Settings settings;
    settings.instrument = lowerCopy(envValue("GOLDDIGGER_INSTRUMENT").empty()
                                        ? settings.instrument
                                        : envValue("GOLDDIGGER_INSTRUMENT"));

    if (!envValue("DUKASCOPY_NODE_COMMAND").empty())
      settings.dukascopyCommand = envValue("DUKASCOPY_NODE_COMMAND");
    if (!envValue("ALPHAVANTAGE_API_KEY").empty())
      settings.alphaVantageApiKey = envValue("ALPHAVANTAGE_API_KEY");
    if (!envValue("GOLDDIGGER_MT5_ENDPOINT").empty())
      settings.mt5Endpoint = envValue("GOLDDIGGER_MT5_ENDPOINT");
    if (!envValue("GOLDDIGGER_MT5_CLIENT_ID").empty())
      settings.mt5ClientId = envValue("GOLDDIGGER_MT5_CLIENT_ID");
    return settings;
  }

  nlohmann::json Settings::toJson(bool includeSecrets) const
  {
    nlohmann::json json;
    json["instrument"] = instrument;
    json["dataSource"] = dataSource;
    json["provider"] = provider;
    json["executionMode"] = executionMode;
    json["autoArmed"] = autoArmed;
    json["paperTrading"] = paperTrading;
    json["volumeLots"] = volumeLots;
    json["estimatedSpread"] = estimatedSpread;
    json["pollSeconds"] = pollSeconds;
    json["availabilityDelaySeconds"] = availabilityDelaySeconds;
    json["autoTimeframes"] = autoTimeframes;
    json["alphaVantageApiKey"] = includeSecrets ? alphaVantageApiKey : std::string();
    json["alphaVantageApiKeySet"] = !alphaVantageApiKey.empty();
    json["alphaVantageBaseUrl"] = alphaVantageBaseUrl;
    json["dukascopyCommand"] = dukascopyCommand;
    json["mt5Endpoint"] = mt5Endpoint;
    json["mt5ClientId"] = mt5ClientId;
    json["mt5SendTimeoutMs"] = mt5SendTimeoutMs;
    json["mt5ReceiveTimeoutMs"] = mt5ReceiveTimeoutMs;
    json["magic"] = magic;
    json["stopLossDistance"] = stopLossDistance;
    json["takeProfitDistance"] = takeProfitDistance;
    return json;
  }

  void Settings::applyJson(const nlohmann::json &patch, bool &liveAuthorised)
  {
    if (!patch.is_object())
      throw std::invalid_argument("the settings patch has to be a JSON object");

    const Settings previous = *this;

    instrument = lowerCopy(readString(patch, "instrument", instrument));
    if (instrument.empty())
      throw std::invalid_argument("'instrument' must not be empty");

    dataSource = lowerCopy(readString(patch, "dataSource", dataSource));
    requireOneOf(dataSource, {"file", "live", "auto"}, "dataSource");

    provider = lowerCopy(readString(patch, "provider", provider));
    requireOneOf(provider, {"dukascopy", "alphavantage", "mt5"}, "provider");

    executionMode = lowerCopy(readString(patch, "executionMode", executionMode));
    requireOneOf(executionMode, {"manual", "auto"}, "executionMode");

    autoArmed = readBool(patch, "autoArmed", autoArmed);
    paperTrading = readBool(patch, "paperTrading", paperTrading);

    if (patch.contains("confirmLive") && patch["confirmLive"].is_boolean())
      liveAuthorised = patch["confirmLive"].get<bool>();

    if (!paperTrading && !liveAuthorised)
      throw std::invalid_argument(
          "live trading is disabled: send confirmLive=true after confirming the venue");

    volumeLots = readNumber(patch, "volumeLots", volumeLots);
    if (!(volumeLots > 0.0) || volumeLots > 1000.0)
      throw std::invalid_argument("'volumeLots' has to be between 0 and 1000");

    estimatedSpread = readNumber(patch, "estimatedSpread", estimatedSpread);
    if (estimatedSpread < 0.0 || estimatedSpread > 10000.0)
      throw std::invalid_argument("'estimatedSpread' has to be between 0 and 10000");

    pollSeconds = readInt(patch, "pollSeconds", pollSeconds);
    if (pollSeconds < 5 || pollSeconds > 3600)
      throw std::invalid_argument("'pollSeconds' has to be between 5 and 3600");

    availabilityDelaySeconds = readInt(patch, "availabilityDelaySeconds", availabilityDelaySeconds);
    if (availabilityDelaySeconds < 0 || availabilityDelaySeconds > 3600)
      throw std::invalid_argument("'availabilityDelaySeconds' has to be between 0 and 3600");

    autoTimeframes = readTimeframes(patch, autoTimeframes);
    if (autoTimeframes.empty())
      autoTimeframes = {"m15"};

    const std::string apiKey = readString(patch, "alphaVantageApiKey", alphaVantageApiKey);
    if (patch.contains("alphaVantageApiKey"))
      alphaVantageApiKey = apiKey;

    alphaVantageBaseUrl = readString(patch, "alphaVantageBaseUrl", alphaVantageBaseUrl);
    dukascopyCommand = readString(patch, "dukascopyCommand", dukascopyCommand);
    mt5Endpoint = readString(patch, "mt5Endpoint", mt5Endpoint);
    mt5ClientId = readString(patch, "mt5ClientId", mt5ClientId);

    mt5SendTimeoutMs = readInt(patch, "mt5SendTimeoutMs", mt5SendTimeoutMs);
    if (mt5SendTimeoutMs < 100 || mt5SendTimeoutMs > 120000)
      throw std::invalid_argument("'mt5SendTimeoutMs' has to be between 100 and 120000");

    mt5ReceiveTimeoutMs = readInt(patch, "mt5ReceiveTimeoutMs", mt5ReceiveTimeoutMs);
    if (mt5ReceiveTimeoutMs < 100 || mt5ReceiveTimeoutMs > 120000)
      throw std::invalid_argument("'mt5ReceiveTimeoutMs' has to be between 100 and 120000");

    magic = static_cast<long long>(readNumber(patch, "magic", static_cast<double>(magic)));
    if (magic < 0)
      throw std::invalid_argument("'magic' has to be positive");

    stopLossDistance = readNumber(patch, "stopLossDistance", stopLossDistance);
    takeProfitDistance = readNumber(patch, "takeProfitDistance", takeProfitDistance);
    if (stopLossDistance < 0.0 || takeProfitDistance < 0.0)
      throw std::invalid_argument("stop loss and take profit distances have to be positive");

    if (!paperTrading && mt5Endpoint.empty())
    {
      *this = previous;
      throw std::invalid_argument("live trading requires an MT5 endpoint");
    }
  }

  SettingsStore::SettingsStore(std::string path)
      : m_path(std::move(path)),
        m_settings(Settings::defaults())
  {
    load();
  }

  void SettingsStore::load()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings = Settings::defaults();

    std::error_code ec;
    if (!fs::is_regular_file(m_path, ec))
      return;

    std::ifstream in(m_path);
    if (!in)
      return;

    nlohmann::json json;
    try
    {
      in >> json;
    }
    catch (const std::exception &)
    {
      return;
    }
    if (!json.is_object())
      return;

    try
    {
      bool authorised = true; // already stored settings are trusted
      m_settings.applyJson(json, authorised);
    }
    catch (const std::exception &)
    {
      // Keep the defaults when the file is invalid.
    }
  }

  void SettingsStore::save() const
  {
    const fs::path path(m_path);
    if (path.has_parent_path())
      fs::create_directories(path.parent_path());

    const fs::path temporary = path.string() + ".tmp";
    {
      std::ofstream out(temporary, std::ios::trunc);
      if (!out)
        throw std::runtime_error("failed to write the settings file: " + m_path);
      out << m_settings.toJson(true).dump(2) << '\n';
    }

    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec)
    {
      fs::remove(temporary, ec);
      throw std::runtime_error("failed to replace the settings file: " + m_path);
    }
  }

  Settings SettingsStore::get() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
  }

  Settings SettingsStore::update(const nlohmann::json &patch, bool liveAuthorised)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    Settings updated = m_settings;
    updated.applyJson(patch, liveAuthorised);
    m_settings = updated;
    save();
    return m_settings;
  }

  void SettingsStore::replace(const Settings &settings)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings = settings;
    save();
  }
} // namespace golddiggerui
