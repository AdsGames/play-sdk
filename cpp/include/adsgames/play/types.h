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
    bool logged_in { false };
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
    int64_t rank { 0 };
    std::string user_id;
    std::string username;
    int64_t value { 0 };
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
    bool improved { false };
    Score best;
};

struct Achievement {
    // Empty for hidden achievements the player has not unlocked
    std::string key;
    std::string title;
    std::string description;
    std::string icon;
    bool hidden { false };
    int points { 0 };
    int64_t unlock_count { 0 };

    // Empty when the player does not have it
    std::string unlocked_at;
};

// Unlocks achievement key once the game's stat reaches at_least, see
// Game::add_stat()
struct StatRule {
    std::string key;
    std::string stat;
    int64_t at_least { 0 };
};

// Code for the player to enter on adsgames.net/link, see Client::link()
struct LinkCode {
    // Short code to show, e.g. "BCDF-GHJK"
    std::string user_code;

    // Page where the player enters the code
    std::string verification_uri;

    // The same page with the code filled in, e.g. for a QR code or a browser
    std::string verification_uri_complete;

    // Seconds until the code runs out
    int64_t expires_in { 0 };
};

// The player a device was linked to
struct Linked {
    std::string user_id;
    std::string username;
};

struct UnlockResult {
    // False when the player already had it
    bool newly_unlocked { false };
    std::string unlocked_at;
};

} // namespace adsgames::play
