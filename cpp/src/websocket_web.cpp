// Browser WebSocket client for emscripten builds

#if defined(__EMSCRIPTEN__)

#include "transport.h"

#if defined(ADSGAMES_PLAY_NETWORK)
#include <emscripten/websocket.h>
#endif

namespace adsgames::play {

namespace {

#if defined(ADSGAMES_PLAY_NETWORK)

    class WebWebSocketClient : public WebSocketClient {
    public:
        WebWebSocketClient() = default;

        ~WebWebSocketClient() override
        {
            close();
        }

        WebWebSocketClient(const WebWebSocketClient&) = delete;
        WebWebSocketClient& operator=(const WebWebSocketClient&) = delete;
        WebWebSocketClient(WebWebSocketClient&&) = delete;
        WebWebSocketClient& operator=(WebWebSocketClient&&) = delete;

        void connect(const std::string& url) override
        {
            close();

            if (emscripten_websocket_is_supported() == 0) {
                events_.push_back({ Event::Type::Error, "WebSockets are not supported" });
                return;
            }

            EmscriptenWebSocketCreateAttributes attributes;
            emscripten_websocket_init_create_attributes(&attributes);
            attributes.url = url.c_str();
            attributes.createOnMainThread = true;

            socket_ = emscripten_websocket_new(&attributes);
            if (socket_ <= 0) {
                socket_ = 0;
                events_.push_back({ Event::Type::Error, "Could not open WebSocket" });
                return;
            }

            // Callbacks run on the main thread between frames
            emscripten_websocket_set_onopen_callback(socket_, this, on_open);
            emscripten_websocket_set_onmessage_callback(socket_, this, on_message);
            emscripten_websocket_set_onerror_callback(socket_, this, on_error);
            emscripten_websocket_set_onclose_callback(socket_, this, on_close);
        }

        void send(const std::string& text) override
        {
            if (socket_ > 0) {
                emscripten_websocket_send_utf8_text(socket_, text.c_str());
            }
        }

        void close() override
        {
            if (socket_ > 0) {
                emscripten_websocket_close(socket_, 1000, "closed");

                // Also removes the callbacks, so none fire into a deleted client
                emscripten_websocket_delete(socket_);
                socket_ = 0;
            }
        }

        std::vector<Event> poll() override
        {
            std::vector<Event> events;
            events.swap(events_);
            return events;
        }

    private:
        static bool on_open(int /*type*/, const EmscriptenWebSocketOpenEvent* /*event*/, void* user)
        {
            static_cast<WebWebSocketClient*>(user)->events_.push_back({ Event::Type::Open, "" });
            return true;
        }

        static bool on_message(
            int /*type*/, const EmscriptenWebSocketMessageEvent* event, void* user)
        {
            if (!event->isText || event->numBytes == 0) {
                return true;
            }

            // Text data ends in a null terminator that numBytes counts
            std::string text(reinterpret_cast<const char*>(event->data), event->numBytes - 1);
            static_cast<WebWebSocketClient*>(user)->events_.push_back(
                { Event::Type::Message, std::move(text) });
            return true;
        }

        static bool on_error(
            int /*type*/, const EmscriptenWebSocketErrorEvent* /*event*/, void* user)
        {
            static_cast<WebWebSocketClient*>(user)->events_.push_back(
                { Event::Type::Error, "Connection failed" });
            return true;
        }

        static bool on_close(int /*type*/, const EmscriptenWebSocketCloseEvent* event, void* user)
        {
            static_cast<WebWebSocketClient*>(user)->events_.push_back(
                { Event::Type::Close, event->reason });
            return true;
        }

        EMSCRIPTEN_WEBSOCKET_T socket_ { 0 };
        std::vector<Event> events_;
    };

#endif

} // namespace

std::unique_ptr<WebSocketClient> WebSocketClient::create()
{
#if defined(ADSGAMES_PLAY_NETWORK)
    return std::make_unique<WebWebSocketClient>();
#else
    return nullptr;
#endif
}

} // namespace adsgames::play

#endif
