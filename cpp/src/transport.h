/**
 * Platform transports
 *
 * Desktop builds use IXWebSocket, which runs its own network threads. Web
 * builds use the browser through emscripten. Both queue what they receive so
 * the game reads it on the main thread once per frame.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace adsgames::play {

class WebSocketClient {
 public:
  struct Event {
    enum class Type : uint8_t { Open, Message, Close, Error };

    Type type;

    // Message text, or the reason for a close or error
    std::string data;
  };

  WebSocketClient() = default;
  virtual ~WebSocketClient() = default;

  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;
  WebSocketClient(WebSocketClient&&) = delete;
  WebSocketClient& operator=(WebSocketClient&&) = delete;

  virtual void connect(const std::string& url) = 0;

  virtual void send(const std::string& text) = 0;

  virtual void close() = 0;

  // Events received since the last call, oldest first
  virtual std::vector<Event> poll() = 0;

  // Client for the current platform, nullptr without network support
  static std::unique_ptr<WebSocketClient> create();
};

struct HttpResponse {
  // 0 when the server could not be reached
  int status{0};
  std::string body;
  std::string error;
};

class HttpClient {
 public:
  using Done = std::function<void(HttpResponse)>;

  HttpClient() = default;
  virtual ~HttpClient() = default;

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;
  HttpClient(HttpClient&&) = delete;
  HttpClient& operator=(HttpClient&&) = delete;

  // Sends a JSON request. An empty body sends none, an empty token no
  // Authorization header.
  virtual void request(const std::string& method,
                       const std::string& url,
                       const std::string& body,
                       const std::string& token,
                       Done done) = 0;

  // Calls done for requests that finished since the last call
  virtual void poll() = 0;

  virtual std::size_t pending() const = 0;

  // Client for the current platform, nullptr without network support
  static std::unique_ptr<HttpClient> create();
};

// Turns the REST base URL into the multiplayer WebSocket URL
std::string multiplayer_url_for(const std::string& base_url);

}  // namespace adsgames::play
