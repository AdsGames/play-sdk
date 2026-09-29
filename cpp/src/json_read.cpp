#include "json_read.h"

#include <string>

namespace adsgames::play::json_read {

namespace {

    using nlohmann::json;

    std::string str(const json& j, const char* key)
    {
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : "";
    }

    std::optional<int64_t> opt_int(const json& j, const char* key)
    {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_number_integer()) {
            return std::nullopt;
        }
        return it->get<int64_t>();
    }

    int64_t integer(const json& j, const char* key)
    {
        return opt_int(j, key).value_or(0);
    }

    bool boolean(const json& j, const char* key)
    {
        const auto it = j.find(key);
        return it != j.end() && it->is_boolean() && it->get<bool>();
    }

    std::optional<Score> score(const json& j)
    {
        if (!j.is_object()) {
            return std::nullopt;
        }

        Score s;
        s.rank = integer(j, "rank");
        s.user_id = str(j, "userId");
        s.username = str(j, "username");
        s.value = integer(j, "value");
        s.updated_at = str(j, "updatedAt");
        if (const auto it = j.find("meta"); it != j.end() && it->is_object()) {
            s.meta = *it;
        }
        return s;
    }

    // Reads the array in field key with read_one, skipping bad entries
    template <typename T, typename F>
    std::optional<std::vector<T>> list(const json& j, const char* key, F read_one)
    {
        if (!j.is_object()) {
            return std::nullopt;
        }
        const auto it = j.find(key);
        if (it == j.end() || !it->is_array()) {
            return std::nullopt;
        }

        std::vector<T> items;
        for (const auto& item : *it) {
            if (auto value = read_one(item)) {
                items.push_back(std::move(*value));
            }
        }
        return items;
    }

} // namespace

std::optional<Session> session(const json& j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    return Session {
        .logged_in = boolean(j, "loggedIn"),
        .user_id = str(j, "userId"),
        .username = str(j, "username"),
    };
}

std::optional<Leaderboard> leaderboard(const json& j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }

    Leaderboard lb;
    lb.key = str(j, "key");
    lb.title = str(j, "title");
    lb.order = str(j, "order");
    lb.format = str(j, "format");
    lb.min_value = opt_int(j, "minValue");
    lb.max_value = opt_int(j, "maxValue");
    return lb;
}

std::optional<std::vector<Leaderboard>> leaderboards(const json& j)
{
    return list<Leaderboard>(j, "leaderboards", leaderboard);
}

std::optional<LeaderboardPage> leaderboard_page(const json& j)
{
    if (!j.is_object() || !j.contains("leaderboard")) {
        return std::nullopt;
    }

    auto lb = leaderboard(j["leaderboard"]);
    auto scores = list<Score>(j, "scores", score);
    if (!lb || !scores) {
        return std::nullopt;
    }

    LeaderboardPage page;
    page.leaderboard = std::move(*lb);
    page.scores = std::move(*scores);
    if (const auto it = j.find("me"); it != j.end()) {
        page.me = score(*it);
    }
    return page;
}

std::optional<SubmitScoreResult> submit_score(const json& j)
{
    if (!j.is_object() || !j.contains("best")) {
        return std::nullopt;
    }

    auto best = score(j["best"]);
    if (!best) {
        return std::nullopt;
    }
    return SubmitScoreResult {
        .improved = boolean(j, "improved"),
        .best = std::move(*best),
    };
}

std::optional<std::vector<Achievement>> achievements(const json& j)
{
    return list<Achievement>(j, "achievements", [](const json& item) -> std::optional<Achievement> {
        if (!item.is_object()) {
            return std::nullopt;
        }

        Achievement a;
        a.key = str(item, "key");
        a.title = str(item, "title");
        a.description = str(item, "description");
        a.icon = str(item, "icon");
        a.hidden = boolean(item, "hidden");
        a.points = static_cast<int>(integer(item, "points"));
        a.unlock_count = integer(item, "unlockCount");
        a.unlocked_at = str(item, "unlockedAt");
        return a;
    });
}

std::optional<UnlockResult> unlock(const json& j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    return UnlockResult {
        .newly_unlocked = boolean(j, "newlyUnlocked"),
        .unlocked_at = str(j, "unlockedAt"),
    };
}

std::optional<std::vector<StatRule>> stat_rules(const json& j)
{
    return list<StatRule>(j, "rules", [](const json& item) -> std::optional<StatRule> {
        if (!item.is_object()) {
            return std::nullopt;
        }
        StatRule rule;
        rule.key = str(item, "key");
        rule.stat = str(item, "stat");
        rule.at_least = integer(item, "atLeast");
        if (rule.key.empty() || rule.stat.empty() || rule.at_least < 1) {
            return std::nullopt;
        }
        return rule;
    });
}

std::optional<DeviceStart> device_start(const json& j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }

    DeviceStart start;
    start.device_code = str(j, "deviceCode");
    start.code.user_code = str(j, "userCode");
    start.code.verification_uri = str(j, "verificationUri");
    start.code.verification_uri_complete = str(j, "verificationUriComplete");
    start.code.expires_in = integer(j, "expiresIn");
    start.interval = opt_int(j, "interval").value_or(5);

    if (start.device_code.empty() || start.code.user_code.empty()) {
        return std::nullopt;
    }
    return start;
}

std::optional<DeviceTokens> device_tokens(const json& j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }

    DeviceTokens tokens;
    tokens.device_token = str(j, "deviceToken");
    tokens.access_token = str(j, "accessToken");
    tokens.expires_in = integer(j, "expiresIn");
    tokens.player.user_id = str(j, "userId");
    tokens.player.username = str(j, "username");

    if (tokens.access_token.empty()) {
        return std::nullopt;
    }
    return tokens;
}

std::string error_code(const json& j)
{
    return j.is_object() ? str(j, "error") : "";
}

template <> std::optional<Session> read(const json& j)
{
    return session(j);
}

template <> std::optional<std::vector<Leaderboard>> read(const json& j)
{
    return leaderboards(j);
}

template <> std::optional<LeaderboardPage> read(const json& j)
{
    return leaderboard_page(j);
}

template <> std::optional<SubmitScoreResult> read(const json& j)
{
    return submit_score(j);
}

template <> std::optional<std::vector<Achievement>> read(const json& j)
{
    return achievements(j);
}

template <> std::optional<UnlockResult> read(const json& j)
{
    return unlock(j);
}

template <> std::optional<std::vector<StatRule>> read(const json& j)
{
    return stat_rules(j);
}

} // namespace adsgames::play::json_read
