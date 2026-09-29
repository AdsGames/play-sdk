// Desktop WebSocket client using IXWebSocket

#if !defined(__EMSCRIPTEN__)

#include <mutex>

#include "transport.h"

#if defined(ADSGAMES_PLAY_NETWORK)
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#endif

namespace adsgames::play {

namespace {

#if defined(ADSGAMES_PLAY_NETWORK)

    // Keeps idle connections open through proxies and load balancers
    constexpr int PING_INTERVAL_S = 20;

    class NativeWebSocketClient : public WebSocketClient {
    public:
        NativeWebSocketClient()
        {
            // Needed once on Windows, does nothing elsewhere
            static const bool net_ready = ix::initNetSystem();
            (void)net_ready;
        }

        ~NativeWebSocketClient() override
        {
            socket_.stop();
        }

        NativeWebSocketClient(const NativeWebSocketClient&) = delete;
        NativeWebSocketClient& operator=(const NativeWebSocketClient&) = delete;
        NativeWebSocketClient(NativeWebSocketClient&&) = delete;
        NativeWebSocketClient& operator=(NativeWebSocketClient&&) = delete;

        void connect(const std::string& url) override
        {
            socket_.setUrl(url);
            socket_.disableAutomaticReconnection();
            socket_.setPingInterval(PING_INTERVAL_S);

            // Runs on the IXWebSocket thread
            socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
                switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    push({ .type = Event::Type::Open, .data = "" });
                    break;
                case ix::WebSocketMessageType::Message:
                    push({ .type = Event::Type::Message, .data = msg->str });
                    break;
                case ix::WebSocketMessageType::Close:
                    push({ .type = Event::Type::Close, .data = msg->closeInfo.reason });
                    break;
                case ix::WebSocketMessageType::Error:
                    push({ .type = Event::Type::Error, .data = msg->errorInfo.reason });
                    break;
                default:
                    break;
                }
            });

            socket_.start();
        }

        void send(const std::string& text) override
        {
            socket_.sendText(text);
        }

        void close() override
        {
            socket_.stop();
        }

        std::vector<Event> poll() override
        {
            const std::scoped_lock lock(mutex_);
            std::vector<Event> events;
            events.swap(events_);
            return events;
        }

    private:
        void push(Event event)
        {
            const std::scoped_lock lock(mutex_);
            events_.push_back(std::move(event));
        }

        ix::WebSocket socket_;
        std::mutex mutex_;
        std::vector<Event> events_;
    };

#endif

} // namespace

std::unique_ptr<WebSocketClient> WebSocketClient::create()
{
#if defined(ADSGAMES_PLAY_NETWORK)
    return std::make_unique<NativeWebSocketClient>();
#else
    return nullptr;
#endif
}

} // namespace adsgames::play

#endif
