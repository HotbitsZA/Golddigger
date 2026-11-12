#property strict

string GDJ_Trim(const string value)
{
   string result = value;
   StringTrimLeft(result);
   StringTrimRight(result);
   return result;
}

string GDJ_Escape(const string value)
{
   string result = value;
   StringReplace(result, "\\", "\\\\");
   StringReplace(result, "\"", "\\\"");
   StringReplace(result, "\r", "\\r");
   StringReplace(result, "\n", "\\n");
   StringReplace(result, "\t", "\\t");
   return result;
}

string GDJ_Quote(const string value)
{
   return "\"" + GDJ_Escape(value) + "\"";
}

string GDJ_Unescape(const string value)
{
   string result = value;
   StringReplace(result, "\\\"", "\"");
   StringReplace(result, "\\\\", "\\");
   StringReplace(result, "\\r", "\r");
   StringReplace(result, "\\n", "\n");
   StringReplace(result, "\\t", "\t");
   return result;
}

string GDJ_ToLower(const string value)
{
   string result = value;
   StringToLower(result);
   return result;
}

int GDJ_SkipWhitespace(const string json, int index)
{
   int length = StringLen(json);
   while(index < length)
   {
      const ushort ch = StringGetCharacter(json, index);
      if(ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
         break;
      ++index;
   }
   return index;
}

bool GDJ_ExtractRawValue(const string json, const string key, string &rawValue)
{
   const string needle = "\"" + key + "\"";
   int searchFrom = 0;

   while(true)
   {
      const int keyPos = StringFind(json, needle, searchFrom);
      if(keyPos < 0)
         return false;

      int colonPos = StringFind(json, ":", keyPos + StringLen(needle));
      if(colonPos < 0)
         return false;

      int start = GDJ_SkipWhitespace(json, colonPos + 1);
      const int length = StringLen(json);
      if(start >= length)
         return false;

      const ushort first = StringGetCharacter(json, start);

      if(first == '"')
      {
         int end = start + 1;
         bool escaped = false;
         while(end < length)
         {
            const ushort ch = StringGetCharacter(json, end);
            if(ch == '"' && !escaped)
               break;
            escaped = (ch == '\\' && !escaped);
            if(ch != '\\')
               escaped = false;
            ++end;
         }

         if(end >= length)
            return false;

         rawValue = StringSubstr(json, start, end - start + 1);
         return true;
      }

      if(first == '{' || first == '[')
      {
         const ushort opener = first;
         const ushort closer = (first == '{') ? '}' : ']';
         int depth = 0;
         int end = start;
         bool inString = false;
         bool escaped = false;
         while(end < length)
         {
            const ushort ch = StringGetCharacter(json, end);
            if(inString)
            {
               if(ch == '"' && !escaped)
                  inString = false;
               escaped = (ch == '\\' && !escaped);
               if(ch != '\\')
                  escaped = false;
            }
            else
            {
               if(ch == '"')
                  inString = true;
               else if(ch == opener)
                  ++depth;
               else if(ch == closer)
               {
                  --depth;
                  if(depth == 0)
                  {
                     rawValue = StringSubstr(json, start, end - start + 1);
                     return true;
                  }
               }
            }
            ++end;
         }

         return false;
      }

      int end = start;
      while(end < length)
      {
         const ushort ch = StringGetCharacter(json, end);
         if(ch == ',' || ch == '}' || ch == ']')
            break;
         ++end;
      }

      rawValue = GDJ_Trim(StringSubstr(json, start, end - start));
      return true;
   }

   return false;
}

bool GDJ_TryGetString(const string json, const string key, string &value)
{
   string rawValue;
   if(!GDJ_ExtractRawValue(json, key, rawValue))
      return false;

   rawValue = GDJ_Trim(rawValue);
   if(StringLen(rawValue) < 2)
      return false;
   if(StringGetCharacter(rawValue, 0) != '"' || StringGetCharacter(rawValue, StringLen(rawValue) - 1) != '"')
      return false;

   value = GDJ_Unescape(StringSubstr(rawValue, 1, StringLen(rawValue) - 2));
   return true;
}

bool GDJ_TryGetLong(const string json, const string key, long &value)
{
   string rawValue;
   if(!GDJ_ExtractRawValue(json, key, rawValue))
      return false;

   value = (long)StringToInteger(GDJ_Trim(rawValue));
   return true;
}

bool GDJ_TryGetDouble(const string json, const string key, double &value)
{
   string rawValue;
   if(!GDJ_ExtractRawValue(json, key, rawValue))
      return false;

   value = StringToDouble(GDJ_Trim(rawValue));
   return true;
}

bool GDJ_TryGetBool(const string json, const string key, bool &value)
{
   string rawValue;
   if(!GDJ_ExtractRawValue(json, key, rawValue))
      return false;

   rawValue = GDJ_Trim(rawValue);
   const string lowered = GDJ_ToLower(rawValue);
   value = (lowered == "true" || lowered == "1");
   return true;
}

bool GDJ_TryGetObject(const string json, const string key, string &value)
{
   if(!GDJ_ExtractRawValue(json, key, value))
      return false;
   value = GDJ_Trim(value);
   return (StringLen(value) > 1 && StringGetCharacter(value, 0) == '{');
}
