# GolddiggerUI

`GolddiggerUI` is the web cockpit of the Golddigger XAUUSD predictor. It is a
small Drogon C++ server with a dependency free single page front end:

- pick a timeframe (`M15`, `H1`, `D1`) and the matching `.dat` model is selected
  automatically,
- see the newest five candles of that timeframe as a chart and a table,
- see the current signal (`BUY`, `SELL`, `HOLD`), its target price, the horizon
  and the edge over the estimated spread,
- place a manual paper or live order, or arm the server side automatic trader.

There is **no database**. Candles, models, settings and orders are files on
disk. The UI only reads what the rest of Golddigger already produces and
appends orders to its own CSV log.

## Requirements

- CMake 3.16 or newer and a C++17 compiler
- [Drogon](https://github.com/drogonframework/drogon) 1.9 or newer
- `dlib`, `nlohmann-json`, `libcurl` and ZeroMQ (the same dependencies as the
  parent Golddigger project)
- Node.js + `npx` for the Dukascopy provider
- an Alpha Vantage API key for the Alpha Vantage provider
- MetaTrader 5 plus the Golddigger ZeroMQ EA for live MT5 data and trading

## Build

### With the parent project (recommended)

The parent build reuses `golddigger_prediction` and the other libraries, so
dlib, curl and ZeroMQ are compiled only once:

```bash
cd Golddigger
cmake -S . -B build
cmake --build build -j8
```

The UI binary lands in `build/bin/GolddiggerUI`. It is built when
`GOLDDIGGER_BUILD_FULL=ON` and Drogon is found. Disable it with:

```bash
cmake -S . -B build -DGOLDDIGGER_BUILD_UI=OFF
```

### Standalone

`GolddiggerUI` also builds on its own. The standalone CMake compiles the few
Golddigger sources the UI needs into `golddigger_ui_core`:

```bash
cd Golddigger/GolddiggerUI
cmake -S . -B build
cmake --build build -j8
```

The binary is `GolddiggerUI/build/GolddiggerUI`. When the checkout layout
differs, point CMake at the parent and the thread component:

```bash
cmake -S . -B build \
  -DGOLDDIGGER_ROOT=/path/to/Golddigger \
  -DTHREAD_COMPONENT_DIR=/path/to/Repository/Software/ThreadComponent
```

## Run

```bash
./GolddiggerUI
# or, from a parent build
./build/bin/GolddiggerUI
```

The server listens on **port 5556** by default and serves the front end at
<http://localhost:5556>. Port 5555 is usually the MetaTrader 5 gateway.

Command line options:

| Option | Meaning |
| --- | --- |
| `--port N`, `-p N` | listen port (default `5556`) |
| `--data DIR` | candle CSV directory (default `Golddigger/Data`) |
| `--models DIR` | trained `.dat` model directory (default `Golddigger/Models`) |
| `--web DIR` | static front end directory (default `GolddiggerUI/web`) |
| `--state DIR` | settings and order log directory (default `GolddiggerUI/state`) |
| `--routes` | print the registered routes on start up |

Every directory can also be set with an environment variable:

| Variable | Meaning |
| --- | --- |
| `GOLDDIGGER_UI_PORT` | listen port |
| `GOLDDIGGER_DATA_DIR` | candle CSV directory |
| `GOLDDIGGER_MODELS_DIR` | model directory |
| `GOLDDIGGER_UI_WEB_DIR` | static front end directory |
| `GOLDDIGGER_UI_STATE_DIR` | state directory |
| `GOLDDIGGER_INSTRUMENT` | instrument, default `xauusd` |
| `ALPHAVANTAGE_API_KEY` | seed the Alpha Vantage key |
| `GOLDDIGGER_MT5_ENDPOINT` | seed the MT5 ZeroMQ endpoint |
| `GOLDDIGGER_MT5_CLIENT_ID` | seed the MT5 client id |
| `DUKASCOPY_NODE_COMMAND` | seed the Dukascopy command |

## Timeframe and model selection

The timeframe is the only thing the user has to choose. For each timeframe the
server resolves the model in this order:

1. an explicit override passed to the API (not exposed in the UI),
2. `Models/<instrument>-<timeframe>-bid.dat`, e.g. `xauusd-m15-bid.dat`,
3. `Models/<instrument>-<timeframe>.dat`,
4. `Models/gold_digger_<timeframe>.dat`,
5. `Models/gold_digger_<instrument>_<timeframe>.dat`,
6. the newest non-tuner `.dat` file whose name contains the timeframe tag
   (`m15`, `h1` or `d1`).

Files starting with `tuner_` or `tunner_` are never selected; they contain
hyperparameters, not serialized models.

Candles are read from `Data/<instrument>-<timeframe>-bid.csv`. If that file is
missing, the newest CSV in the data directory that contains both the instrument
and the timeframe tag is used.

A timeframe is marked **stale** when its newest candle is older than
`3 * interval + availabilityDelaySeconds`. The automatic trader never executes
a stale signal.

## Data sources

The source selector has three values:

- **file** – only the rolling CSV files (default),
- **live** – only the configured live provider; a provider error is returned
  as an HTTP 502 instead of silently falling back,
- **auto** – try the live provider, then fall back to the CSV files with a
  warning.

Providers:

- **Dukascopy** – runs the configured `dukascopy-node` command.
- **Alpha Vantage** – `FX_INTRADAY` endpoint; requires an API key. The key can
  be stored in the UI and is never returned by the API (`alphaVantageApiKey`
  is always empty in responses, `alphaVantageApiKeySet` says whether one is
  configured).
- **MT5** – reads candles over the Golddigger ZeroMQ protocol.

`POST /api/candles/refresh` fetches live candles and merges them into the CSV
file. Duplicate timestamps are replaced, not duplicated, so the file stays a
rolling series.

## Execution and safety

The defaults are deliberately safe:

- execution mode `manual`
- paper trading `on`
- automatic trader `off`
- volume `0.10` lots
- estimated spread `0.40`

Automatic execution needs **all** of the following:

1. `executionMode` is `auto`,
2. `autoArmed` is `true`,
3. the newest candle is not stale,
4. the signal is not `HOLD`,
5. no automatic order has been logged yet for that candle.

Live trading is a separate, explicit step: `PUT /api/settings` with
`paperTrading=false` is rejected unless the request also carries
`confirmLive=true`. Live orders additionally need a non-empty MT5 endpoint.
Switching paper trading off in the UI shows a confirmation checkbox first.

Paper orders are written to `state/orders.csv` and go no further. Live orders
are sent with the same record through the MT5 ZeroMQ client; the broker answer
is appended to the same log.

The order log columns are:

```text
timestamp_ms,time_utc,automatic,instrument,timeframe,candle_timestamp_ms,
side,volume_lots,reference_price,stop_loss,take_profit,status,message,ticket
```

## Configuration files

Nothing is stored in a database. The state directory contains:

- `settings.json` – the UI settings, written atomically (temporary file then
  rename). Unknown fields are ignored, invalid values are rejected with a
  readable message.
- `orders.csv` – the append only order log.

Both are created on demand. Deleting them resets the UI to the safe defaults.

## HTTP API

All endpoints return JSON. Errors have the shape
`{"error": "message"}` and an appropriate status code (`400` for bad input,
`502` for a provider failure, `500` otherwise).

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/status` | app version, settings, paths, provider readiness, all timeframes |
| `GET` | `/api/timeframes` | timeframe → model / data summary |
| `GET` | `/api/models` | every trained model and the selection per timeframe |
| `GET` | `/api/candles?timeframe=m15&count=5&source=file` | newest candles |
| `POST` | `/api/candles/refresh` | fetch live candles and optionally persist them |
| `GET` | `/api/signal?timeframe=m15&source=file` | prediction, candle and execution state |
| `POST` | `/api/signal/evaluate` | signal plus optional automatic execution |
| `GET` | `/api/orders?limit=50` | newest orders first |
| `POST` | `/api/orders` | place a manual order |
| `GET` | `/api/settings` | settings, paths and provider readiness |
| `PUT`/`POST` | `/api/settings` | partial settings update |
| `POST` | `/api/provider/test` | fetch two candles from the configured provider |

Examples:

```bash
curl -s http://localhost:5556/api/status | jq .

curl -s 'http://localhost:5556/api/candles?timeframe=m15&count=5&source=file' | jq .

curl -s 'http://localhost:5556/api/signal?timeframe=h1&source=file' | jq .

# Manual paper order (the default mode)
curl -s -X POST http://localhost:5556/api/orders \
  -H 'Content-Type: application/json' \
  -d '{"timeframe":"m15","side":"BUY","volumeLots":0.1,"comment":"manual entry"}' | jq .

# Arm the automatic trader
curl -s -X PUT http://localhost:5556/api/settings \
  -H 'Content-Type: application/json' \
  -d '{"executionMode":"auto","autoArmed":true}' | jq .

# Switch to live trading on the MT5 gateway
curl -s -X PUT http://localhost:5556/api/settings \
  -H 'Content-Type: application/json' \
  -d '{"paperTrading":false,"confirmLive":true,"mt5Endpoint":"tcp://127.0.0.1:5555"}' | jq .
```

## Front end

The front end is plain HTML, CSS and JavaScript in `web/`. It needs no build
step and no CDN. It polls `/api/status`, `/api/signal` and `/api/orders` on the
configured interval (`pollSeconds`, at least 5 seconds), pauses while the tab
is hidden and remembers the selected timeframe in `localStorage` under
`golddigger.timeframe`.

## Tests

The tests start the real server on a free loop back port and use a temporary
`Data/`, `Models/` and `state/` directory. The fixtures are 140 synthetic
candles and a tiny serialized SVR that predicts a constant +1% move, so the
signals are deterministic. The project's real data files are never touched.

```bash
cd GolddiggerUI
cmake -S . -B build -DGOLDDIGGER_UI_BUILD_TESTS=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

The suite covers model resolution and tuner exclusion, candle parsing, settings
validation and persistence, key redaction, signal freshness, manual paper
orders, automatic arming, one-order-per-candle deduplication and the HTTP
routes themselves.

## Troubleshooting

- **Every API call returns 404.** The Drogon headers used at compile time do
  not match the linked library. The build sets `CMAKE_NO_SYSTEM_FROM_IMPORTED`
  to keep `/usr/local/include` from shadowing the imported target; remove any
  stale Drogon headers from that directory or rebuild after fixing the paths.
- **The UI shows `stale`.** The CSV files are older than three candles. Use
  **Fetch live** in the UI (or `POST /api/candles/refresh`) and make sure the
  selected provider is configured.
- **`no trained model found`.** The expected path is shown in the model card.
  Train a model for that timeframe or rename an existing one to match the
  resolution order above.
- **The server does not start on 5555.** MT5 already uses that port. The UI
  defaults to `5556`; change it with `--port` or `GOLDDIGGER_UI_PORT`.
