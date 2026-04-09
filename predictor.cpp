#include "MarketData/alpha_vantage_fx_provider.h"
#include "MarketData/candle_data_provider.h"
#include "MarketData/dukascopy_cli_provider.h"
#include "Prediction/predictor_engine.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    std::string default_model_path(CandleTimeframe timeframe)
    {
        return "Models/gold_digger_" + to_string(timeframe) + ".dat";
    }

void print_usage()
{
    std::cout << "Usage: predictor [--model PATH] [--instrument SYMBOL] [--timeframe m15|h1|d1]\n"
              << "                 [--provider dukascopy|alphavantage]\n"
              << "                 [--dukascopy-command CMD] [--dukascopy-debug-dir DIR]\n"
              << "                 [--alpha-vantage-api-key KEY] [--alpha-vantage-base-url URL]\n"
              << "                 [--poll-seconds N] [--availability-delay-seconds N]\n"
              << "                 [--max-predictions N]\n";
}
}

int main(int argc, char *argv[])
{
    PredictorConfig config;
    std::string providerName{"dukascopy"};
    std::string dukascopyCommand{"npx dukascopy-node -s"};
    std::string dukascopyDebugDirectory;
    std::string alphaVantageApiKey;
    std::string alphaVantageBaseUrl{"https://www.alphavantage.co/query"};
    bool modelProvided = false;

    if (const char *envCommand = std::getenv("DUKASCOPY_NODE_COMMAND"))
        dukascopyCommand = envCommand;
    if (const char *envKey = std::getenv("ALPHAVANTAGE_API_KEY"))
        alphaVantageApiKey = envKey;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];

        auto require_value = [&](const char *option) -> const char *
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << option << '\n';
                std::exit(1);
            }

            return argv[++i];
        };

        if (argument == "--help")
        {
            print_usage();
            return 0;
        }
        if (argument == "--model")
        {
            config.modelFile = require_value("--model");
            modelProvided = true;
        }
        else if (argument == "--instrument")
        {
            config.instrument = require_value("--instrument");
        }
        else if (argument == "--timeframe")
        {
            CandleTimeframe timeframe{};
            if (!parse_timeframe(require_value("--timeframe"), timeframe))
            {
                std::cerr << "Unsupported timeframe. Use m15, h1, or d1.\n";
                return 1;
            }
            config.timeframe = timeframe;
        }
        else if (argument == "--provider")
        {
            providerName = require_value("--provider");
        }
        else if (argument == "--dukascopy-command")
        {
            dukascopyCommand = require_value("--dukascopy-command");
        }
        else if (argument == "--dukascopy-debug-dir")
        {
            dukascopyDebugDirectory = require_value("--dukascopy-debug-dir");
        }
        else if (argument == "--alpha-vantage-api-key")
        {
            alphaVantageApiKey = require_value("--alpha-vantage-api-key");
        }
        else if (argument == "--alpha-vantage-base-url")
        {
            alphaVantageBaseUrl = require_value("--alpha-vantage-base-url");
        }
        else if (argument == "--poll-seconds")
        {
            config.pollInterval = std::chrono::seconds(std::stoll(require_value("--poll-seconds")));
        }
        else if (argument == "--availability-delay-seconds")
        {
            config.availabilityDelay = std::chrono::seconds(std::stoll(require_value("--availability-delay-seconds")));
        }
        else if (argument == "--max-predictions")
        {
            config.maxPredictions = std::stoull(require_value("--max-predictions"));
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            print_usage();
            return 1;
        }
    }

    if (!modelProvided)
        config.modelFile = default_model_path(config.timeframe);

    std::unique_ptr<ICandleDataProvider> provider;
    if (providerName == "dukascopy")
    {
        provider = std::make_unique<DukascopyCliDataProvider>(dukascopyCommand, dukascopyDebugDirectory);
    }
    else if (providerName == "alphavantage")
    {
        provider = std::make_unique<AlphaVantageFxProvider>(AlphaVantageFxProvider::Config{
            alphaVantageApiKey,
            alphaVantageBaseUrl,
            "compact"});
    }
    else
    {
        std::cerr << "Unsupported provider: " << providerName << '\n';
        return 1;
    }

    PredictorEngine predictor(std::move(provider), config);
    return predictor.run();
}
