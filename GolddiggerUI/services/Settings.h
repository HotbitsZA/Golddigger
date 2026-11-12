#pragma once

/**
  GolddiggerUI - the persisted UI settings.

  The settings are a single JSON document in the state directory. There is no
  database. Every field has a sane default, so a missing or partial file keeps
  working.

  The trading related settings deliberately default to the safe side:
  `executionMode` is manual, `paperTrading` is true and automatic orders are
  not armed.
*/

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace golddiggerui
{
  struct Settings
  {
    std::string instrument{"xauusd"};

    /// "file" (rolling CSV), "live" (provider only) or "auto" (live with a
    /// fallback to the files).
    std::string dataSource{"file"};

    /// "dukascopy", "alphavantage" or "mt5".
    std::string provider{"dukascopy"};

    /// "manual" or "auto".
    std::string executionMode{"manual"};

    /// Automatic orders have to be armed explicitly on top of "auto".
    bool autoArmed{false};

    /// Paper trading only writes the order log; no broker is contacted.
    bool paperTrading{true};

    double volumeLots{0.10};
    double estimatedSpread{0.40};

    int pollSeconds{15};
    int availabilityDelaySeconds{60};

    /// Timeframes the background auto trader evaluates.
    std::vector<std::string> autoTimeframes{"m15"};

    std::string alphaVantageApiKey;
    std::string alphaVantageBaseUrl{"https://www.alphavantage.co/query"};
    std::string dukascopyCommand{"npx dukascopy-node -s"};

    std::string mt5Endpoint{"tcp://127.0.0.1:5555"};
    std::string mt5ClientId{"golddigger-ui"};
    int mt5SendTimeoutMs{2000};
    int mt5ReceiveTimeoutMs{5000};

    long long magic{20261110};
    double stopLossDistance{0.0};
    double takeProfitDistance{0.0};

    /// JSON representation. With `includeSecrets` false (the default, used by
    /// the API) the Alpha Vantage key is never sent back; only whether one is
    /// configured. The settings file is written with secrets included.
    nlohmann::json toJson(bool includeSecrets = false) const;

    /// Apply a partial JSON document. Throws std::invalid_argument with a
    /// readable message when a value is invalid.
    void applyJson(const nlohmann::json &patch, bool &liveAuthorised);

    static Settings defaults();
  };

  class SettingsStore
  {
  public:
    explicit SettingsStore(std::string path);

    /// A thread safe copy of the current settings.
    Settings get() const;

    /// Apply a partial JSON patch, validate it and write the file.
    /// `liveAuthorised` has to be true when paper trading is switched off.
    Settings update(const nlohmann::json &patch, bool liveAuthorised);

    /// Replace the whole document (tests).
    void replace(const Settings &settings);

    const std::string &path() const { return m_path; }

  private:
    void load();
    void save() const;

    mutable std::mutex m_mutex;
    std::string m_path;
    Settings m_settings;
  };
} // namespace golddiggerui
