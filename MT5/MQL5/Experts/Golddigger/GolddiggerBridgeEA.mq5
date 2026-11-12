#property strict
#property description "Golddigger MT5 ZeroMQ bridge"
#property version   "1.00"

#include <Trade/Trade.mqh>
#include <Golddigger/GolddiggerProtocol.mqh>
#include <Golddigger/GolddiggerZeroMqNative.mqh>

input string InpBindAddress = "tcp://*:5555";
input int    InpPollIntervalMs = 100;
input int    InpRequestBufferBytes = 262144;
input int    InpDefaultDeviationPoints = 20;
input long   InpDefaultMagic = 20260417;
input bool   InpVerboseLogging = true;

CGolddiggerZeroMqServer g_server;
CTrade                  g_trade;

string GDB_ResolveInstrument(const string requestedInstrument)
{
   if(requestedInstrument == "")
      return _Symbol;
   return requestedInstrument;
}

bool GDB_TryGetSpreadPayload(const GdRequestEnvelope &request, string &payload, string &errorMessage)
{
   string instrument = "";
   GDJ_TryGetString(request.payload, "instrument", instrument);
   instrument = GDB_ResolveInstrument(instrument);

   if(!SymbolSelect(instrument, true))
   {
      errorMessage = "Failed to select symbol " + instrument + ".";
      return false;
   }

   MqlTick tick;
   if(!SymbolInfoTick(instrument, tick))
   {
      errorMessage = "Failed to read live tick for " + instrument + ".";
      return false;
   }

   const double pointSize = SymbolInfoDouble(instrument, SYMBOL_POINT);
   payload = GDP_BuildSpreadPayload(instrument, tick, pointSize);
   return true;
}

bool GDB_TryGetLiveDataPayload(const GdRequestEnvelope &request, string &payload, string &errorMessage)
{
   string instrument = "";
   string timeframeName = "";
   long fromTimestampMs = 0;
   long toTimestampMs = 0;
   bool includeVolumes = true;

   if(!GDJ_TryGetString(request.payload, "instrument", instrument))
      instrument = _Symbol;
   if(!GDJ_TryGetString(request.payload, "timeframe", timeframeName))
   {
      errorMessage = "GET_LIVE_DATA requires payload.timeframe.";
      return false;
   }
   if(!GDJ_TryGetLong(request.payload, "from_timestamp_ms", fromTimestampMs))
   {
      errorMessage = "GET_LIVE_DATA requires payload.from_timestamp_ms.";
      return false;
   }
   if(!GDJ_TryGetLong(request.payload, "to_timestamp_ms", toTimestampMs))
   {
      errorMessage = "GET_LIVE_DATA requires payload.to_timestamp_ms.";
      return false;
   }
   GDJ_TryGetBool(request.payload, "include_volumes", includeVolumes);

   instrument = GDB_ResolveInstrument(instrument);

   ENUM_TIMEFRAMES timeframe;
   if(!GDP_ParseTimeframe(timeframeName, timeframe))
   {
      errorMessage = "Unsupported timeframe " + timeframeName + ".";
      return false;
   }

   MqlRates rates[];
   ArraySetAsSeries(rates, false);
   const int copied = CopyRates(instrument,
                                timeframe,
                                (datetime)(fromTimestampMs / 1000),
                                (datetime)(toTimestampMs / 1000),
                                rates);
   if(copied < 0)
   {
      errorMessage = "CopyRates failed for " + instrument + ".";
      return false;
   }

   payload = GDP_BuildCandlesPayload(instrument, timeframe, rates, fromTimestampMs, toTimestampMs, includeVolumes);
   return true;
}

bool GDB_ExtractTradeInputs(const GdRequestEnvelope &request,
                            string &instrument,
                            double &volume,
                            double &price,
                            double &stopLoss,
                            double &takeProfit,
                            int &deviationPoints,
                            ulong &magic,
                            string &comment,
                            string &errorMessage)
{
   GDJ_TryGetString(request.payload, "instrument", instrument);
   instrument = GDB_ResolveInstrument(instrument);

   if(!GDJ_TryGetDouble(request.payload, "volume_lots", volume) || volume <= 0.0)
   {
      errorMessage = "Trade requests require payload.volume_lots > 0.";
      return false;
   }

   if(!GDJ_TryGetDouble(request.payload, "price", price))
      price = 0.0;
   if(!GDJ_TryGetDouble(request.payload, "stop_loss", stopLoss))
      stopLoss = 0.0;
   if(!GDJ_TryGetDouble(request.payload, "take_profit", takeProfit))
      takeProfit = 0.0;

   long deviationValue = InpDefaultDeviationPoints;
   if(GDJ_TryGetLong(request.payload, "deviation_points", deviationValue))
      deviationPoints = (int)deviationValue;
   else
      deviationPoints = InpDefaultDeviationPoints;

   long magicValue = InpDefaultMagic;
   if(GDJ_TryGetLong(request.payload, "magic", magicValue))
      magic = (ulong)magicValue;
   else
      magic = (ulong)InpDefaultMagic;

   if(!GDJ_TryGetString(request.payload, "comment", comment))
      comment = "golddigger";

   return true;
}

bool GDB_TryExecuteTrade(const GdRequestEnvelope &request, const bool isBuy, string &payload, string &errorMessage)
{
   string instrument = "";
   double volume = 0.0;
   double price = 0.0;
   double stopLoss = 0.0;
   double takeProfit = 0.0;
   int deviationPoints = InpDefaultDeviationPoints;
   ulong magic = (ulong)InpDefaultMagic;
   string comment = "golddigger";

   if(!GDB_ExtractTradeInputs(request,
                              instrument,
                              volume,
                              price,
                              stopLoss,
                              takeProfit,
                              deviationPoints,
                              magic,
                              comment,
                              errorMessage))
      return false;

   g_trade.SetExpertMagicNumber((long)magic);
   g_trade.SetDeviationInPoints(deviationPoints);

   bool accepted = false;
   if(isBuy)
      accepted = g_trade.Buy(volume, instrument, price, stopLoss, takeProfit, comment);
   else
      accepted = g_trade.Sell(volume, instrument, price, stopLoss, takeProfit, comment);

   payload = GDP_BuildTradePayload(accepted, g_trade, g_trade.ResultRetcodeDescription());
   return true;
}

bool GDB_TryCloseAll(const GdRequestEnvelope &request, string &payload, string &errorMessage)
{
   string instrumentFilter = "";
   long magicValue = 0;
   const bool hasInstrumentFilter = GDJ_TryGetString(request.payload, "instrument", instrumentFilter) && instrumentFilter != "";
   const bool hasMagicFilter = GDJ_TryGetLong(request.payload, "magic", magicValue);

   ulong tickets[];
   ArrayResize(tickets, 0);

   const int total = PositionsTotal();
   for(int i = 0; i < total; ++i)
   {
      const ulong ticket = PositionGetTicket(i);
      if(ticket == 0 || !PositionSelectByTicket(ticket))
         continue;

      const string symbol = PositionGetString(POSITION_SYMBOL);
      const long positionMagic = PositionGetInteger(POSITION_MAGIC);

      if(hasInstrumentFilter && symbol != instrumentFilter)
         continue;
      if(hasMagicFilter && positionMagic != magicValue)
         continue;

      const int newIndex = ArraySize(tickets);
      ArrayResize(tickets, newIndex + 1);
      tickets[newIndex] = ticket;
   }

   g_trade.SetDeviationInPoints(InpDefaultDeviationPoints);

   int closedCount = 0;
   for(int i = 0; i < ArraySize(tickets); ++i)
   {
      if(g_trade.PositionClose(tickets[i]))
         ++closedCount;
   }

   payload = GDP_BuildCloseAllPayload(closedCount, "Processed CLOSE_ALL request.");
   return true;
}

string GDB_HandleRequest(const string requestJson)
{
   GdRequestEnvelope request;
   string errorCode = "";
   string errorMessage = "";

   if(!GDP_ParseRequestEnvelope(requestJson, request, errorCode, errorMessage))
      return GDP_MakeErrorEnvelope(request.request_id, request.command, errorCode, errorMessage);

   string payload = "";
   string command = request.command;
   StringToUpper(command);

   if(command == "PING")
      payload = GDP_BuildPingPayload();
   else if(command == "GET_SPREAD")
   {
      if(!GDB_TryGetSpreadPayload(request, payload, errorMessage))
         return GDP_MakeErrorEnvelope(request.request_id, request.command, "GET_SPREAD_FAILED", errorMessage);
   }
   else if(command == "GET_LIVE_DATA")
   {
      if(!GDB_TryGetLiveDataPayload(request, payload, errorMessage))
         return GDP_MakeErrorEnvelope(request.request_id, request.command, "GET_LIVE_DATA_FAILED", errorMessage);
   }
   else if(command == "BUY")
   {
      if(!GDB_TryExecuteTrade(request, true, payload, errorMessage))
         return GDP_MakeErrorEnvelope(request.request_id, request.command, "BUY_FAILED", errorMessage);
   }
   else if(command == "SELL")
   {
      if(!GDB_TryExecuteTrade(request, false, payload, errorMessage))
         return GDP_MakeErrorEnvelope(request.request_id, request.command, "SELL_FAILED", errorMessage);
   }
   else if(command == "CLOSE_ALL")
   {
      if(!GDB_TryCloseAll(request, payload, errorMessage))
         return GDP_MakeErrorEnvelope(request.request_id, request.command, "CLOSE_ALL_FAILED", errorMessage);
   }
   else
      return GDP_MakeErrorEnvelope(request.request_id, request.command, "UNSUPPORTED_COMMAND", "Unsupported command.");

   return GDP_MakeEnvelope(request.request_id, request.command, "OK", payload);
}

int OnInit()
{
   g_trade.SetAsyncMode(false);

   if(!g_server.Start(InpBindAddress, InpRequestBufferBytes))
      return INIT_FAILED;

   EventSetMillisecondTimer(InpPollIntervalMs);

   Print("GolddiggerBridgeEA started on ", InpBindAddress);
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason)
{
   EventKillTimer();
   g_server.Stop();
   Print("GolddiggerBridgeEA stopped.");
}

void OnTimer()
{
   for(int i = 0; i < 16; ++i)
   {
      string requestJson = "";
      if(!g_server.TryReceive(requestJson))
         break;

      if(InpVerboseLogging)
         Print("Golddigger request: ", requestJson);

      const string responseJson = GDB_HandleRequest(requestJson);
      if(InpVerboseLogging)
         Print("Golddigger response: ", responseJson);

      g_server.Send(responseJson);
   }
}
