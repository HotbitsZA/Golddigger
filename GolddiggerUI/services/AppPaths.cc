#include "AppPaths.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

namespace golddiggerui
{
  namespace fs = std::filesystem;

  namespace
  {
    std::string envValue(const char *name)
    {
      const char *value = std::getenv(name);
      return value != nullptr ? std::string(value) : std::string();
    }

    /// Directory the executable was started from, best effort. argv[0] is
    /// resolved against the working directory; when that fails we simply keep
    /// the working directory.
    std::string executableDir(const std::string &argv0)
    {
      std::error_code ec;
      if (argv0.empty())
      {
        const auto cwd = fs::current_path(ec);
        return ec ? std::string(".") : cwd.string();
      }

      fs::path exe(argv0);
      if (exe.has_parent_path())
      {
        const auto parent = fs::absolute(exe.parent_path(), ec);
        if (!ec)
          return parent.string();
      }

      const auto cwd = fs::current_path(ec);
      return ec ? std::string(".") : cwd.string();
    }

    /// First existing directory from the candidate list, else the first
    /// non-empty entry. Empty candidates are skipped so an unset environment
    /// variable does not win over the "next to the executable" defaults. The
    /// returned directory may not exist yet (the state directory is created
    /// on start up).
    std::string firstExisting(const std::vector<std::string> &candidates)
    {
      std::error_code ec;
      std::string firstNonEmpty;
      for (const auto &candidate : candidates)
      {
        if (candidate.empty())
          continue;
        if (firstNonEmpty.empty())
          firstNonEmpty = candidate;
        if (fs::is_directory(candidate, ec))
          return candidate;
      }
      return firstNonEmpty;
    }

    AppPaths resolve(const std::string &exeDir)
    {
#ifdef GOLDDIGGER_ROOT
      const std::string rootDefault = GOLDDIGGER_ROOT;
#else
      const std::string rootDefault = "..";
#endif
#ifdef GOLDDIGGER_DATA_DIR
      const std::string dataDefault = GOLDDIGGER_DATA_DIR;
#else
      const std::string dataDefault;
#endif
#ifdef GOLDDIGGER_MODELS_DIR
      const std::string modelsDefault = GOLDDIGGER_MODELS_DIR;
#else
      const std::string modelsDefault;
#endif
#ifdef GOLDDIGGER_UI_WEB_DIR
      const std::string webDefault = GOLDDIGGER_UI_WEB_DIR;
#else
      const std::string webDefault;
#endif
#ifdef GOLDDIGGER_UI_STATE_DIR
      const std::string stateDefault = GOLDDIGGER_UI_STATE_DIR;
#else
      const std::string stateDefault;
#endif

      const std::string dataDir = firstExisting({
          envValue("GOLDDIGGER_DATA_DIR"),
          dataDefault,
          exeDir + "/../Data",
          exeDir + "/Data",
          "Data",
      });

      const std::string modelsDir = firstExisting({
          envValue("GOLDDIGGER_MODELS_DIR"),
          modelsDefault,
          exeDir + "/../Models",
          exeDir + "/Models",
          "Models",
      });

      const std::string webDir = firstExisting({
          envValue("GOLDDIGGER_UI_WEB_DIR"),
          webDefault,
          exeDir + "/../web",
          exeDir + "/web",
          "web",
      });

      const std::string stateDir = firstExisting({
          envValue("GOLDDIGGER_UI_STATE_DIR"),
          stateDefault,
          exeDir + "/../state",
          exeDir + "/state",
          "state",
      });

      return buildAppPaths(rootDefault, dataDir, modelsDir, webDir, stateDir);
    }

    AppPaths &instance()
    {
      static AppPaths paths = resolve(".");
      return paths;
    }

    bool initialised = false;
  } // namespace

  AppPaths buildAppPaths(const std::string &root,
                         const std::string &dataDir,
                         const std::string &modelsDir,
                         const std::string &webDir,
                         const std::string &stateDir)
  {
    AppPaths paths;
    paths.root = root;
    paths.dataDir = dataDir;
    paths.modelsDir = modelsDir;
    paths.webDir = webDir;
    paths.stateDir = stateDir;
    paths.settingsFile = (fs::path(stateDir) / "settings.json").string();
    paths.ordersFile = (fs::path(stateDir) / "orders.csv").string();
    return paths;
  }

  void initAppPaths(const std::string &argv0)
  {
    if (initialised)
      return;
    initialised = true;
    instance() = resolve(executableDir(argv0));
  }

  void setAppPaths(const AppPaths &paths)
  {
    initialised = true;
    instance() = paths;
  }

  const AppPaths &appPaths()
  {
    return instance();
  }

  void ensureStateDir(const AppPaths &paths)
  {
    std::error_code ec;
    if (!paths.stateDir.empty())
      fs::create_directories(paths.stateDir, ec);
  }
} // namespace golddiggerui
