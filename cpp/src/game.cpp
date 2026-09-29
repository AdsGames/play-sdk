#include "adsgames/play/game.h"

#include <nlohmann/json.hpp>
#include <utility>

#include "login_store.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

#include <cstdlib>
#endif

namespace adsgames::play {

namespace {

    // How long to wait before asking play again after a failure
    constexpr std::chrono::seconds RETRY(30);

    // How often to check who is playing while offline or a guest. A guest may
    // log in in another game on this machine.
    constexpr std::chrono::seconds SESSION_RECHECK(60);

    bool unreachable(int status)
    {
        return status == 0 || status == 404 || status >= 500;
    }

#if defined(__EMSCRIPTEN__)
// The emscripten file system is lost on reload, so web builds keep the game's
// state in localStorage. Storage can be blocked, which reads as empty.
// clang-format off
EM_JS(char*, play_storage_read, (const char* key), {
  try {
    const value = localStorage.getItem(UTF8ToString(key));
    return value === null ? 0 : stringToNewUTF8(value);
  } catch (e) {
    return 0;
  }
});

EM_JS(void, play_storage_write, (const char* key, const char* value), {
  try {
    localStorage.setItem(UTF8ToString(key), UTF8ToString(value));
  } catch (e) {
    // Only lasts for this page view
  }
});
    // clang-format on

    std::string read_state(const std::string& key)
    {
        char* value = play_storage_read(key.c_str());
        if (value == nullptr) {
            return "";
        }
        std::string out(value);
        free(value); // NOLINT(cppcoreguidelines-no-malloc), from emscripten
        return out;
    }

    void write_state(const std::string& key, const std::string& text)
    {
        play_storage_write(key.c_str(), text.c_str());
    }
#else
    std::string read_state(const std::string& path)
    {
        return login_store::read_text(path);
    }

    void write_state(const std::string& path, const std::string& text)
    {
        login_store::write_text(path, text);
    }
#endif

    // A member of a save, null when it is missing
    const nlohmann::json& field(const nlohmann::json& j, const char* key)
    {
        static const nlohmann::json missing;
        const auto it = j.find(key);
        return it != j.end() ? *it : missing;
    }

    // A member of a save that should be an object, empty when it is not
    const nlohmann::json& object_field(const nlohmann::json& j, const char* key)
    {
        static const nlohmann::json empty = nlohmann::json::object();
        const auto& value = field(j, key);
        return value.is_object() ? value : empty;
    }

    std::set<int64_t> integers(const nlohmann::json& values)
    {
        std::set<int64_t> out;
        if (values.is_array()) {
            for (const auto& value : values) {
                if (value.is_number_integer()) {
                    out.insert(value.get<int64_t>());
                }
            }
        }
        return out;
    }

    std::set<std::string> strings(const nlohmann::json& values)
    {
        std::set<std::string> out;
        if (values.is_array()) {
            for (const auto& value : values) {
                if (value.is_string()) {
                    out.insert(value.get<std::string>());
                }
            }
        }
        return out;
    }

} // namespace

Game::Game(ClientConfig config)
    : config_(std::move(config))
{
    // Re-check who is playing when the device logs in or out, in this game or
    // another one
    const auto on_device_token = config_.on_device_token;
    config_.on_device_token = [this, on_device_token](const std::string& token) {
        recheck_session();
        if (on_device_token) {
            on_device_token(token);
        }
    };

    client_ = std::make_unique<Client>(config_);

#if defined(__EMSCRIPTEN__)
    store_path_ = "adsgames-play:" + client_->site_url() + ":" + client_->game();
#else
    store_path_ = login_store::file_for(client_->site_url(), "game", client_->game());
#endif

    load();
}

Game::~Game() = default;

void Game::update()
{
    // Callbacks can queue more, so work on a copy
    std::vector<std::function<void()>> local;
    local.swap(local_);
    for (const auto& run : local) {
        run();
    }

    client_->update();

    const auto now = Clock::now();

    if (session_wanted_ && !session_in_flight_ && now >= next_session_) {
        check_session();
    }

    if (!rules_known_ && !rules_in_flight_ && now >= next_rules_
        && player_.status != PlayerStatus::Unknown && player_.status != PlayerStatus::Offline) {
        rules_in_flight_ = true;
        client_->achievement_rules([this](const auto& result) {
            rules_in_flight_ = false;
            if (!result.ok()) {
                next_rules_ = Clock::now() + RETRY;
                return;
            }
            rules_ = result.value();
            rules_known_ = true;
            apply_rules();
        });
    }

    if (now >= next_send_) {
        send_unlocks();
    }

    fetch_notices();
}

void Game::recheck_session()
{
    session_wanted_ = true;
    next_session_ = { };
}

void Game::check_session()
{
    session_wanted_ = false;
    session_in_flight_ = true;

    client_->session([this](const auto& result) {
        session_in_flight_ = false;

        if (!result.ok()) {
            player_ = Player {
                .status
                = unreachable(result.status()) ? PlayerStatus::Offline : PlayerStatus::Guest,
                .user_id = { },
                .username = { },
            };
            session_wanted_ = true;
            next_session_ = Clock::now() + SESSION_RECHECK;
            return;
        }

        const bool was_logged_in = player_.status == PlayerStatus::LoggedIn;
        player_ = Player {
            .status = result->logged_in ? PlayerStatus::LoggedIn : PlayerStatus::Guest,
            .user_id = result->user_id,
            .username = result->username,
        };

        // Progress made before this login counts for this player
        if (player_.status == PlayerStatus::LoggedIn && !was_logged_in) {
            apply_rules();
            next_send_ = { };
        }

        if (player_.status == PlayerStatus::Guest) {
            session_wanted_ = true;
            next_session_ = Clock::now() + SESSION_RECHECK;
        }
    });
}

int64_t Game::stat(const std::string& name) const
{
    if (const auto it = counters_.find(name); it != counters_.end()) {
        return it->second;
    }
    if (const auto it = sets_.find(name); it != sets_.end()) {
        return static_cast<int64_t>(it->second.size());
    }
    return 0;
}

void Game::add_stat(const std::string& name, int64_t amount)
{
    counters_[name] += amount;
    save();
    apply_rules();
}

void Game::add_to_stat_set(const std::string& name, int64_t value)
{
    if (sets_[name].insert(value).second) {
        save();
        apply_rules();
    }
}

void Game::apply_rules()
{
    for (const auto& rule : rules_) {
        if (stat(rule.stat) >= rule.at_least) {
            unlock(rule.key);
        }
    }
}

void Game::unlock(const std::string& key)
{
    if (unlocked(key) || pending_.contains(key)) {
        return;
    }
    pending_.insert(key);
    save();
    send_unlocks();
}

bool Game::unlocked(const std::string& key) const
{
    if (player_.status != PlayerStatus::LoggedIn) {
        return false;
    }
    const auto it = confirmed_.find(player_.user_id);
    return it != confirmed_.end() && it->second.contains(key);
}

void Game::send_unlocks()
{
    if (player_.status != PlayerStatus::LoggedIn) {
        return;
    }

    const auto user_id = player_.user_id;
    for (const auto& key : pending_) {
        if (sending_.contains(key)) {
            continue;
        }

        sending_.insert(key);
        client_->unlock(key, [this, key, user_id](const auto& result) {
            sending_.erase(key);

            if (result.ok()) {
                pending_.erase(key);
                confirmed_[user_id].insert(key);
                save();
                if (result->newly_unlocked) {
                    notice_keys_.push_back(key);
                }
                return;
            }

            if (result.not_logged_in()) {
                // The login ran out or was removed, find out who is playing now
                recheck_session();
            } else if (result.status() == 404) {
                // Not in the game's manifest, sending again will not help
                pending_.erase(key);
                save();
                return;
            }
            next_send_ = Clock::now() + RETRY;
        });
    }
}

void Game::fetch_notices()
{
    if (notice_keys_.empty() || notices_in_flight_) {
        return;
    }

    // Hidden achievements only have a title once unlocked, so ask play
    const auto keys = std::move(notice_keys_);
    notice_keys_.clear();
    notices_in_flight_ = true;

    client_->achievements([this, keys](const auto& result) {
        notices_in_flight_ = false;

        for (const auto& key : keys) {
            UnlockNotice notice {
                .key = key,
                .title = key,
                .description = { },
                .icon = { },
                .points = 0,
            };
            if (result.ok()) {
                for (const auto& achievement : result.value()) {
                    if (achievement.key == key) {
                        notice.title = achievement.title;
                        notice.description = achievement.description;
                        notice.icon = achievement.icon;
                        notice.points = achievement.points;
                        break;
                    }
                }
            }
            notices_.push_back(std::move(notice));
        }
    });
}

std::optional<UnlockNotice> Game::next_unlock()
{
    if (notices_.empty()) {
        return std::nullopt;
    }
    auto notice = std::move(notices_.front());
    notices_.pop_front();
    return notice;
}

void Game::submit_score(const std::string& key, int64_t value, const nlohmann::json& meta,
    Client::Callback<SubmitScoreResult> done)
{
    // Guests get 401 anyway, so do not ask
    if (player_.status == PlayerStatus::Guest || player_.status == PlayerStatus::Offline) {
        const int status = player_.status == PlayerStatus::Guest ? 401 : 0;
        local_.emplace_back([done = std::move(done), status] {
            if (done) {
                done(Result<SubmitScoreResult>::failure(
                    status, status == 401 ? "log in to save scores" : "could not reach play"));
            }
        });
        return;
    }

    client_->submit_score(key, value, meta, [this, done = std::move(done)](const auto& result) {
        if (result.not_logged_in()) {
            recheck_session();
        }
        if (done) {
            done(result);
        }
    });
}

void Game::link(std::function<void(const LinkCode&)> on_code, Client::Callback<Linked> done)
{
    client_->link(std::move(on_code), [this, done = std::move(done)](const auto& result) {
        recheck_session();
        if (done) {
            done(result);
        }
    });
}

void Game::unlink()
{
    client_->unlink();
    recheck_session();
}

void Game::load()
{
    const auto j = nlohmann::json::parse(read_state(store_path_), nullptr, false);
    if (!j.is_object()) {
        return;
    }

    // Missing or mistyped values are skipped, a damaged save never crashes
    for (const auto& [name, value] : object_field(j, "counters").items()) {
        if (value.is_number_integer()) {
            counters_[name] = value.get<int64_t>();
        }
    }

    for (const auto& [name, values] : object_field(j, "sets").items()) {
        sets_[name] = integers(values);
    }

    pending_ = strings(field(j, "pending"));

    for (const auto& [user, keys] : object_field(j, "confirmed").items()) {
        confirmed_[user] = strings(keys);
    }
}

void Game::save() const
{
    if (store_path_.empty()) {
        return;
    }

    nlohmann::json sets = nlohmann::json::object();
    for (const auto& [name, values] : sets_) {
        sets[name] = values;
    }

    const nlohmann::json j = {
        { "counters", counters_ },
        { "sets", sets },
        { "pending", pending_ },
        { "confirmed", confirmed_ },
    };

    write_state(store_path_, j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

} // namespace adsgames::play
