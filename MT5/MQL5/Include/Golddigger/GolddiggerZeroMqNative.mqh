#property strict

#define GD_ZMQ_REP 4
#define GD_ZMQ_DONTWAIT 1
#define GD_ZMQ_EAGAIN 11

#import "libzmq.dll"
long zmq_ctx_new();
int  zmq_ctx_term(long context);
long zmq_socket(long context, int type);
int  zmq_bind(long socket, uchar &endpoint[]);
int  zmq_close(long socket);
int  zmq_send(long socket, uchar &buffer[], int length, int flags);
int  zmq_recv(long socket, uchar &buffer[], int length, int flags);
int  zmq_errno();
#import

class CGolddiggerZeroMqServer
{
private:
   long   m_context;
   long   m_socket;
   int    m_bufferBytes;
   string m_endpoint;
   string m_lastError;

   void SetError(const string message)
   {
      m_lastError = message;
      Print("GolddiggerZeroMqServer: ", message);
   }

   bool StringToUtf8Z(const string value, uchar &bytes[])
   {
      ArrayFree(bytes);
      const int written = StringToCharArray(value, bytes, 0, WHOLE_ARRAY, CP_UTF8);
      if(written <= 0)
         return false;

      return true;
   }

public:
   CGolddiggerZeroMqServer(void)
   {
      m_context = 0;
      m_socket = 0;
      m_bufferBytes = 262144;
      m_endpoint = "";
      m_lastError = "";
   }

   bool Start(const string endpoint, const int bufferBytes)
   {
      Stop();

      m_bufferBytes = MathMax(bufferBytes, 4096);
      m_endpoint = endpoint;

      m_context = zmq_ctx_new();
      if(m_context == 0)
      {
         SetError("zmq_ctx_new failed.");
         return false;
      }

      m_socket = zmq_socket(m_context, GD_ZMQ_REP);
      if(m_socket == 0)
      {
         SetError("zmq_socket(ZMQ_REP) failed.");
         Stop();
         return false;
      }

      uchar endpointBytes[];
      if(!StringToUtf8Z(endpoint, endpointBytes))
      {
         SetError("Failed to encode ZeroMQ endpoint.");
         Stop();
         return false;
      }

      if(zmq_bind(m_socket, endpointBytes) != 0)
      {
         SetError("zmq_bind failed for endpoint " + endpoint + ".");
         Stop();
         return false;
      }

      m_lastError = "";
      return true;
   }

   void Stop()
   {
      if(m_socket != 0)
      {
         zmq_close(m_socket);
         m_socket = 0;
      }

      if(m_context != 0)
      {
         zmq_ctx_term(m_context);
         m_context = 0;
      }
   }

   bool IsStarted(void) const
   {
      return (m_socket != 0);
   }

   string Endpoint(void) const
   {
      return m_endpoint;
   }

   string LastError(void) const
   {
      return m_lastError;
   }

   bool TryReceive(string &message)
   {
      message = "";
      if(m_socket == 0)
         return false;

      uchar buffer[];
      ArrayResize(buffer, m_bufferBytes);
      ArrayInitialize(buffer, 0);

      const int received = zmq_recv(m_socket, buffer, m_bufferBytes - 1, GD_ZMQ_DONTWAIT);
      if(received < 0)
      {
         const int err = zmq_errno();
         if(err == GD_ZMQ_EAGAIN)
            return false;

         SetError("zmq_recv failed with error " + IntegerToString(err) + ".");
         return false;
      }

      message = CharArrayToString(buffer, 0, received, CP_UTF8);
      return true;
   }

   bool Send(const string message)
   {
      if(m_socket == 0)
         return false;

      uchar bytes[];
      if(!StringToUtf8Z(message, bytes))
      {
         SetError("Failed to encode ZeroMQ reply.");
         return false;
      }

      int length = ArraySize(bytes);
      if(length > 0 && bytes[length - 1] == 0)
         --length;

      const int sent = zmq_send(m_socket, bytes, length, 0);
      if(sent < 0)
      {
         SetError("zmq_send failed with error " + IntegerToString(zmq_errno()) + ".");
         return false;
      }

      return true;
   }
};
