/**
 * Response shapes of the play API. Times are ISO 8601 strings.
 */

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace adsgames::play {

struct Session {
  bool logged_in{false};
  std::string user_id;
  std::string username;
};

struct Leaderboard {
  std::string key;
  std::string title;

  // "asc" means lower is better, for example times
  std::string order;

  // "integer" or "time_ms"
  std::string format;

  std::optional<int64_t> min_value;
  std::optional<int64_t> max_value;
};

struct Score {
  int64_t rank{0};
  std::string user_id;
  std::string username;
  int64_t value{0};
  nlohmann::json meta;
  std::string updated_at;
};

struct LeaderboardPage {
  Leaderboard leaderboard;
  std::vector<Score> scores;

  // The player's own best, empty for guests or players with no score
  std::optional<Score> me;
};

struct SubmitScoreResult {
  // False when the player's best did not change
  bool improved{false};
  Score best;
};

struct Achievement {
  // Empty for hidden achievements the player has not unlocked
  std::string key;
  std::string title;
  std::string description;
  std::string icon;
  bool hidden{false};
  int points{0};
  int64_t unlock_count{0};

  // Empty when the player does not have it
  std::string unlocked_at;
};

struct UnlockResult {
  // False when the player already had it
  bool newly_unlocked{false};
  std::string unlocked_at;
};

}  // namespace adsgames::play
