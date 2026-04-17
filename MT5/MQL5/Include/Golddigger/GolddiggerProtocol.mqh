#property strict

#include <Trade/Trade.mqh>
#include <Golddigger/GolddiggerJson.mqh>

#define GD_PROTOCOL_NAME "golddigger.mt5.zmq"
#define GD_PROTOCOL_VERSION 1

struct GdRequestEnvelope
{
   string request_id;
   string client_id;
   string command;
   string payload;
};

long GDP_CurrentUtcTimestampMs()
{
   return (long)TimeGMT() * 1000;
}

string GDP_TimeframeToString(const ENUM_TIMEFRAMES timeframe)
{
   switch(timeframe)
   {
      case PERIOD_M15: return "m15";
      case PERIOD_H1:  return "h1";
      case PERIOD_D1:  return "d1";
      default:         return "";
   }
}

bool GDP_ParseTimeframe(const string value, ENUM_TIMEFRAMES &timeframe)
{
   const string normalized = GDJ_ToLower(value);
   if(normalized == "m15")
      timeframe = PERIOD_M15;
   else if(normalized == "h1")
      timeframe = PERIOD_H1;
   else if(normalized == "d1")
      timeframe = PERIOD_D1;
   else
      return false;

   return true;
}

string GDP_MakeEnvelope(const string requestId,
                        const string command,
                        const string status,
                        const string payloadJson)
{
   return "{"
          "\"protocol\":" + GDJ_Quote(GD_PROTOCOL_NAME) + ","
          "\"version\":" + IntegerToString(GD_PROTOCOL_VERSION) + ","
          "\"type\":\"response\","
          "\"request_id\":" + GDJ_Quote(requestId) + ","
          "\"command\":" + GDJ_Quote(command) + ","
          "\"status\":" + GDJ_Quote(status) + ","
          "\"responded_at_utc_ms\":" + LongToString(GDP_CurrentUtcTimestampMs()) + ","
          "\"payload\":" + payloadJson +
          "}";
}

string GDP_MakeErrorEnvelope(const string requestId,
                             const string command,
                             const string code,
                             const string message)
{
   return "{"
          "\"protocol\":" + GDJ_Quote(GD_PROTOCOL_NAME) + ","
          "\"version\":" + IntegerToString(GD_PROTOCOL_VERSION) + ","
          "\"type\":\"response\","
          "\"request_id\":" + GDJ_Quote(requestId) + ","
          "\"command\":" + GDJ_Quote(command) + ","
          "\"status\":\"ERROR\","
          "\"responded_at_utc_ms\":" + LongToString(GDP_CurrentUtcTimestampMs()) + ","
          "\"error\":{"
             "\"code\":" + GDJ_Quote(code) + ","
             "\"message\":" + GDJ_Quote(message) +
          "}"
          "}";
}

bool GDP_ParseRequestEnvelope(const string json,
                              GdRequestEnvelope &request,
                              string &errorCode,
                              string &errorMessage)
{
   string protocolName;
   long version = 0;
   string messageType;

   if(!GDJ_TryGetString(json, "protocol", protocolName) || protocolName != GD_PROTOCOL_NAME)
   {
      errorCode = "BAD_PROTOCOL";
      errorMessage = "Unsupported protocol name.";
      return false;
   }

   if(!GDJ_TryGetLong(json, "version", version) || version != GD_PROTOCOL_VERSION)
   {
      errorCode = "BAD_VERSION";
      errorMessage = "Unsupported protocol version.";
      return false;
   }

   if(!GDJ_TryGetString(json, "type", messageType) || messageType != "request")
   {
      errorCode = "BAD_TYPE";
      errorMessage = "Expected a request envelope.";
      return false;
   }

   if(!GDJ_TryGetString(json, "request_id", request.request_id) || request.request_id == "")
   {
      errorCode = "BAD_REQUEST_ID";
      errorMessage = "Missing request_id.";
      return false;
   }

   if(!GDJ_TryGetString(json, "client_id", request.client_id))
      request.client_id = "";

   if(!GDJ_TryGetString(json, "command", request.command) || request.command == "")
   {
      errorCode = "BAD_COMMAND";
      errorMessage = "Missing command.";
      return false;
   }

   if(!GDJ_TryGetObject(json, "payload", request.payload))
      request.payload = "{}";

   return true;
}

string GDP_BuildPingPayload()
{
   return "{"
          "\"heartbeat\":\"PONG\","
          "\"terminal_connected\":" + (TerminalInfoInteger(TERMINAL_CONNECTED) ? "true" : "false") + ","
          "\"terminal_name\":" + GDJ_Quote(TerminalInfoString(TERMINAL_NAME)) + ","
          "\"company\":" + GDJ_Quote(TerminalInfoString(TERMINAL_COMPANY)) + ","
          "\"account_login\":" + LongToString(AccountInfoInteger(ACCOUNT_LOGIN)) + ","
          "\"server_time_utc_ms\":" + LongToString(GDP_CurrentUtcTimestampMs()) +
          "}";
}

string GDP_BuildSpreadPayload(const string instrument,
                              const MqlTick &tick,
                              const double pointSize)
{
   const int digits = (int)SymbolInfoInteger(instrument, SYMBOL_DIGITS);
   const double spread = tick.ask - tick.bid;
   const double spreadPoints = (pointSize > 0.0) ? (spread / pointSize) : spread;

   return "{"
          "\"instrument\":" + GDJ_Quote(instrument) + ","
          "\"bid\":" + DoubleToString(tick.bid, digits) + ","
          "\"ask\":" + DoubleToString(tick.ask, digits) + ","
          "\"spread\":" + DoubleToString(spread, 10) + ","
          "\"spread_points\":" + DoubleToString(spreadPoints, 4) + ","
          "\"timestamp_ms\":" + LongToString((long)tick.time_msc) +
          "}";
}

string GDP_BuildCandlesPayload(const string instrument,
                               const ENUM_TIMEFRAMES timeframe,
                               MqlRates &rates[],
                               const long fromTimestampMs,
                               const long toTimestampMs,
                               const bool includeVolumes)
{
   const int digits = (int)SymbolInfoInteger(instrument, SYMBOL_DIGITS);
   string candlesJson = "";
   const int count = ArraySize(rates);
   for(int i = 0; i < count; ++i)
   {
      const long openTimestampMs = (long)rates[i].time * 1000;
      if(openTimestampMs < fromTimestampMs || openTimestampMs >= toTimestampMs)
         continue;

      if(candlesJson != "")
         candlesJson += ",";

      candlesJson += "{"
                     "\"timestamp_ms\":" + LongToString(openTimestampMs) + ","
                     "\"open\":" + DoubleToString(rates[i].open, digits) + ","
                     "\"high\":" + DoubleToString(rates[i].high, digits) + ","
                     "\"low\":" + DoubleToString(rates[i].low, digits) + ","
                     "\"close\":" + DoubleToString(rates[i].close, digits) + ","
                     "\"volume\":" + DoubleToString(includeVolumes ? (double)rates[i].tick_volume : 0.0, 0) +
                     "}";
   }

   return "{"
          "\"instrument\":" + GDJ_Quote(instrument) + ","
          "\"timeframe\":" + GDJ_Quote(GDP_TimeframeToString(timeframe)) + ","
          "\"candles\":[" + candlesJson + "]"
          "}";
}

string GDP_BuildTradePayload(const bool accepted,
                             const CTrade &trade,
                             const string message)
{
   return "{"
          "\"accepted\":" + (accepted ? "true" : "false") + ","
          "\"order_ticket\":" + LongToString((long)trade.ResultOrder()) + ","
          "\"deal_ticket\":" + LongToString((long)trade.ResultDeal()) + ","
          "\"position_ticket\":" + LongToString((long)trade.ResultOrder()) + ","
          "\"fill_price\":" + DoubleToString(trade.ResultPrice(), _Digits) + ","
          "\"message\":" + GDJ_Quote(message) + ","
          "\"external_code\":" + GDJ_Quote(LongToString((long)trade.ResultRetcode())) +
          "}";
}

string GDP_BuildCloseAllPayload(const int closedCount,
                                const string message)
{
   return "{"
          "\"closed_count\":" + IntegerToString(closedCount) + ","
          "\"message\":" + GDJ_Quote(message) +
          "}";
}
