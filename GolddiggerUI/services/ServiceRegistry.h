#pragma once

/**
  GolddiggerUI - the process wide service objects.

  The controller and the background auto trader share one SettingsStore,
  MarketDataService, ModelService and TradingService. They are created once,
  after the paths are known, and live for the rest of the process.
*/

#include <memory>

#include "services/AppPaths.h"
#include "services/MarketDataService.h"
#include "services/ModelService.h"
#include "services/Settings.h"
#include "services/TradingService.h"

namespace golddiggerui
{
  struct Services
  {
    std::unique_ptr<SettingsStore> settings;
    std::unique_ptr<MarketDataService> market;
    std::unique_ptr<ModelService> models;
    std::unique_ptr<TradingService> trading;

    /// The paths every service was built with.
    const AppPaths &paths() const { return market->paths(); }
  };

  /// Create (or recreate) the services for the given paths.
  void initServices(const AppPaths &paths);

  /// The service registry. Call initServices() first.
  Services &services();
} // namespace golddiggerui
