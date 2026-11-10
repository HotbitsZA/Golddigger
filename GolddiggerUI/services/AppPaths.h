#pragma once

/**
  GolddiggerUI - runtime paths.

  The UI is file backed: candles live in the Golddigger Data directory, the
  trained SVR models in the Models directory and the UI writes its own settings
  and order log into a state directory. There is no database.

  Every directory can be overridden with an environment variable or a command
  line option, so the same binary can be pointed at another data set:

      GOLDDIGGER_DATA_DIR      directory with <instrument>-<timeframe>-bid.csv
      GOLDDIGGER_MODELS_DIR    directory with the trained .dat models
      GOLDDIGGER_UI_WEB_DIR    directory with the static front end
      GOLDDIGGER_UI_STATE_DIR  directory for settings.json and orders.csv
*/

#include <string>

namespace golddiggerui
{
  struct AppPaths
  {
    std::string root;         ///< the Golddigger project root
    std::string dataDir;      ///< candle csv files
    std::string modelsDir;    ///< trained dlib models
    std::string webDir;       ///< static front end
    std::string stateDir;     ///< UI settings and order log
    std::string settingsFile; ///< <state>/settings.json
    std::string ordersFile;   ///< <state>/orders.csv
  };

  /// Build a path set from explicit directories. The settings and order files
  /// are derived from `stateDir`.
  AppPaths buildAppPaths(const std::string &root,
                         const std::string &dataDir,
                         const std::string &modelsDir,
                         const std::string &webDir,
                         const std::string &stateDir);

  /// Give the path resolver a hint about where the binary lives. Called from
  /// main() with argv[0] so the "next to the executable" fallbacks work.
  void initAppPaths(const std::string &argv0);

  /// Replace the process wide paths (command line options, tests).
  void setAppPaths(const AppPaths &paths);

  /// Resolve (once) and return the process wide paths.
  const AppPaths &appPaths();

  /// Create the state directory if it does not exist yet.
  void ensureStateDir(const AppPaths &paths);
} // namespace golddiggerui
