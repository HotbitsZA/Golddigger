#include "ServiceRegistry.h"

#include <stdexcept>

namespace golddiggerui
{
  namespace
  {
    std::unique_ptr<Services> g_services;
  } // namespace

  void initServices(const AppPaths &paths)
  {
    auto services = std::make_unique<Services>();

    services->settings = std::make_unique<SettingsStore>(paths.settingsFile);
    services->market = std::make_unique<MarketDataService>(paths);
    services->models = std::make_unique<ModelService>(paths);
    services->trading = std::make_unique<TradingService>(
        paths, *services->settings, *services->market, *services->models);

    g_services = std::move(services);
  }

  Services &services()
  {
    if (!g_services)
      throw std::runtime_error("GolddiggerUI services have not been initialised");
    return *g_services;
  }
} // namespace golddiggerui
