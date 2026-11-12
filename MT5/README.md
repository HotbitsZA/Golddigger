# MT5 Bridge Files

These files are the MetaTrader 5 side of the Golddigger ZeroMQ bridge.

## Repo Layout

Copy these folders to the Windows MT5 terminal data directory:

- [`MT5/MQL5/Experts/Golddigger/`](/Users/phelelanicwele/HotBits/Golddigger/MT5/MQL5/Experts/Golddigger/)
- [`MT5/MQL5/Include/Golddigger/`](/Users/phelelanicwele/HotBits/Golddigger/MT5/MQL5/Include/Golddigger/)

Result on Windows:

- `MQL5/Experts/Golddigger/GolddiggerBridgeEA.mq5`
- `MQL5/Include/Golddigger/GolddiggerJson.mqh`
- `MQL5/Include/Golddigger/GolddiggerProtocol.mqh`
- `MQL5/Include/Golddigger/GolddiggerZeroMqNative.mqh`

## Runtime Dependency

This EA uses direct DLL imports from `libzmq.dll`.

You need:

- `libzmq.dll` available inside the MT5 `MQL5/Libraries/` folder or terminal library path
- `Allow DLL imports` enabled when attaching the EA
- MT5 algorithmic trading enabled

## Socket Role

The EA binds a ZeroMQ `REP` socket by default:

- `tcp://*:5555`

Golddigger should connect to that Windows host with:

- `tcp://WINDOWS_PC_IP:5555`

## Supported Commands

The EA handles:

- `PING`
- `GET_SPREAD`
- `GET_LIVE_DATA`
- `BUY`
- `SELL`
- `CLOSE_ALL`

The JSON envelope matches the Golddigger MT5 protocol already implemented on the C++ side.

## MT5 Workflow

1. Open MT5.
2. `File -> Open Data Folder`
3. Copy the `Experts/Golddigger` and `Include/Golddigger` folders into `MQL5/`.
4. Copy `libzmq.dll` into `MQL5/Libraries/`.
5. Restart MetaEditor or refresh the Navigator.
6. Compile `GolddiggerBridgeEA.mq5`.
7. Attach the EA to any chart for the instrument you want available.
8. Enable:
   - algorithmic trading
   - DLL imports

## Notes

- The EA is intentionally request/reply only for now.
- Live candles are served on demand through `GET_LIVE_DATA`.
- Trade execution includes support for:
  - `volume_lots`
  - optional `stop_loss`
  - optional `take_profit`
  - optional `price`
  - `deviation_points`
  - `magic`
  - `comment`

- `CLOSE_ALL` can be filtered by:
  - `instrument`
  - `magic`

- I could not compile these MQL5 files from this macOS workspace, so they still need to be compiled and smoke-tested in MetaEditor on your Windows machine.
