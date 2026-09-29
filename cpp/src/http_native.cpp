// Desktop HTTP client using IXWebSocket's HttpClient

#if !defined(__EMSCRIPTEN__)

#include "transport.h"

#if defined(ADSGAMES_PLAY_NETWORK)
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>

#include <algorithm>
#include <mutex>
#endif

namespace adsgames::play {

namespace {

#if defined(ADSGAMES_PLAY_NETWORK)

constexpr int CONNECT_TIMEOUT_S = 10;
constexpr int TRANSFER_TIMEOUT_S = 30;

class NativeHttpClient : public HttpClient {
 public:
  NativeHttpClient() {
    // Needed once on Windows, does nothing elsewhere
    static const bool net_ready = ix::initNetSystem();
    (void)net_ready;
  }

  ~NativeHttpClient() override {
    // Stop requests in flight so the client thread can join quickly
    const std::scoped_lock lock(mutex_);
    for (const auto& args : in_flight_) {
      args->cancel = true;
    }
  }

  NativeHttpClient(const NativeHttpClient&) = delete;
  NativeHttpClient& operator=(const NativeHttpClient&) = delete;
  NativeHttpClient(NativeHttpClient&&) = delete;
  NativeHttpClient& operator=(NativeHttpClient&&) = delete;

  void request(const std::string& method,
               const std::string& url,
               const std::string& body,
               const std::string& token,
               Done done) override {
    auto args = client_.createRequest(url, method);
    args->body = body;
    args->connectTimeout = CONNECT_TIMEOUT_S;
    args->transferTimeout = TRANSFER_TIMEOUT_S;
    // Built without zlib
    args->compress = false;
    args->extraHeaders["Accept"] = "application/json";
    if (!body.empty()) {
      args->extraHeaders["Content-Type"] = "application/json";
    }
    if (!token.empty()) {
      args->extraHeaders["Authorization"] = "Bearer " + token;
    }

    {
      const std::scoped_lock lock(mutex_);
      in_flight_.push_back(args);
    }

    // Runs on the HttpClient thread
    client_.performRequest(
        args, [this, args, done = std::move(done)](
                  const ix::HttpResponsePtr& response) mutable {
          HttpResponse res;
          if (response->errorCode == ix::HttpErrorCode::Ok) {
            res.status = response->statusCode;
            res.body = response->body;
          } else {
            res.error = response->errorMsg;
          }

          const std::scoped_lock lock(mutex_);
          std::erase(in_flight_, args);
          done_.emplace_back(std::move(done), std::move(res));
        });
  }

  void poll() override {
    std::vector<std::pair<Done, HttpResponse>> finished;
    {
      const std::scoped_lock lock(mutex_);
      finished.swap(done_);
    }
    for (auto& [done, res] : finished) {
      done(std::move(res));
    }
  }

  std::size_t pending() const override {
    const std::scoped_lock lock(mutex_);
    return in_flight_.size() + done_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<ix::HttpRequestArgsPtr> in_flight_;
  std::vector<std::pair<Done, HttpResponse>> done_;

  // Last, so its thread stops before the queues above go away
  ix::HttpClient client_{true};
};

#endif

}  // namespace

std::unique_ptr<HttpClient> HttpClient::create() {
#if defined(ADSGAMES_PLAY_NETWORK)
  return std::make_unique<NativeHttpClient>();
#else
  return nullptr;
#endif
}

}  // namespace adsgames::play

#endif
