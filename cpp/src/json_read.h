/**
 * Readers for play responses
 *
 * Emscripten builds do not catch exceptions by default, so nothing here
 * throws. Missing or mistyped fields read as empty values.
 */

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

#include "adsgames/play/types.h"

namespace adsgames::play::json_read {

std::optional<Session> session(const nlohmann::json& j);

std::optional<Leaderboard> leaderboard(const nlohmann::json& j);

std::optional<std::vector<Leaderboard>> leaderboards(const nlohmann::json& j);

std::optional<LeaderboardPage> leaderboard_page(const nlohmann::json& j);

std::optional<SubmitScoreResult> submit_score(const nlohmann::json& j);

std::optional<std::vector<Achievement>> achievements(const nlohmann::json& j);

std::optional<UnlockResult> unlock(const nlohmann::json& j);

std::optional<std::vector<StatRule>> stat_rules(const nlohmann::json& j);

// Response of /api/device/start on adsgames.net
struct DeviceStart {
    std::string device_code;
    LinkCode code;
    int64_t interval { 5 };
};

std::optional<DeviceStart> device_start(const nlohmann::json& j);

// Response of /api/device/token and /api/device/refresh. device_token is
// only in the first.
struct DeviceTokens {
    std::string device_token;
    std::string access_token;
    int64_t expires_in { 0 };
    Linked player;
};

std::optional<DeviceTokens> device_tokens(const nlohmann::json& j);

// The error field of an error response, empty if there is none
std::string error_code(const nlohmann::json& j);

// Reads the result type T of a request
template <typename T> std::optional<T> read(const nlohmann::json& j);

template <> std::optional<Session> read(const nlohmann::json& j);
template <> std::optional<std::vector<Leaderboard>> read(const nlohmann::json& j);
template <> std::optional<LeaderboardPage> read(const nlohmann::json& j);
template <> std::optional<SubmitScoreResult> read(const nlohmann::json& j);
template <> std::optional<std::vector<Achievement>> read(const nlohmann::json& j);
template <> std::optional<UnlockResult> read(const nlohmann::json& j);
template <> std::optional<std::vector<StatRule>> read(const nlohmann::json& j);

} // namespace adsgames::play::json_read
