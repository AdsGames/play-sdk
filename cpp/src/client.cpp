#include "adsgames/play/client.h"

#include <utility>

#include "json_read.h"
#include "transport.h"

namespace adsgames::play {

namespace {

// Encodes a path segment, keys are already limited to [a-z0-9_-]
std::string encode(const std::string& value) {
  static constexpr char HEX[] = "0123456789ABCDEF";

  std::string out;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += HEX[c >> 4];
      out += HEX[c & 0xF];
    }
  }
  return out;
}

// Never throws, bad UTF-8 in meta is replaced
std::string dump(const nlohmann::json& j) {
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace

Client::Client(ClientConfig config)
    : config_(std::move(config)), http_(HttpClient::create()) {
  if (config_.base_url.empty()) {
    config_.base_url = default_base_url();
  }
  while (!config_.base_url.empty() && config_.base_url.back() == '/') {
    config_.base_url.pop_back();
  }
}

Client::~Client() = default;

void Client::session(Callback<Session> done) {
  request<Session>("GET", game_path("/session"), "", std::move(done));
}

void Client::leaderboards(Callback<std::vector<Leaderboard>> done) {
  request<std::vector<Leaderboard>>("GET", game_path("/leaderboards"), "",
                                    std::move(done));
}

void Client::leaderboard(const std::string& key,
                         int limit,
                         Callback<LeaderboardPage> done) {
  request<LeaderboardPage>(
      "GET",
      game_path("/leaderboards/" + encode(key) +
                "?limit=" + std::to_string(limit)),
      "", std::move(done));
}

void Client::submit_score(const std::string& key,
                          int64_t value,
                          const nlohmann::json& meta,
                          Callback<SubmitScoreResult> done) {
  nlohmann::json body = {{"value", value}};
  if (!meta.is_null()) {
    body["meta"] = meta;
  }
  request<SubmitScoreResult>(
      "POST", game_path("/leaderboards/" + encode(key) + "/scores"),
      dump(body), std::move(done));
}

void Client::achievements(Callback<std::vector<Achievement>> done) {
  request<std::vector<Achievement>>("GET", game_path("/achievements"), "",
                                    std::move(done));
}

void Client::unlock(const std::string& key, Callback<UnlockResult> done) {
  request<UnlockResult>("POST",
                        game_path("/achievements/" + encode(key) + "/unlock"),
                        "", std::move(done));
}

void Client::update() {
  // Callbacks can start new requests, so work on a copy
  std::vector<std::function<void()>> local;
  local.swap(local_);
  for (const auto& run : local) {
    run();
  }

  if (http_) {
    http_->poll();
  }
}

std::size_t Client::pending() const {
  return local_.size() + (http_ ? http_->pending() : 0);
}

std::string Client::multiplayer_url() const {
  return multiplayer_url_for(config_.base_url);
}

std::shared_ptr<Multiplayer> Client::host(int version) const {
  return Multiplayer::host(multiplayer_url(), config_.game, version);
}

std::shared_ptr<Multiplayer> Client::join(const std::string& code,
                                          int version) const {
  return Multiplayer::join(multiplayer_url(), config_.game, version, code);
}

std::string Client::game_path(const std::string& rest) const {
  return "/games/" + encode(config_.game) + rest;
}

template <typename T>
void Client::request(const std::string& method,
                     const std::string& path,
                     const std::string& body,
                     Callback<T> done) {
  if (!http_) {
    local_.emplace_back([done = std::move(done)] {
      if (done) {
        done(Result<T>::failure(0, "online features are not in this build"));
      }
    });
    return;
  }

  http_->request(
      method, config_.base_url + path, body, config_.token,
      [done = std::move(done)](const HttpResponse& res) {
        if (!done) {
          return;
        }
        if (res.status == 0) {
          done(Result<T>::failure(0, res.error.empty() ? "could not reach play"
                                                        : res.error));
          return;
        }

        // Anything the server sends is untrusted input
        const auto j = nlohmann::json::parse(res.body, nullptr, false);

        if (res.status < 200 || res.status >= 300) {
          const bool has_error = j.is_object() && j.contains("error") &&
                                 j["error"].is_string();
          done(Result<T>::failure(
              res.status, has_error ? j["error"].get<std::string>()
                                    : "request failed with " +
                                          std::to_string(res.status)));
          return;
        }

        if (auto value = json_read::read<T>(j)) {
          done(Result<T>::success(std::move(*value), res.status));
        } else {
          done(Result<T>::failure(res.status, "unexpected response from play"));
        }
      });
}

}  // namespace adsgames::play
