#pragma once

/**
  GolddiggerUI - candle access.

  Candles come from two places and neither needs a database:

  * the rolling CSV files the rest of Golddigger already uses
    (`Data/<instrument>-<timeframe>-bid.csv`), and
  * the live providers (Dukascopy CLI, Alpha Vantage FX, MT5 over ZeroMQ).

  The service keeps a small mtime based cache of the parsed CSV files so the
  UI can ask for candle counts and the last five candles on every poll without
  re-reading a multi megabyte file each time.
*/

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MarketData/candle_data_provider.h"
#include "services/AppPaths.h"
#include "services/Settings.h"

namespace golddiggerui
{
  /// The outcome of a live fetch, optionally merged into the CSV file.
  struct RefreshResult
  {
    std::vector<Candle> candles; ///< the requested newest candles after the merge
    std::size_t fetched = 0;
    std::size_t added = 0;
    std::size_t replaced = 0;
    std::size_t total = 0; ///< rows in the file after the merge
    std::string path;
    bool persisted = false;
  };

  class MarketDataService
  {
  public:
    explicit MarketDataService(AppPaths paths);

    const AppPaths &paths() const { return m_paths; }

    /// `Data/<instrument>-<timeframe>-bid.csv`, or the best matching CSV that
    /// exists in the data directory. Returns the primary path when no file
    /// exists, so error messages can show where the data is expected.
    std::string dataPath(CandleTimeframe timeframe, const std::string &instrument) const;

    /// True when a CSV file can actually be read for this timeframe.
    bool hasData(CandleTimeframe timeframe, const std::string &instrument) const;

    /// The parsed file, shared so callers can read count/latest cheaply.
    /// Returns nullptr when the file does not exist.
    std::shared_ptr<const std::vector<Candle>> allCandles(CandleTimeframe timeframe,
                                                         const std::string &instrument) const;

    /// The newest `count` completed candles of the CSV file.
    std::vector<Candle> recentCandles(CandleTimeframe timeframe,
                                      const std::string &instrument,
                                      std::size_t count) const;

    /// Fetch the newest `count` completed candles from the configured provider.
    /// Throws std::runtime_error when the provider fails.
    std::vector<Candle> liveCandles(CandleTimeframe timeframe,
                                    const std::string &instrument,
                                    const Settings &settings,
                                    std::size_t count) const;

    /// Fetch live candles and, when `persist` is true, merge them into the CSV
    /// file (duplicate timestamps are replaced, not duplicated).
    RefreshResult refresh(CandleTimeframe timeframe,
                          const std::string &instrument,
                          const Settings &settings,
                          std::size_t count,
                          bool persist);

    /// Build the provider that the settings (and the environment defaults)
    /// select.
    std::unique_ptr<ICandleDataProvider> makeProvider(const Settings &settings) const;

    static std::string timeframeId(CandleTimeframe timeframe) { return to_string(timeframe); }

  private:
    struct CacheEntry
    {
      std::filesystem::file_time_type mtime{};
      std::uintmax_t size = 0;
      std::shared_ptr<const std::vector<Candle>> candles;
    };

    std::string resolveExistingPath(CandleTimeframe timeframe, const std::string &instrument) const;
    std::vector<Candle> fetch(CandleTimeframe timeframe,
                              const std::string &instrument,
                              const Settings &settings,
                              std::size_t count) const;
    void invalidate(const std::string &path);

    AppPaths m_paths;
    mutable std::mutex m_mutex;
    mutable std::map<std::string, CacheEntry> m_cache;
  };
} // namespace golddiggerui
