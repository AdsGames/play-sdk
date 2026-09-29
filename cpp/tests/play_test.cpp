// End to end test of the SDK against a running play server.
// Exits with 77 (skipped) when the server can not be reached, see README.md.
//
//   PLAY_URL=http://127.0.0.1:8080 PLAY_TOKEN=<jwt> ./play_test
//
// Scores and unlocks are only tested with PLAY_TOKEN. They need the game
// PLAY_GAME (default sdk-test) to have a board "high-score" and an
// achievement "first-win".

#include <adsgames/play/client.h>
#include <adsgames/play/game.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <thread>
#include <utility>

using adsgames::play::Client;
using adsgames::play::Game;
using adsgames::play::Multiplayer;
using adsgames::play::PlayerStatus;
using adsgames::play::Result;

namespace {

constexpr int SKIP = 77;

std::string env(const char* name, const std::string& fallback)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

// Update until the condition holds or time runs out
bool pump(const std::function<void()>& update, const std::function<bool()>& done,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        update();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Runs one request to completion
template <typename T>
std::optional<Result<T>> wait(Client& client, const std::function<void(Client::Callback<T>)>& start)
{
    std::optional<Result<T>> result;
    start([&result](const Result<T>& r) { result = r; });
    pump([&client] { client.update(); }, [&result] { return result.has_value(); });
    return result;
}

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (!ok) {
        failures += 1;
    }
}

void test_multiplayer(const Client& client)
{
    using State = Multiplayer::State;

    auto host = client.host(1);
    pump([&] { host->update(); }, [&] { return host->state() != State::Connecting; });
    check(host->state() == State::Waiting && host->code().size() == 4,
        "host opens a room: " + host->code() + host->error());
    if (host->state() != State::Waiting) {
        return;
    }

    // Lower case, codes are case insensitive
    std::string code = host->code();
    for (auto& c : code) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    auto guest = client.join(code, 1);
    pump(
        [&] {
            host->update();
            guest->update();
        },
        [&] { return host->state() == State::Paired && guest->state() == State::Paired; });
    check(guest->state() == State::Paired && guest->seat() == 1, "guest joins");

    guest->send({ { "move", "a3-b4" } });
    std::optional<nlohmann::json> received;
    pump(
        [&] {
            host->update();
            guest->update();
            received = host->receive();
        },
        [&] { return received.has_value(); });
    check(received && (*received)["move"] == "a3-b4", "message reaches host");

    guest->leave();
    pump([&] { host->update(); }, [&] { return host->peer_left(); });
    check(host->peer_left() && host->state() == State::Closed, "host sees guest leave");

    auto wrong = client.join(code, 2);
    pump([&] { wrong->update(); }, [&] { return wrong->state() == State::Closed; });
    check(wrong->state() == State::Closed && !wrong->error_code().empty(),
        "join of a closed room fails: " + wrong->error_code());
}

// Stats, the unlock queue and stat rules. Needs "ten-wins" to unlock at
// wins >= 10, see scripts/sdk-test.json.
void test_game(const std::string& url, const std::string& game, const std::string& token)
{
    {
        Game guest({ .game = game, .base_url = url });
        pump([&] { guest.update(); },
            [&] { return guest.player().status != PlayerStatus::Unknown; });
        check(guest.player().status == PlayerStatus::Guest, "game sees a guest");

        guest.add_stat("wins", 10);
        pump([&] { guest.update(); }, [] { return false; }, std::chrono::milliseconds(500));
        check(guest.stat("wins") == 10 && !guest.unlocked("ten-wins"),
            "guest keeps stats, unlock waits");

        std::optional<int> status;
        guest.submit_score("high-score", 10, nullptr, [&](const auto& r) { status = r.status(); });
        pump([&] { guest.update(); }, [&] { return status.has_value(); });
        check(status == 401, "guest score is refused without a request");
    }

    if (token.empty()) {
        return;
    }

    // Same saved state as the guest, now logged in
    Game player({ .game = game, .base_url = url, .token = token });
    pump([&] { player.update(); }, [&] { return player.unlocked("ten-wins"); },
        std::chrono::milliseconds(10000));
    check(player.player().status == PlayerStatus::LoggedIn && player.unlocked("ten-wins"),
        "guest progress unlocks once logged in");

    player.add_to_stat_set("levels", 1);
    player.add_to_stat_set("levels", 1);
    player.add_to_stat_set("levels", 2);
    check(player.stat("levels") == 2, "stat sets count different values");
}

} // namespace

int main()
{
    // Keep the shared login and game state away from the real ones
    const auto state_dir = std::filesystem::temp_directory_path() / "adsgames-play-test";
    std::filesystem::remove_all(state_dir);
#if defined(_WIN32)
    _putenv_s("ADSGAMES_PLAY_LOGIN_DIR", state_dir.string().c_str());
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    setenv("ADSGAMES_PLAY_LOGIN_DIR", state_dir.string().c_str(), 1);
#endif
    const std::string url = env("PLAY_URL", "http://127.0.0.1:8080");
    const std::string game = env("PLAY_GAME", "sdk-test");
    const std::string token = env("PLAY_TOKEN", "");

    using adsgames::play::Session;
    using adsgames::play::SubmitScoreResult;

    Client guest({ .game = game, .base_url = url });
    auto session = wait<Session>(guest, [&](auto done) { guest.session(std::move(done)); });
    if (!session || session->status() == 0) {
        std::cerr << "SKIP play is not reachable at " << url
                  << (session ? ": " + session->error() : ": timed out") << "\n";
        return SKIP;
    }
    check(session->ok() && !session->value().logged_in, "guest session");

    auto guest_score = wait<SubmitScoreResult>(
        guest, [&](auto done) { guest.submit_score("high-score", 10, nullptr, std::move(done)); });
    check(guest_score && guest_score->not_logged_in(), "guest score needs a login");

    if (token.empty()) {
        std::cout << "skip scores and unlocks, PLAY_TOKEN is not set\n";
    } else {
        using adsgames::play::Achievement;
        using adsgames::play::Leaderboard;
        using adsgames::play::LeaderboardPage;
        using adsgames::play::UnlockResult;

        Client player({ .game = game, .base_url = url, .token = token });
        auto me = wait<Session>(player, [&](auto done) { player.session(std::move(done)); });
        check(me && me->ok() && me->value().logged_in,
            "player session: " + (me ? me->value().username : ""));

        auto boards = wait<std::vector<Leaderboard>>(
            player, [&](auto done) { player.leaderboards(std::move(done)); });
        check(boards && boards->ok() && !boards->value().empty(), "list leaderboards");

        auto score = wait<SubmitScoreResult>(player, [&](auto done) {
            player.submit_score("high-score", 4200, { { "version", "test" } }, std::move(done));
        });
        check(score && score->ok() && score->value().best.value >= 4200,
            "submit score" + (score && !score->ok() ? ": " + score->error() : ""));

        auto page = wait<LeaderboardPage>(
            player, [&](auto done) { player.leaderboard("high-score", 5, std::move(done)); });
        check(page && page->ok() && page->value().me.has_value(), "leaderboard has my score");

        auto missing = wait<LeaderboardPage>(
            player, [&](auto done) { player.leaderboard("nope", 5, std::move(done)); });
        check(missing && missing->status() == 404, "missing board is 404");

        auto unlock = wait<UnlockResult>(
            player, [&](auto done) { player.unlock("first-win", std::move(done)); });
        check(unlock && unlock->ok() && !unlock->value().unlocked_at.empty()
                && (unlock->status() == 200 || unlock->status() == 201),
            "unlock achievement");

        auto list = wait<std::vector<Achievement>>(
            player, [&](auto done) { player.achievements(std::move(done)); });
        check(list && list->ok() && !list->value().empty() && !list->value()[0].unlocked_at.empty(),
            "achievement shows unlocked");

        // Fire and forget still completes
        player.unlock("first-win", nullptr);
        pump([&] { player.update(); }, [&] { return player.pending() == 0; });
        check(player.pending() == 0, "request without callback finishes");
    }

    test_multiplayer(guest);
    test_game(url, game, token);
    std::filesystem::remove_all(state_dir);

    std::cout << (failures == 0 ? "PASS" : "FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
