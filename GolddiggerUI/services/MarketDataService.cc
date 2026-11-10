#include "MarketDataService.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include "MarketData/alpha_vantage_fx_provider.h"
#include "MarketData/dukascopy_cli_provider.h"
#include "MarketData/mt5_zeromq_provider.h"
#include "Utils/utils.h"
#include "services/HttpJson.h"

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

    bool contains(const std::string &haystack, const std::string &needle)
    {
      return haystack.find(needle) != std::string::npos;
    }

    std::vector<Candle> normalize(std::vector<Candle> candles, std::uint64_t upperBoundMs)
    {
      std::sort(candles.begin(), candles.end(), [](const Candle &lhs, const Candle &rhs)
                { return lhs.timestamp < rhs.timestamp; });

      candles.erase(std::unique(candles.begin(), candles.end(), [](const Candle &lhs, const Candle &rhs)
                                { return lhs.timestamp == rhs.timestamp; }),
                    candles.end());

      if (upperBoundMs > 0)
      {
        candles.erase(std::remove_if(candles.begin(), candles.end(), [upperBoundMs](const Candle &candle)
                                     { return candle.timestamp >= upperBoundMs; }),
                      candles.end());
      }
      return candles;
    }

    std::vector<Candle> keepLast(std::vector<Candle> candles, std::size_t count)
    {
      if (count == 0 || candles.size() <= count)
        return candles;
      return std::vector<Candle>(candles.end() - static_cast<std::ptrdiff_t>(count), candles.end());
    }
  } // namespace

  MarketDataService::MarketDataService(AppPaths paths)
      : m_paths(std::move(paths))
  {
  }

  std::string MarketDataService::dataPath(CandleTimeframe timeframe,
                                          const std::string &instrument) const
  {
    return (fs::path(m_paths.dataDir) /
            (lowerCopy(instrument) + "-" + to_string(timeframe) + "-bid.csv"))
        .string();
  }

  std::string MarketDataService::resolveExistingPath(CandleTimeframe timeframe,
                                                     const std::string &instrument) const
  {
    const std::string primary = dataPath(timeframe, instrument);
    std::error_code ec;
    if (fs::is_regular_file(primary, ec))
      return primary;

    const std::string instrumentTag = lowerCopy(instrument);
    const std::string timeframeTag = to_string(timeframe);

    std::string best;
    fs::file_time_type bestTime{};
    if (!fs::is_directory(m_paths.dataDir, ec))
      return primary;

    for (const auto &entry : fs::directory_iterator(m_paths.dataDir, ec))
    {
      if (ec)
        break;
      if (!entry.is_regular_file(ec))
        continue;

      const std::string name = lowerCopy(entry.path().filename().string());
      if (entry.path().extension() != ".csv" || !contains(name, instrumentTag) || !contains(name, timeframeTag))
        continue;

      const auto writeTime = fs::last_write_time(entry.path(), ec);
      if (best.empty() || (!ec && writeTime > bestTime))
      {
        best = entry.path().string();
        bestTime = writeTime;
      }
    }

    return best.empty() ? primary : best;
  }

  bool MarketDataService::hasData(CandleTimeframe timeframe, const std::string &instrument) const
  {
    std::error_code ec;
    return fs::is_regular_file(resolveExistingPath(timeframe, instrument), ec);
  }

  std::shared_ptr<const std::vector<Candle>> MarketDataService::allCandles(
      CandleTimeframe timeframe, const std::string &instrument) const
  {
    const std::string path = resolveExistingPath(timeframe, instrument);

    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
      return nullptr;

    const auto mtime = fs::last_write_time(path, ec);
    if (ec)
      return nullptr;
    const auto size = fs::file_size(path, ec);
    if (ec)
      return nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_cache.find(path);
    if (found != m_cache.end() && found->second.mtime == mtime && found->second.size == size)
      return found->second.candles;

    auto candles = std::make_shared<std::vector<Candle>>();
    read_gold_data(path, *candles);
    std::shared_ptr<const std::vector<Candle>> stored = candles;

    CacheEntry entry;
    entry.mtime = mtime;
    entry.size = size;
    entry.candles = stored;
    m_cache[path] = std::move(entry);
    return stored;
  }

  std::vector<Candle> MarketDataService::recentCandles(CandleTimeframe timeframe,
                                                       const std::string &instrument,
                                                       std::size_t count) const
  {
    const auto candles = allCandles(timeframe, instrument);
    if (!candles)
      return {};
    return keepLast(*candles, count);
  }

  std::unique_ptr<ICandleDataProvider> MarketDataService::makeProvider(const Settings &settings) const
  {
    if (settings.provider == "alphavantage")
    {
      if (settings.alphaVantageApiKey.empty())
        throw std::runtime_error(
            "Alpha Vantage is selected but no API key is configured. Set ALPHAVANTAGE_API_KEY "
            "or store a key in the UI settings.");

      AlphaVantageFxProvider::Config config;
      config.apiKey = settings.alphaVantageApiKey;
      config.baseUrl = settings.alphaVantageBaseUrl;
      config.outputSize = "compact";
      return std::make_unique<AlphaVantageFxProvider>(config);
    }

    if (settings.provider == "mt5")
    {
      mt5::ClientConfig config;
      config.endpoint = settings.mt5Endpoint;
      config.clientId = settings.mt5ClientId;
      config.sendTimeoutMs = settings.mt5SendTimeoutMs;
      config.receiveTimeoutMs = settings.mt5ReceiveTimeoutMs;
      return std::make_unique<Mt5ZeroMqProvider>(config);
    }

    return std::make_unique<DukascopyCliDataProvider>(settings.dukascopyCommand, std::string());
  }

  std::vector<Candle> MarketDataService::fetch(CandleTimeframe timeframe,
                                               const std::string &instrument,
                                               const Settings &settings,
                                               std::size_t count) const
  {
    const auto intervalMs = static_cast<std::uint64_t>(candle_interval(timeframe).count());
    if (intervalMs == 0)
      throw std::runtime_error("invalid candle interval");

    const auto now = nowMs();
    const auto delayMs = static_cast<std::uint64_t>(std::max(0, settings.availabilityDelaySeconds)) * 1000ULL;
    if (now <= delayMs)
      throw std::runtime_error("no completed candle is available yet");

    const std::uint64_t upperBoundMs = ((now - delayMs) / intervalMs) * intervalMs;
    if (upperBoundMs == 0)
      throw std::runtime_error("no completed candle is available yet");

    const std::size_t wanted = std::max<std::size_t>(count, 1);
    const std::size_t lookbackCandles = std::max<std::size_t>(wanted + 8, 16);
    const std::uint64_t lookbackMs = intervalMs * static_cast<std::uint64_t>(lookbackCandles);
    const std::uint64_t fromTimestampMs = (upperBoundMs > lookbackMs) ? (upperBoundMs - lookbackMs) : 0;

    CandleFetchRequest request;
    request.instrument = instrument;
    request.timeframe = timeframe;
    request.fromTimestampMs = fromTimestampMs;
    request.toTimestampMs = upperBoundMs;
    request.includeVolumes = true;

    auto provider = makeProvider(settings);
    auto candles = normalize(provider->fetchCandles(request), upperBoundMs);
    return keepLast(std::move(candles), wanted);
  }

  std::vector<Candle> MarketDataService::liveCandles(CandleTimeframe timeframe,
                                                     const std::string &instrument,
                                                     const Settings &settings,
                                                     std::size_t count) const
  {
    return fetch(timeframe, instrument, settings, count);
  }

  RefreshResult MarketDataService::refresh(CandleTimeframe timeframe,
                                           const std::string &instrument,
                                           const Settings &settings,
                                           std::size_t count,
                                           bool persist)
  {
    RefreshResult result;
    result.path = resolveExistingPath(timeframe, instrument);
    result.persisted = persist;

    auto fetched = fetch(timeframe, instrument, settings, std::max<std::size_t>(count, 64));
    result.fetched = fetched.size();
    result.candles = keepLast(fetched, std::max<std::size_t>(count, 1));

    if (!persist)
    {
      result.total = fetched.size();
      return result;
    }

    std::vector<Candle> merged;
    std::error_code ec;
    if (fs::is_regular_file(result.path, ec))
      read_gold_data(result.path, merged);

    const auto stats = merge_candles_by_timestamp(merged, fetched);
    result.added = stats.addedCount;
    result.replaced = stats.replacedCount;
    result.total = merged.size();
    result.candles = keepLast(merged, std::max<std::size_t>(count, 1));

    write_gold_data(result.path, merged);
    invalidate(result.path);
    return result;
  }

  void MarketDataService::invalidate(const std::string &path)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(path);
  }
} // namespace golddiggerui
