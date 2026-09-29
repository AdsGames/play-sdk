/**
 * Everything a game needs from play, on top of Client
 *
 * Game keeps the parts every game would otherwise write itself:
 *
 * - Who is playing: logged in, a guest, or play can not be reached.
 * - Stats saved on the device, e.g. deaths or levels finished. Achievements
 *   with a stat rule in the game's adsgames.json unlock on their own.
 * - A queue of unlocks. They are saved, sent once the player is logged in,
 *   and retried until play confirms them, so guests keep what they earn.
 * - Notices for newly unlocked achievements, with their titles, to show.
 *
 * The game still decides when things happen and how they look. Like Client,
 * callbacks run on the game thread from update(), so call it once per frame.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "client.h"

namespace adsgames::play {

enum class PlayerStatus : uint8_t {
    // play has not answered yet
    Unknown,
    // Not logged in. Unlocks wait until they are.
    Guest,
    LoggedIn,
    // play could not be reached, e.g. a build not hosted on adsgames.net
    Offline,
};

struct Player {
    PlayerStatus status { PlayerStatus::Unknown };
    std::string user_id;
    std::string username;
};

// A newly unlocked achievement, for the game to show
struct UnlockNotice {
    std::string key;
    std::string title;
    std::string description;
    std::string icon;
    int points { 0 };
};

class Game {
public:
    explicit Game(ClientConfig config);
    ~Game();

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    Game(Game&&) = delete;
    Game& operator=(Game&&) = delete;

    // Run callbacks, send waiting unlocks and retry failures. Call once per
    // frame.
    void update();

    const Player& player() const
    {
        return player_;
    }

    // --- Stats, saved on this device ---

    // A counter or the size of a set, 0 if the game never set it
    int64_t stat(const std::string& name) const;

    // Add to a counter, e.g. add_stat("deaths")
    void add_stat(const std::string& name, int64_t amount = 1);

    // Add a value to a set, the stat is how many different values it has.
    // e.g. add_to_stat_set("levels-finished", level)
    void add_to_stat_set(const std::string& name, int64_t value);

    // --- Achievements ---

    // Unlock an achievement. Safe to call again, it is sent once. Saved and
    // sent once the player is logged in.
    void unlock(const std::string& key);

    // True once play confirmed the achievement for the logged in player
    bool unlocked(const std::string& key) const;

    // The next newly unlocked achievement to show, if any
    std::optional<UnlockNotice> next_unlock();

    // --- Leaderboards ---

    // Keeps the player's best. Guests get not_logged_in() without a request.
    void submit_score(const std::string& key, int64_t value, const nlohmann::json& meta,
        Client::Callback<SubmitScoreResult> done);

    // --- Device login, see Client::link() ---

    void link(std::function<void(const LinkCode&)> on_code, Client::Callback<Linked> done);
    void cancel_link()
    {
        client_->cancel_link();
    }
    bool linking() const
    {
        return client_->linking();
    }
    void unlink();

    // Everything else, e.g. leaderboard pages and multiplayer
    Client& client()
    {
        return *client_;
    }

private:
    using Clock = std::chrono::steady_clock;

    // Ask play who is playing
    void check_session();

    // Ask again soon, e.g. after a log in or out
    void recheck_session();

    void load();
    void save() const;

    // Unlock what the stat rules say, when the rules are known
    void apply_rules();

    // Send waiting unlocks when logged in
    void send_unlocks();

    // Turn newly unlocked keys into notices, with titles from play
    void fetch_notices();

    ClientConfig config_;
    std::unique_ptr<Client> client_;
    std::string store_path_;

    Player player_;
    bool session_wanted_ { true };
    bool session_in_flight_ { false };
    Clock::time_point next_session_;

    std::map<std::string, int64_t> counters_;
    std::map<std::string, std::set<int64_t>> sets_;

    std::vector<StatRule> rules_;
    bool rules_known_ { false };
    bool rules_in_flight_ { false };
    Clock::time_point next_rules_;

    // Waiting to send, kept until play confirms
    std::set<std::string> pending_;
    std::set<std::string> sending_;
    Clock::time_point next_send_;

    // Confirmed unlocks by user id
    std::map<std::string, std::set<std::string>> confirmed_;

    std::vector<std::string> notice_keys_;
    bool notices_in_flight_ { false };
    std::deque<UnlockNotice> notices_;

    // Results that do not need a request, run from update()
    std::vector<std::function<void()>> local_;
};

} // namespace adsgames::play
