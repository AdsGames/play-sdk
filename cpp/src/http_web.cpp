// Browser HTTP client for emscripten builds, using emscripten_fetch
//
// Same-origin requests send the adsgames.net session cookie, so players are
// logged in without the game handling tokens.

#if defined(__EMSCRIPTEN__)

#include "transport.h"

#if defined(ADSGAMES_PLAY_NETWORK)
#include <emscripten/fetch.h>

#include <cstring>
#endif

namespace adsgames::play {

namespace {

#if defined(ADSGAMES_PLAY_NETWORK)

constexpr unsigned TIMEOUT_MS = 30000;

struct Finished {
  std::vector<std::pair<HttpClient::Done, HttpResponse>> responses;
  std::size_t in_flight{0};
};

// Lives until the fetch finishes, which can be after the client is gone
struct WebRequest {
  std::weak_ptr<Finished> finished;
  HttpClient::Done done;
  std::string body;
  std::vector<std::string> header_text;
  std::vector<const char*> headers;
};

void on_finish(emscripten_fetch_t* fetch) {
  auto* request = static_cast<WebRequest*>(fetch->userData);

  HttpResponse res;
  res.status = fetch->status;
  if (fetch->data != nullptr && fetch->numBytes > 0) {
    res.body.assign(fetch->data, static_cast<std::size_t>(fetch->numBytes));
  }
  if (res.status == 0) {
    res.error = "could not reach play";
  }

  if (auto finished = request->finished.lock()) {
    finished->in_flight -= 1;
    finished->responses.emplace_back(std::move(request->done), std::move(res));
  }

  emscripten_fetch_close(fetch);
  delete request;
}

class WebHttpClient : public HttpClient {
 public:
  void request(const std::string& method,
               const std::string& url,
               const std::string& body,
               const std::string& token,
               Done done) override {
    auto* request = new WebRequest{finished_, std::move(done), body, {}, {}};

    request->header_text = {"Accept", "application/json"};
    if (!body.empty()) {
      request->header_text.insert(request->header_text.end(),
                                  {"Content-Type", "application/json"});
    }
    if (!token.empty()) {
      request->header_text.insert(request->header_text.end(),
                                  {"Authorization", "Bearer " + token});
    }
    for (const auto& text : request->header_text) {
      request->headers.push_back(text.c_str());
    }
    request->headers.push_back(nullptr);

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strncpy(attr.requestMethod, method.c_str(),
                 sizeof(attr.requestMethod) - 1);
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.timeoutMSecs = TIMEOUT_MS;
    attr.requestHeaders = request->headers.data();
    if (!request->body.empty()) {
      attr.requestData = request->body.data();
      attr.requestDataSize = request->body.size();
    }
    // Both run on the main thread between frames. Error responses (4xx, 5xx)
    // also land in onerror, with their body.
    attr.onsuccess = on_finish;
    attr.onerror = on_finish;
    attr.userData = request;

    finished_->in_flight += 1;
    emscripten_fetch(&attr, url.c_str());
  }

  void poll() override {
    std::vector<std::pair<Done, HttpResponse>> responses;
    responses.swap(finished_->responses);
    for (auto& [done, res] : responses) {
      done(std::move(res));
    }
  }

  std::size_t pending() const override {
    return finished_->in_flight + finished_->responses.size();
  }

 private:
  std::shared_ptr<Finished> finished_ = std::make_shared<Finished>();
};

#endif

}  // namespace

std::unique_ptr<HttpClient> HttpClient::create() {
#if defined(ADSGAMES_PLAY_NETWORK)
  return std::make_unique<WebHttpClient>();
#else
  return nullptr;
#endif
}

}  // namespace adsgames::play

#endif
