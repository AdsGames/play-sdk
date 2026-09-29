/**
 * Client for the play service of one game
 *
 * Requests run in the background. Their callbacks run on the game thread
 * from update(), so call it once per frame. Callbacks may be empty for
 * requests the game does not need an answer to.
 *
 * Web builds on adsgames.net send the player's session cookie. Desktop
 * builds have no cookie and play as guests unless given a token, so
 * submit_score() and unlock() fail with not_logged_in() for them.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "multiplayer.h"
#include "result.h"
#include "types.h"

namespace adsgames::play {

class HttpClient;

struct ClientConfig {
  // adsgames.net game slug, e.g. "mini-jim"
  std::string game;

  // Empty for the default, see default_base_url()
  std::string base_url;

  // adsgames.net JWT, sent as a bearer token. Not needed on the web.
  std::string token;
};

// "/api/play" on the web, ADSGAMES_PLAY_URL on desktop. Players can override
// it with ?play= on the page URL or the ADSGAMES_PLAY_URL environment variable.
std::string default_base_url();

class Client {
 public:
  template <typename T>
  using Callback = std::function<void(const Result<T>&)>;

  explicit Client(ClientConfig config);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  void session(Callback<Session> done);

  void leaderboards(Callback<std::vector<Leaderboard>> done);

  // Top scores of a board (1 to 100), and the player's own best
  void leaderboard(const std::string& key,
                   int limit,
                   Callback<LeaderboardPage> done);

  // Keeps the player's best. Use milliseconds for times. meta must be a json
  // object of at most 1 KB, or null.
  void submit_score(const std::string& key,
                    int64_t value,
                    const nlohmann::json& meta,
                    Callback<SubmitScoreResult> done);

  void achievements(Callback<std::vector<Achievement>> done);

  // Safe to call again for an achievement the player already has
  void unlock(const std::string& key, Callback<UnlockResult> done);

  // Run callbacks of finished requests, call once per frame
  void update();

  // Requests that have not called back yet
  std::size_t pending() const;

  // WebSocket URL of the multiplayer relay, next to the REST API
  std::string multiplayer_url() const;

  // Open a multiplayer room for this game
  std::shared_ptr<Multiplayer> host(int version) const;

  // Join a multiplayer room of this game by its code
  std::shared_ptr<Multiplayer> join(const std::string& code, int version) const;

  const std::string& game() const { return config_.game; }

  const std::string& base_url() const { return config_.base_url; }

 private:
  template <typename T>
  void request(const std::string& method,
               const std::string& path,
               const std::string& body,
               Callback<T> done);

  std::string game_path(const std::string& rest) const;

  ClientConfig config_;
  std::unique_ptr<HttpClient> http_;

  // Failures that happen before a request is sent, run from update()
  std::vector<std::function<void()>> local_;
};

}  // namespace adsgames::play
