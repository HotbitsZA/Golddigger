#pragma once

/**
  GolddiggerUI - the JSON API.

  Everything the front end needs is served from here. There is no database:
  candles come from the rolling CSV files or a live provider, models are the
  `.dat` files in the models directory, and orders/settings are plain files in
  the state directory.

      GET  /api/status                  paths, settings, provider readiness
      GET  /api/timeframes              timeframe -> model/data mapping
      GET  /api/models                  every trained model
      GET  /api/candles                 the newest candles (file or live)
      POST /api/candles/refresh         fetch live candles, optionally persist
      GET  /api/signal                  prediction for the selected timeframe
      POST /api/signal/evaluate         signal + optional automatic execution
      GET  /api/orders                  the order log
      POST /api/orders                  place a manual order
      GET  /api/settings                current settings
      PUT  /api/settings                update settings
      POST /api/settings                same as PUT
      POST /api/provider/test           fetch a couple of live candles
*/

#include <drogon/HttpController.h>

#include <string>

namespace golddiggerui
{
  class GolddiggerController : public drogon::HttpController<GolddiggerController>
  {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(GolddiggerController::status, "/api/status", drogon::Get);
    ADD_METHOD_TO(GolddiggerController::timeframes, "/api/timeframes", drogon::Get);
    ADD_METHOD_TO(GolddiggerController::models, "/api/models", drogon::Get);
    ADD_METHOD_TO(GolddiggerController::candles, "/api/candles", drogon::Get);
    ADD_METHOD_TO(GolddiggerController::refreshCandles, "/api/candles/refresh", drogon::Post);
    ADD_METHOD_TO(GolddiggerController::signal, "/api/signal", drogon::Get);
    ADD_METHOD_TO(GolddiggerController::evaluateSignal, "/api/signal/evaluate", drogon::Post);
    ADD_METHOD_TO(GolddiggerController::listOrders, "/api/orders", drogon::Get);
    ADD_METHOD_TO(GolddiggerController::placeOrder, "/api/orders", drogon::Post);
    ADD_METHOD_TO(GolddiggerController::getSettings, "/api/settings", drogon::Get);
    ADD_METHOD_TO(GolddiggerController::updateSettings, "/api/settings", drogon::Put);
    ADD_METHOD_TO(GolddiggerController::updateSettings, "/api/settings", drogon::Post);
    ADD_METHOD_TO(GolddiggerController::testProvider, "/api/provider/test", drogon::Post);
    METHOD_LIST_END

    void status(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void timeframes(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void models(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void candles(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void refreshCandles(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void signal(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void evaluateSignal(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void listOrders(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void placeOrder(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void getSettings(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void updateSettings(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void testProvider(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback);
  };
} // namespace golddiggerui
