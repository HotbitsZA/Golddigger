#include "MT5/mt5_zeromq_client.h"

#include <zmq.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    std::runtime_error zmq_error(const std::string &context)
    {
        std::ostringstream error;
        error << context << ": " << zmq_strerror(errno);
        return std::runtime_error(error.str());
    }
}

namespace mt5
{
    ZeroMqClient::ZeroMqClient(ClientConfig config)
        : m_config(std::move(config)),
          m_context(zmq_ctx_new())
    {
        if (m_context == nullptr)
            throw zmq_error("Failed to create ZeroMQ context");
    }

    ZeroMqClient::~ZeroMqClient()
    {
        if (m_context != nullptr)
        {
            zmq_ctx_term(m_context);
            m_context = nullptr;
        }
    }

    std::string ZeroMqClient::describeEndpoint() const
    {
        return m_config.endpoint;
    }

    nlohmann::json ZeroMqClient::ping() const
    {
        return sendRequest(Command::Ping, build_ping_payload()).at("payload");
    }

    SpreadQuote ZeroMqClient::getSpread(const std::string &instrument) const
    {
        return parse_spread_quote(sendRequest(Command::GetSpread, build_spread_payload(instrument)).at("payload"));
    }

    std::vector<Candle> ZeroMqClient::getLiveData(const CandleFetchRequest &request) const
    {
        const auto response = sendRequest(Command::GetLiveData, build_live_data_payload(request));
        if (!response.contains("payload"))
            throw std::runtime_error("MT5 GET_LIVE_DATA response is missing payload.");
        return parse_candle_array(response.at("payload").at("candles"));
    }

    TradeResult ZeroMqClient::buy(const TradeRequest &request) const
    {
        return parse_trade_result(sendRequest(Command::Buy, build_trade_payload(request)).at("payload"));
    }

    TradeResult ZeroMqClient::sell(const TradeRequest &request) const
    {
        return parse_trade_result(sendRequest(Command::Sell, build_trade_payload(request)).at("payload"));
    }

    CloseAllResult ZeroMqClient::closeAll(const CloseAllRequest &request) const
    {
        return parse_close_all_result(sendRequest(Command::CloseAll, build_close_all_payload(request)).at("payload"));
    }

    nlohmann::json ZeroMqClient::sendRequest(Command command, const nlohmann::json &payload) const
    {
        if (m_context == nullptr)
            throw std::runtime_error("MT5 ZeroMQ client context is not available.");

        void *socket = zmq_socket(m_context, ZMQ_REQ);
        if (socket == nullptr)
            throw zmq_error("Failed to create ZeroMQ REQ socket");

        const auto cleanupSocket = [&]()
        {
            if (socket != nullptr)
            {
                zmq_close(socket);
                socket = nullptr;
            }
        };

        const int linger = 0;
        if (zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger)) != 0)
        {
            cleanupSocket();
            throw zmq_error("Failed to set ZeroMQ linger");
        }

        if (zmq_setsockopt(socket, ZMQ_SNDTIMEO, &m_config.sendTimeoutMs, sizeof(m_config.sendTimeoutMs)) != 0)
        {
            cleanupSocket();
            throw zmq_error("Failed to set ZeroMQ send timeout");
        }

        if (zmq_setsockopt(socket, ZMQ_RCVTIMEO, &m_config.receiveTimeoutMs, sizeof(m_config.receiveTimeoutMs)) != 0)
        {
            cleanupSocket();
            throw zmq_error("Failed to set ZeroMQ receive timeout");
        }

        if (zmq_connect(socket, m_config.endpoint.c_str()) != 0)
        {
            cleanupSocket();
            throw zmq_error("Failed to connect to MT5 ZeroMQ endpoint " + m_config.endpoint);
        }

        const auto requestId = generate_request_id();
        const auto envelope = build_request_envelope(command, payload, m_config.clientId, requestId);
        const auto serialized = envelope.dump();

        if (zmq_send(socket, serialized.data(), serialized.size(), 0) < 0)
        {
            cleanupSocket();
            throw zmq_error("Failed to send MT5 ZeroMQ request");
        }

        zmq_msg_t replyMessage;
        zmq_msg_init(&replyMessage);
        const int receivedBytes = zmq_msg_recv(&replyMessage, socket, 0);
        if (receivedBytes < 0)
        {
            zmq_msg_close(&replyMessage);
            cleanupSocket();
            throw zmq_error("Failed to receive MT5 ZeroMQ response");
        }

        std::string reply(
            static_cast<const char *>(zmq_msg_data(&replyMessage)),
            static_cast<std::size_t>(zmq_msg_size(&replyMessage)));
        zmq_msg_close(&replyMessage);
        cleanupSocket();

        const auto response = nlohmann::json::parse(reply);
        validate_response_envelope(response, command, requestId);
        return response;
    }
}
