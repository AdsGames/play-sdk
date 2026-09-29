#include "adsgames/play/client.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "json_read.h"
#include "login_store.h"
#include "transport.h"

namespace adsgames::play {

namespace {

    // Encodes a path segment, keys are already limited to [a-z0-9_-]
    std::string encode(const std::string& value)
    {
        constexpr std::string_view HEX = "0123456789ABCDEF";

        std::string out;
        for (const unsigned char c : value) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += HEX[static_cast<unsigned>(c) >> 4U];
                out += HEX[static_cast<unsigned>(c) & 0xFU];
            }
        }
        return out;
    }

    // Never throws, bad UTF-8 in meta is replaced
    std::string dump(const nlohmann::json& j)
    {
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    bool is_success(const HttpResponse& res)
    {
        return res.status >= 200 && res.status < 300;
    }

    // How often a game checks the shared login for changes by other games
    constexpr std::chrono::seconds LOGIN_SYNC_INTERVAL(5);

    // Refresh play tokens a minute early, so a request never carries an expired one
    std::chrono::seconds token_lifetime(int64_t expires_in)
    {
        return std::chrono::seconds(std::max<int64_t>(expires_in - 60, 30));
    }

} // namespace

Client::Client(ClientConfig config)
    : config_(std::move(config))
    , http_(HttpClient::create())
{
    if (config_.base_url.empty()) {
        config_.base_url = default_base_url();
    }
    while (!config_.base_url.empty() && config_.base_url.back() == '/') {
        config_.base_url.pop_back();
    }

    if (config_.site_url.empty()) {
        config_.site_url = default_site_url(config_.base_url);
    }
    while (!config_.site_url.empty() && config_.site_url.back() == '/') {
        config_.site_url.pop_back();
    }

    device_token_ = config_.device_token;

#if !defined(__EMSCRIPTEN__)
    if (config_.token.empty()) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (const char* token = std::getenv("ADSGAMES_PLAY_TOKEN"); token != nullptr) {
            config_.token = token;
        }
    }
#endif

    if (config_.shared_login) {
        login_path_ = login_store::path_for(config_.site_url);

        if (device_token_.empty()) {
            device_token_ = login_store::load(login_path_);
        } else if (login_store::load(login_path_).empty()) {
            // A game that kept its own token shares it from now on
            login_store::save(login_path_, device_token_);
        }
        next_login_sync_ = Clock::now() + LOGIN_SYNC_INTERVAL;
    }
}

Client::~Client() = default;

void Client::session(Callback<Session> done)
{
    request<Session>("GET", game_path("/session"), "", std::move(done));
}

void Client::leaderboards(Callback<std::vector<Leaderboard>> done)
{
    request<std::vector<Leaderboard>>("GET", game_path("/leaderboards"), "", std::move(done));
}

void Client::leaderboard(const std::string& key, int limit, Callback<LeaderboardPage> done)
{
    request<LeaderboardPage>("GET",
        game_path("/leaderboards/" + encode(key) + "?limit=" + std::to_string(limit)), "",
        std::move(done));
}

void Client::submit_score(const std::string& key, int64_t value, const nlohmann::json& meta,
    Callback<SubmitScoreResult> done)
{
    nlohmann::json body = { { "value", value } };
    if (!meta.is_null()) {
        body["meta"] = meta;
    }
    request<SubmitScoreResult>(
        "POST", game_path("/leaderboards/" + encode(key) + "/scores"), dump(body), std::move(done));
}

void Client::achievements(Callback<std::vector<Achievement>> done)
{
    request<std::vector<Achievement>>("GET", game_path("/achievements"), "", std::move(done));
}

void Client::unlock(const std::string& key, Callback<UnlockResult> done)
{
    request<UnlockResult>(
        "POST", game_path("/achievements/" + encode(key) + "/unlock"), "", std::move(done));
}

void Client::link(const std::string& device_name, std::function<void(const LinkCode&)> on_code,
    Callback<Linked> done)
{
    cancel_link();

    if (!http_) {
        local_.emplace_back([done = std::move(done)] {
            if (done) {
                done(Result<Linked>::failure(0, "online features are not in this build"));
            }
        });
        return;
    }

    const int generation = link_.generation;
    link_.active = true;
    link_.in_flight = true;
    link_.done = std::move(done);

    const nlohmann::json body = { { "game", config_.game }, { "name", device_name } };
    http_->request("POST", config_.site_url + "/api/device/start", dump(body), "",
        [this, generation, on_code = std::move(on_code)](const HttpResponse& res) {
            if (generation != link_.generation || !link_.active) {
                return;
            }
            link_.in_flight = false;

            const auto j = nlohmann::json::parse(res.body, nullptr, false);
            const auto start = is_success(res) ? json_read::device_start(j) : std::nullopt;
            if (!start) {
                const auto code = json_read::error_code(j);
                finish_link(Result<Linked>::failure(
                    res.status, code.empty() ? "could not start linking" : code));
                return;
            }

            const auto now = Clock::now();
            link_.device_code = start->device_code;
            link_.interval = std::chrono::seconds(std::max<int64_t>(start->interval, 1));
            link_.expires = now
                + std::chrono::seconds(start->code.expires_in > 0 ? start->code.expires_in : 600);
            link_.next_poll = now + link_.interval;

            if (on_code) {
                on_code(start->code);
            }
        });
}

void Client::link(std::function<void(const LinkCode&)> on_code, Callback<Linked> done)
{
    link(default_device_name(), std::move(on_code), std::move(done));
}

void Client::cancel_link()
{
    link_.generation++;
    link_.active = false;
    link_.in_flight = false;
    link_.device_code.clear();
    link_.done = nullptr;
}

void Client::finish_link(const Result<Linked>& result)
{
    const auto done = std::move(link_.done);
    cancel_link();
    if (done) {
        done(result);
    }
}

void Client::poll_link()
{
    if (!link_.active || link_.in_flight || link_.device_code.empty() || !http_) {
        return;
    }

    const auto now = Clock::now();
    if (now >= link_.expires) {
        finish_link(Result<Linked>::failure(400, "expired_token"));
        return;
    }
    if (now < link_.next_poll) {
        return;
    }

    const int generation = link_.generation;
    link_.in_flight = true;

    const nlohmann::json body = { { "deviceCode", link_.device_code } };
    http_->request("POST", config_.site_url + "/api/device/token", dump(body), "",
        [this, generation](const HttpResponse& res) {
            if (generation != link_.generation || !link_.active) {
                return;
            }
            link_.in_flight = false;

            const auto j = nlohmann::json::parse(res.body, nullptr, false);

            if (is_success(res)) {
                const auto tokens = json_read::device_tokens(j);
                if (!tokens || tokens->device_token.empty()) {
                    finish_link(
                        Result<Linked>::failure(res.status, "unexpected response from site"));
                    return;
                }

                access_token_ = tokens->access_token;
                access_expires_ = Clock::now() + token_lifetime(tokens->expires_in);
                set_device_token(tokens->device_token);
                finish_link(Result<Linked>::success(tokens->player, res.status));
                return;
            }

            // Keep polling while the player has not decided, or the site is down
            const auto code = json_read::error_code(j);
            if (res.status == 0 || code == "authorization_pending") {
                link_.next_poll = Clock::now() + link_.interval;
                return;
            }
            if (code == "slow_down") {
                link_.interval += std::chrono::seconds(5);
                link_.next_poll = Clock::now() + link_.interval;
                return;
            }

            // access_denied, expired_token or an unexpected error
            finish_link(
                Result<Linked>::failure(res.status, code.empty() ? "linking failed" : code));
        });
}

void Client::unlink()
{
    cancel_link();

    if (!device_token_.empty() && http_) {
        const nlohmann::json body = { { "deviceToken", device_token_ } };
        http_->request("POST", config_.site_url + "/api/device/unlink", dump(body), "",
            [](const HttpResponse&) { });
    }

    access_token_.clear();
    if (!device_token_.empty()) {
        set_device_token("");
    }
}

void Client::set_device_token(std::string token)
{
    device_token_ = std::move(token);
    login_store::save(login_path_, device_token_);
    if (config_.on_device_token) {
        config_.on_device_token(device_token_);
    }
}

void Client::sync_shared_login()
{
    const auto now = Clock::now();
    if (login_path_.empty() || now < next_login_sync_) {
        return;
    }
    next_login_sync_ = now + LOGIN_SYNC_INTERVAL;

    auto shared = login_store::load(login_path_);
    if (shared == device_token_) {
        return;
    }

    // Another game linked this device, or logged it out
    device_token_ = std::move(shared);
    access_token_.clear();
    if (config_.on_device_token) {
        config_.on_device_token(device_token_);
    }
}

void Client::device_token_rejected()
{
    const auto rejected = device_token_;
    access_token_.clear();

    // Another game may have linked again since this token was read. Only
    // forget the shared login if it is the token the site turned down.
    const auto shared = login_store::load(login_path_);
    device_token_ = shared == rejected ? "" : shared;
    if (shared == rejected) {
        login_store::save(login_path_, "");
    }

    if (config_.on_device_token) {
        config_.on_device_token(device_token_);
    }
}

void Client::refresh_access()
{
    if (refreshing_) {
        return;
    }
    refreshing_ = true;

    const nlohmann::json body = { { "deviceToken", device_token_ } };
    http_->request("POST", config_.site_url + "/api/device/refresh", dump(body), "",
        [this, sent = device_token_](const HttpResponse& res) {
            refreshing_ = false;

            const auto j = nlohmann::json::parse(res.body, nullptr, false);
            if (sent != device_token_) {
                // The device logged out or linked again meanwhile, so the answer is
                // for an old token. Refresh the new one if it has no play token yet.
                if (!device_token_.empty()
                    && (access_token_.empty() || Clock::now() >= access_expires_)) {
                    refresh_access();
                    return;
                }
            } else if (is_success(res)) {
                if (const auto tokens = json_read::device_tokens(j)) {
                    access_token_ = tokens->access_token;
                    access_expires_ = Clock::now() + token_lifetime(tokens->expires_in);
                }
            } else if (res.status == 401) {
                // The player removed this device on adsgames.net, or it went unused
                // for too long
                device_token_rejected();

                // A newer shared login, try that one
                if (!device_token_.empty()) {
                    refresh_access();
                    return;
                }
            }
            // If the site could not be reached, the requests go out as a guest

            std::vector<std::function<void()>> waiting;
            waiting.swap(waiting_);
            for (const auto& run : waiting) {
                run();
            }
        });
}

void Client::achievement_rules(Callback<std::vector<StatRule>> done)
{
    request<std::vector<StatRule>>("GET", game_path("/achievements/rules"), "", std::move(done));
}

void Client::update()
{
    // Callbacks can start new requests, so work on a copy
    std::vector<std::function<void()>> local;
    local.swap(local_);
    for (const auto& run : local) {
        run();
    }

    if (http_) {
        http_->poll();
    }

    poll_link();
}

std::size_t Client::pending() const
{
    return local_.size() + waiting_.size() + (http_ ? http_->pending() : 0);
}

std::string Client::multiplayer_url() const
{
    return multiplayer_url_for(config_.base_url);
}

std::shared_ptr<Multiplayer> Client::host(int version) const
{
    return Multiplayer::host(multiplayer_url(), config_.game, version);
}

std::shared_ptr<Multiplayer> Client::join(const std::string& code, int version) const
{
    return Multiplayer::join(multiplayer_url(), config_.game, version, code);
}

std::string Client::game_path(const std::string& rest) const
{
    return "/games/" + encode(config_.game) + rest;
}

template <typename T>
void Client::request(
    const std::string& method, const std::string& path, const std::string& body, Callback<T> done)
{
    if (!http_) {
        local_.emplace_back([done = std::move(done)] {
            if (done) {
                done(Result<T>::failure(0, "online features are not in this build"));
            }
        });
        return;
    }

    sync_shared_login();

    // A linked device needs a current play token first
    if (!device_token_.empty() && (access_token_.empty() || Clock::now() >= access_expires_)) {
        waiting_.emplace_back(
            [this, method, path, body, done] { send<T>(method, path, body, done); });
        refresh_access();
        return;
    }

    send<T>(method, path, body, std::move(done));
}

template <typename T>
void Client::send(
    const std::string& method, const std::string& path, const std::string& body, Callback<T> done)
{
    const std::string play_token = access_token_;

    http_->request(method, config_.base_url + path, body,
        play_token.empty() ? config_.token : play_token,
        [this, play_token, done = std::move(done)](const HttpResponse& res) {
            // play turned the token down, get a new one for the next request.
            // Keep a newer token got while this request ran.
            if (!play_token.empty() && res.status == 401 && access_token_ == play_token) {
                access_token_.clear();
            }

            if (!done) {
                return;
            }
            if (res.status == 0) {
                done(Result<T>::failure(0, res.error.empty() ? "could not reach play" : res.error));
                return;
            }

            // Anything the server sends is untrusted input
            const auto j = nlohmann::json::parse(res.body, nullptr, false);

            if (res.status < 200 || res.status >= 300) {
                const bool has_error
                    = j.is_object() && j.contains("error") && j["error"].is_string();
                done(Result<T>::failure(res.status,
                    has_error ? j["error"].get<std::string>()
                              : "request failed with " + std::to_string(res.status)));
                return;
            }

            if (auto value = json_read::read<T>(j)) {
                done(Result<T>::success(std::move(*value), res.status));
            } else {
                done(Result<T>::failure(res.status, "unexpected response from play"));
            }
        });
}

} // namespace adsgames::play
