# Golddigger

Golddigger is a C++17 machine-learning project for training and running short-horizon XAUUSD price predictors with `dlib`.

It currently supports:

- offline model training from candle CSV files
- hyperparameter tuning for SVR (`C`, `epsilon-insensitivity`, `gamma`)
- daily patching of rolling Dukascopy candle CSV files
- live prediction from Dukascopy CLI candle data
- live prediction from Alpha Vantage `FX_INTRADAY`
- live prediction from MetaTrader 5 over ZeroMQ
- MetaTrader 5 ZeroMQ command exchange for heartbeat, spread, live data, and trade execution

The project builds five binaries in the project root:

- `trainer`
- `predictor`
- `tuner.bin`
- `patcher.bin`
- `mt5_gateway`

## What It Does

Golddigger builds an SVR model on top of engineered candle features such as:

- recent log returns
- RSI
- ADX
- relative ATR
- relative SMA20 / SMA50
- SMA spread
- cyclical hour-of-day and day-of-week features

The regression target is a signed best-horizon move measured from the current candle close:

- `m15`: best move reached within the next `6` candles
- `h1`: best move reached within the next `4` candles
- `d1`: best move reached within the next `4` candles

Features are still derived from candle closes. Labels use the future horizon's highest `high` and lowest `low`, then keep whichever log-return has the larger absolute magnitude. This makes the model learn entry points that lead into the strongest subsequent expansion rather than only the very next candle close.

The training flow is:

1. tune `C`, `epsilon-insensitivity`, and `gamma` with `tuner.bin`
2. save the tuned parameters to `Models/tuner_<timeframe>.dat`
3. run `trainer` to build a model
4. run `predictor` to fetch live candles and emit predictions

`trainer` automatically loads the matching `tuner_*.dat` file when one exists for the training dataset timeframe.

## Dependencies

Build/runtime dependencies used by the current codebase:

- CMake
- a C++17 compiler
- [dlib](https://dlib.net/)
- `nlohmann-json`
- libcurl
- libzmq / ZeroMQ
- Node.js and `npx` if you want to use the Dukascopy provider
- an Alpha Vantage API key if you want to use the Alpha Vantage provider
- MetaTrader 5 plus an EA speaking the Golddigger ZeroMQ protocol if you want to use the MT5 provider

This project also depends on local code from your HotBits repo:

- `cBaseWorker_V2.h`
- `cHTTPClient.cpp`
- `cHTTPClient.h`

By default, CMake expects those files under:

```text
../Repository/Software/ThreadComponent
```

If your checkout lives somewhere else, pass the path explicitly:

```bash
cmake -S . -B build -DTHREAD_COMPONENT_DIR=/path/to/Repository/Software/ThreadComponent
```

### Homebrew Notes

On macOS, the current project configuration already looks for Homebrew installs of `nlohmann-json`. A typical setup is:

```bash
brew install nlohmann-json dlib curl
brew install zeromq
```

## Build

Configure and build:

```bash
cmake -S . -B build
cmake --build build
```

Executables are written to the project root:

```text
./trainer
./predictor
./tuner.bin
./patcher.bin
./mt5_gateway
```

## Data Files

The repo currently includes a sample training file:

```text
Data/xauusd-m15-bid-2024-01-01-2026-03-11.csv
```

The rolling live-data files patched by the updater use the current naming convention:

```text
Data/xauusd-m15-bid.csv
Data/xauusd-h1-bid.csv
Data/xauusd-d1-bid.csv
```

CSV format:

```text
timestamp,open,high,low,close,volume
```

## Models and Tuned Parameter Files

Typical output files:

- tuned hyperparameters: `Models/tuner_m15.dat`, `Models/tuner_h1.dat`, `Models/tuner_d1.dat`
- trained models: `Models/gold_digger_m15.dat`, `Models/gold_digger_h1.dat`, etc.

If you pass custom output paths, those are used instead.

## Binary Usage

### `tuner.bin`

Tunes SVR hyperparameters using `dlib::find_max_global` and `cross_validate_regression_trainer`.

Default behavior:

- uses `Data/xauusd-m15-bid-2024-01-01-2026-03-11.csv`
- writes `Models/tuner_m15.dat`
- uses `50` max optimizer calls
- tunes on up to `5000` evenly spaced samples using `3` folds by default
- derives the epsilon-insensitivity search range automatically from the label distribution
- derives the gamma search range automatically from normalized sample distances

Usage:

```bash
./tuner.bin
./tuner.bin --max-calls 25 Data/xauusd-h1.csv
./tuner.bin --max-calls 50 --progress-seconds 60 Data/xauusd-m15-bid.csv
./tuner.bin --max-calls 25 --tuning-samples 8000 --folds 4 Data/xauusd-h1.csv
./tuner.bin --epsilon-range auto Data/xauusd-m15-bid.csv
./tuner.bin --epsilon-range 0.00005:0.003 Data/xauusd-m15-bid.csv
./tuner.bin --gamma-range auto Data/xauusd-m15-bid.csv
./tuner.bin --gamma-range 0.0001:3 Data/xauusd-m15-bid.csv
./tuner.bin Data/xauusd-h1.csv=Models/tuner_h1.dat
./tuner.bin Data/xauusd-m15.csv=Models/tuner_m15.dat Data/xauusd-h1.csv=Models/tuner_h1.dat
```

Useful flags:

- `--max-calls N`
- `--progress-seconds N`
- `--tuning-samples N`
- `--folds N`
- `--epsilon-range auto|MIN:MAX`
- `--gamma-range auto|MIN:MAX`

Argument format:

- `data.csv`
  Result path is inferred as `Models/tuner_<timeframe>.dat`
- `data.csv=output.dat`
  Uses the explicit output file

Notes:

- tuning jobs run on worker threads, so progress can be printed while cross-validation is still running
- the tuner now keeps solver tolerance fixed and tunes `epsilon` as the SVR epsilon-insensitivity parameter
- by default, tuning uses an evenly spaced subset of the generated samples so large `m15` datasets do not spend hours inside the first cross-validation call
- by default, the tuner derives a sensible epsilon-insensitivity search range from the dataset's return distribution
- by default, the tuner derives a sensible gamma search range from normalized sample distances
- `--epsilon-range MIN:MAX` lets you override that auto-derived range manually
- `--gamma-range MIN:MAX` lets you override the auto-derived gamma range manually
- `Ctrl+C` requests a graceful stop and waits for the current evaluation boundary

### `trainer`

Trains a model from one or more CSV datasets.

Default behavior:

- uses `Data/xauusd-m15-bid-2024-01-01-2026-03-11.csv`
- writes `Models/gold_digger_m15.dat`

Usage:

```bash
./trainer
./trainer Data/xauusd-h1.csv
./trainer Data/xauusd-h1.csv=Models/gold_digger_h1.dat
./trainer Data/xauusd-m15.csv=Models/gold_digger_m15.dat Data/xauusd-h1.csv=Models/gold_digger_h1.dat
```

Notes:

- training jobs are launched as worker threads
- duplicate model output paths are rejected
- if `Models/tuner_<timeframe>.dat` exists, the trainer loads it automatically
- if no tuning file exists, trainer falls back to built-in defaults
- `Ctrl+C` requests a graceful stop and waits for worker threads to exit

### `predictor`

Loads a trained model, fetches live candles from a provider, keeps track of the next missing candle timestamp, and predicts the signed best move over the configured future horizon after each newly completed candle becomes available.

Supported providers:

- `dukascopy`
- `alphavantage`
- `mt5`

General usage:

```bash
./predictor --model Models/gold_digger_m15.dat --instrument xauusd --timeframe m15
```

Useful flags:

- `--provider dukascopy|alphavantage|mt5`
- `--model PATH`
- `--instrument SYMBOL`
- `--timeframe m15|h1|d1`
- `--poll-seconds N`
- `--availability-delay-seconds N`
- `--max-predictions N`

Provider-specific flags:

- Dukascopy:
  `--dukascopy-command "npx dukascopy-node"`
- Alpha Vantage:
  `--alpha-vantage-api-key KEY`
  `--alpha-vantage-base-url URL`
- MT5 ZeroMQ:
  `--mt5-endpoint tcp://HOST:PORT`
  `--mt5-client-id ID`
  `--mt5-send-timeout-ms N`
  `--mt5-receive-timeout-ms N`

### Predictor With Dukascopy

The Dukascopy provider shells out to:

```bash
npx dukascopy-node
```

Example:

```bash
./predictor \
  --provider dukascopy \
  --model Models/gold_digger_m15.dat \
  --instrument xauusd \
  --timeframe m15 \
  --dukascopy-command "npx dukascopy-node"
```

Optional environment override:

```bash
export DUKASCOPY_NODE_COMMAND="npx dukascopy-node"
```

### Predictor With Alpha Vantage

Example:

```bash
export ALPHAVANTAGE_API_KEY=your_api_key

./predictor \
  --provider alphavantage \
  --model Models/gold_digger_m15.dat \
  --instrument xauusd \
  --timeframe m15
```

You can also pass the key explicitly:

```bash
./predictor \
  --provider alphavantage \
  --alpha-vantage-api-key your_api_key \
  --model Models/gold_digger_m15.dat \
  --instrument xauusd \
  --timeframe m15
```

Current Alpha Vantage limitations:

- implemented against `FX_INTRADAY`
- supports `m15` and `h1`
- `d1` is not supported through the current Alpha Vantage provider
- volume is not provided by the API, so predictor uses `0.0` for volume on that provider

### Predictor With MT5 ZeroMQ

Example:

```bash
./predictor \
  --provider mt5 \
  --mt5-endpoint tcp://192.168.1.50:5555 \
  --mt5-client-id golddigger-mac \
  --model Models/xauusd-m15-bid.dat \
  --instrument xauusd \
  --timeframe m15
```

The MT5 provider uses JSON request/response envelopes over a ZeroMQ `REQ/REP` link.

Request envelope:

```json
{
  "protocol": "golddigger.mt5.zmq",
  "version": 1,
  "type": "request",
  "request_id": "gd-1776355200000-0",
  "client_id": "golddigger-mac",
  "sent_at_utc_ms": 1776355200000,
  "command": "GET_LIVE_DATA",
  "payload": {
    "instrument": "xauusd",
    "timeframe": "m15",
    "from_timestamp_ms": 1776351600000,
    "to_timestamp_ms": 1776355200000,
    "include_volumes": true
  }
}
```

Response envelope:

```json
{
  "protocol": "golddigger.mt5.zmq",
  "version": 1,
  "type": "response",
  "request_id": "gd-1776355200000-0",
  "command": "GET_LIVE_DATA",
  "status": "OK",
  "payload": {
    "instrument": "XAUUSD",
    "timeframe": "m15",
    "candles": [
      {
        "timestamp_ms": 1776354300000,
        "open": 4821.10,
        "high": 4823.00,
        "low": 4819.80,
        "close": 4820.15,
        "volume": 1234.0
      }
    ]
  }
}
```

Supported MT5 commands on the Golddigger side:

- `PING`
- `GET_SPREAD`
- `GET_LIVE_DATA`
- `BUY`
- `SELL`
- `CLOSE_ALL`

Trade execution payloads use:

- `instrument`
- `volume_lots`
- optional `stop_loss`
- optional `take_profit`
- optional `price`
- `deviation_points`
- `magic`
- `comment`

### `mt5_gateway`

`mt5_gateway` is a small CLI for exercising the MT5 ZeroMQ contract before wiring in automation.

Examples:

```bash
./mt5_gateway --endpoint tcp://192.168.1.50:5555 PING
./mt5_gateway --endpoint tcp://192.168.1.50:5555 GET_SPREAD --instrument xauusd
./mt5_gateway --endpoint tcp://192.168.1.50:5555 GET_LIVE_DATA --instrument xauusd --timeframe m15 --from-local "2026-04-17 08:00" --to-local "2026-04-17 10:00"
./mt5_gateway --endpoint tcp://192.168.1.50:5555 BUY --instrument xauusd --volume 0.10 --sl 4800 --tp 4850 --magic 20260417 --comment "golddigger-test"
./mt5_gateway --endpoint tcp://192.168.1.50:5555 CLOSE_ALL --instrument xauusd --magic 20260417
```

## Live Prediction Timing

The predictor intentionally avoids requesting the still-forming candle.

It uses:

- candle timeframe
- an availability delay
- polling

So if a provider usually lags one or more minutes after candle close, use:

```bash
./predictor --availability-delay-seconds 120
```

Default availability delay is `60` seconds.

## Daily Data Patching

### `patcher.bin`

Fetches one or more completed UTC trading days of Dukascopy candles and merges them into the rolling CSV files in `Data/`.

Default behavior:

- uses Dukascopy CLI via `npx dukascopy-node`
- patches `m15`, `h1`, and `d1`
- targets the previous UTC calendar day
- writes into `Data/xauusd-m15-bid.csv`, `Data/xauusd-h1-bid.csv`, and `Data/xauusd-d1-bid.csv`

Usage:

```bash
./patcher.bin
./patcher.bin --date 2026-03-31
./patcher.bin --from 2026-04-01 --to 2026-04-06
./patcher.bin --from "2026-03-31 00:00" --to "2026-04-07 23:45" --timeframes m15
./patcher.bin --timeframes m15,h1
./patcher.bin --dukascopy-command "npx dukascopy-node" --data-dir Data
```

Useful flags:

- `--date YYYY-MM-DD`
- `--from YYYY-MM-DD[ HH:MM[:SS]]`
- `--to YYYY-MM-DD[ HH:MM[:SS]]`
- `--instrument SYMBOL`
- `--data-dir DIR`
- `--timeframes m15,h1,d1`
- `--dukascopy-command CMD`

Notes:

- requests are formatted in UTC/GMT for Dukascopy
- duplicate timestamps are avoided when patching
- if a timestamp already exists in a CSV, the downloaded candle replaces that row
- `--from` and `--to` patch an inclusive UTC date range with one Dukascopy request per timeframe across the whole window, which is useful for catching up missed days without tripping over weekends or holidays
- `--from` and `--to` also accept exact UTC datetimes; when a time is included, `--to` is treated as the last candle timestamp you want included
- date flags accept both `YYYY-MM-DD` / `DD-MM-YYYY` and `YYYY-MM-DD HH:MM[:SS]` / `DD-MM-YYYY HH:MM[:SS]`
- this is designed as a one-shot updater, so you can schedule it externally for `01:00 GMT`

## Typical Workflow

### 1. Tune hyperparameters

```bash
./tuner.bin --max-calls 50 Data/xauusd-m15-bid-2024-01-01-2026-03-11.csv
```

### 2. Train a model

```bash
./trainer Data/xauusd-m15-bid-2024-01-01-2026-03-11.csv=Models/gold_digger_m15.dat
```

### 3. Run live prediction

```bash
./predictor \
  --provider dukascopy \
  --model Models/gold_digger_m15.dat \
  --instrument xauusd \
  --timeframe m15
```

### 4. Patch the rolling candle files

```bash
./patcher.bin
```

## Project Layout

```text
.
├── CMakeLists.txt
├── Data/
├── DataUpdate/
├── Indicators/
├── MarketData/
├── MT5/
├── Models/
├── Prediction/
├── Training/
├── Utils/
├── mt5_gateway.cpp
├── patcher.cpp
├── predictor.cpp
├── trainer.cpp
└── tuner.cpp
```

## Notes

- The predictor prints an action signal based on predicted price change vs estimated spread.
- The project uses the current working directory for relative paths like `Data/...` and `Models/...`.

## License

This project is licensed under the GNU General Public License v3.0.

See [LICENSE](/Users/phelelanicwele/HotBits/Golddigger/LICENSE) for the full text.

Copyright (C) 2026 Hotbits

Third-party dependencies such as `dlib` and `nlohmann-json` remain under their own respective licenses.
