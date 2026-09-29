/**
 * Client for the play service of one game
 *
 * Requests run in the background. Their callbacks run on the game thread
 * from update(), so call it once per frame. Callbacks may be empty for
 * requests the game does not need an answer to.
 *
 * Web builds on adsgames.net send the player's session cookie. Desktop
 * builds have no cookie and play as guests until the player links the device
 * with link(). Guests get not_logged_in() from submit_score() and unlock().
 */

#pragma once

#include <chrono>
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

    // adsgames.net JWT, sent as a bearer token. Not needed on the web. Desktop
    // builds read ADSGAMES_PLAY_TOKEN when this is empty, for testing.
    std::string token;

    // Share the device login with every A.D.S. Games game on this machine.
    // Desktop builds then load and save the device token themselves, and a
    // link or log out in one game applies to all of them. See login_store.h.
    bool shared_login { true };

    // Device token saved from an earlier link(), for games that keep it
    // themselves. The client trades it for short lived play tokens, so the
    // player stays logged in.
    std::string device_token;

    // Called with a new device token after link(), and with "" once the
    // device is unlinked or the player removed it on adsgames.net. Save it to
    // keep the player logged in on this device.
    std::function<void(const std::string&)> on_device_token;

    // adsgames.net, where devices link. Empty for the default, see
    // default_site_url().
    std::string site_url;
};

// "/api/play" on the web, ADSGAMES_PLAY_URL on desktop. Players can override
// it with ?play= on the page URL or the ADSGAMES_PLAY_URL environment variable.
std::string default_base_url();

// The site in front of base_url, e.g. "https://www.adsgames.net" for
// "https://www.adsgames.net/api/play". https://www.adsgames.net when the play
// URL is not on a site.
std::string default_site_url(const std::string& base_url);

// Name for this machine when linking, e.g. "Jims-MacBook-Pro (macOS)"
std::string default_device_name();

class Client {
public:
    template <typename T> using Callback = std::function<void(const Result<T>&)>;

    explicit Client(ClientConfig config);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    void session(Callback<Session> done);

    void leaderboards(Callback<std::vector<Leaderboard>> done);

    // Top scores of a board (1 to 100), and the player's own best
    void leaderboard(const std::string& key, int limit, Callback<LeaderboardPage> done);

    // Keeps the player's best. Use milliseconds for times. meta must be a json
    // object of at most 1 KB, or null.
    void submit_score(const std::string& key, int64_t value, const nlohmann::json& meta,
        Callback<SubmitScoreResult> done);

    void achievements(Callback<std::vector<Achievement>> done);

    // Safe to call again for an achievement the player already has
    void unlock(const std::string& key, Callback<UnlockResult> done);

    // Achievements that unlock from a stat, including hidden ones
    void achievement_rules(Callback<std::vector<StatRule>> done);

    // Log this device in to an adsgames.net account. on_code gets a code for
    // the player to enter at code.verification_uri. done runs once the player
    // approves (with the player), denies, or the code runs out. On approval
    // the client is logged in and on_device_token gets the token to save.
    void link(const std::string& device_name, std::function<void(const LinkCode&)> on_code,
        Callback<Linked> done);

    // link() with default_device_name()
    void link(std::function<void(const LinkCode&)> on_code, Callback<Linked> done);

    // Stop waiting for a link. done is not called.
    void cancel_link();

    bool linking() const
    {
        return link_.active;
    }

    // True when the client has a device token
    bool linked() const
    {
        return !device_token_.empty();
    }

    // Log this device out, and unlink it on adsgames.net
    void unlink();

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

    const std::string& game() const
    {
        return config_.game;
    }

    const std::string& base_url() const
    {
        return config_.base_url;
    }

    const std::string& site_url() const
    {
        return config_.site_url;
    }

private:
    using Clock = std::chrono::steady_clock;

    // Sends a request with the current token
    template <typename T>
    void send(const std::string& method, const std::string& path, const std::string& body,
        Callback<T> done);

    // Trades the device token for a play token, then runs the waiting requests
    void refresh_access();

    // Sets or clears the device token, saves it for other games and tells
    // the game
    void set_device_token(std::string token);

    // Picks up a link or log out made by another game, at most every few
    // seconds
    void sync_shared_login();

    // The site turned the device token down
    void device_token_rejected();

    void poll_link();

    // Ends the current link and calls its done callback
    void finish_link(const Result<Linked>& result);

    template <typename T>
    void request(const std::string& method, const std::string& path, const std::string& body,
        Callback<T> done);

    std::string game_path(const std::string& rest) const;

    ClientConfig config_;
    std::unique_ptr<HttpClient> http_;

    // Failures that happen before a request is sent, run from update()
    std::vector<std::function<void()>> local_;

    // Device login, see link()
    std::string login_path_;
    Clock::time_point next_login_sync_;
    std::string device_token_;
    std::string access_token_;
    Clock::time_point access_expires_;
    bool refreshing_ { false };

    // Requests waiting for a play token
    std::vector<std::function<void()>> waiting_;

    struct LinkState {
        bool active { false };
        bool in_flight { false };
        // Ignores answers for a link that was cancelled
        int generation { 0 };
        std::string device_code;
        std::chrono::seconds interval { 5 };
        Clock::time_point next_poll;
        Clock::time_point expires;
        Callback<Linked> done;
    };
    LinkState link_;
};

} // namespace adsgames::play
