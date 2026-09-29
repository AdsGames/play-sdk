# adsgames::play (C++)

Leaderboards, achievements and online multiplayer for A.D.S. Games C++ games. It works on desktop and with emscripten, and does not depend on asw.

- **Web:** browser `fetch` and `WebSocket` through emscripten. Requests are same-origin on adsgames.net and send the player's session cookie.
- **Desktop:** IXWebSocket 12 for HTTP and WebSockets. TLS uses Secure Transport on macOS and OpenSSL elsewhere. Desktop builds have no cookie, so players are guests unless the game has a token.

## Add it

```cmake
CPMAddPackage(
  NAME adsgames_play
  GITHUB_REPOSITORY AdsGames/play-sdk
  GIT_TAG v0.1.0
  SOURCE_SUBDIR cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE adsgames::play)
```

It uses `nlohmann_json::nlohmann_json` and `ixwebsocket::ixwebsocket` when the game already has them, and fetches them otherwise.

| CMake option            | Default                             | Use                                                                 |
| ----------------------- | ----------------------------------- | ------------------------------------------------------------------- |
| `ADSGAMES_PLAY_NETWORK` | `ON`                                | `OFF` builds without network code. Requests then fail with status 0 |
| `ADSGAMES_PLAY_URL`     | `https://www.adsgames.net/api/play` | Desktop base URL                                                    |

At runtime, players can point a build at another server. On desktop they set the `ADSGAMES_PLAY_URL` environment variable. On the web they add `?play=` to the page URL.

## Usage

Requests run in the background. Their callbacks run from `update()` on the game thread, so call it once per frame.

```cpp
#include <adsgames/play/client.h>

adsgames::play::Client play({.game = "mini-jim"});

play.session([](const auto& result) {
  if (result.ok() && result->logged_in) {
    username = result->username;
  }
});

play.submit_score("level-1", time_ms, {{"deaths", deaths}}, [](const auto& result) {
  if (result.not_logged_in()) {
    // Guest, show "log in on adsgames.net to save scores"
  } else if (result.ok() && result->improved) {
    // New best, rank result->best.rank
  }
});

play.unlock("no-deaths", nullptr);  // Callbacks can be empty

// Every frame
play.update();
```

A `Result<T>` has `ok()`, `value()` (or `->`), `status()` (HTTP status, 0 when play could not be reached) and `error()`.

## Game: stats, achievements and scores

`Game` wraps `Client` with what every game needs, so games only say what happened:

```cpp
#include <adsgames/play/game.h>

adsgames::play::Game play({.game = "mini-jim"});

// Every frame
play.update();
while (auto notice = play.next_unlock()) {
  show_banner("Achievement unlocked: " + notice->title);
}

// Gameplay
play.add_stat("deaths");                        // A counter
play.add_to_stat_set("levels-finished", level);  // Counts different values
play.unlock("pecked");                          // Saved, sent once, retried

play.submit_score("level-1", time_ms, {{"deaths", deaths}}, [](const auto& result) {
  // result.not_logged_in() for guests, without a request
});

// Menus
play.player();  // {status: Unknown, Guest, LoggedIn or Offline, username}
```

- **Stats** are saved on the device, per game. Achievements with a stat rule in the game's `adsgames.json`, e.g. `"stat": "deaths", "atLeast": 100`, unlock on their own once the stat gets there.
- **Unlocks** are saved and sent once the player is logged in, then retried until play confirms them. Guests keep what they earn and get it when they log in. Confirmed unlocks are kept per player, so a second player on the same device can still earn them.
- **Notices** come with the achievement's title, description, icon and points, also for hidden achievements.
- **Who is playing** is checked on start, after a device link or log out in any game, and every minute while a guest or offline.

Saved state lives next to the shared login on desktop, and in `localStorage` on the web. `client()` gives the `Client` for everything else, such as leaderboard pages and multiplayer.

## Log in on desktop

Desktop builds have no site cookie. The player links the device to their adsgames.net account once, and every A.D.S. Games game on it is logged in:

1. The game calls `link()` and shows the code, e.g. `BCDF-GHJK`.
2. The player enters it at `adsgames.net/link` and approves.
3. The client saves a device token in a folder all A.D.S. Games games share. Other games pick it up when they start, or within a few seconds if they are already open.

```cpp
adsgames::play::Client play({.game = "mini-jim"});

if (!play.linked()) {
  // Show a "Link account" button
}

play.link(
    [](const adsgames::play::LinkCode& code) {
      show_link_screen(code.user_code, code.verification_uri);
      SDL_OpenURL(code.verification_uri_complete.c_str());
    },
    [](const auto& result) {
      if (result.ok()) {
        welcome(result->username);
      } else {
        // result.error() is "access_denied", "expired_token", or another failure
      }
    });

play.cancel_link();  // The player closed the link screen
play.unlink();       // Log out, in every game on this machine
```

`link()` names the device with `default_device_name()`, e.g. `Jims-MacBook-Pro (macOS)`. Pass a name first to use another.

The client trades the device token for play tokens that last an hour, and refreshes them before they run out. A device stays logged in until the player removes it on their adsgames.net account page, or it goes unused for 14 days. It then logs out at its next refresh.

The shared login is one file per site:

| OS      | Folder                                                           |
| ------- | ---------------------------------------------------------------- |
| macOS   | `~/Library/Application Support/adsgames/play/`                   |
| Windows | `%APPDATA%\adsgames\play\`                                       |
| Linux   | `$XDG_CONFIG_HOME/adsgames/play/`, or `~/.config/adsgames/play/` |

Set the `ADSGAMES_PLAY_LOGIN_DIR` environment variable to use another folder, e.g. in tests. To keep the token yourself instead, set `shared_login = false`, pass `device_token`, and save it from `on_device_token` (`""` means logged out). Web builds use the site's cookie and ignore all of this.

Linking uses the site in front of the play URL, e.g. `https://www.adsgames.net` for `https://www.adsgames.net/api/play`. Set `site_url` or the `ADSGAMES_SITE_URL` CMake option to change it.

## Multiplayer

```cpp
auto room = play.host(PROTOCOL_VERSION);  // or play.join(code, PROTOCOL_VERSION)

// Every frame
room->update();
if (room->state() == Multiplayer::State::Waiting) show_code(room->code());
while (auto message = room->receive()) apply_move(*message);
if (room->peer_left()) win_by_forfeit();
if (room->state() == Multiplayer::State::Closed) show_error(room->error());

room->send({{"kind", "move"}, {"from", 12}, {"to", 16}});
room->leave();
```

The relay does not read messages, so each game checks the other player's moves itself.

## Build

`CMakePresets.json` has a preset for each build. Run them from `cpp/`:

```bash
cmake --preset debug && cmake --build --preset debug
```

| Preset    | Build                                                           | Folder          |
| --------- | --------------------------------------------------------------- | --------------- |
| `debug`   | Debug, with tests                                               | `build/debug`   |
| `release` | Release                                                         | `build/release` |
| `tidy`    | Debug, checked by clang-tidy. Rebuilds every file, so each run checks all of them. Needs `clang-tidy` on the `PATH`. | `build/tidy`    |
| `web`     | Emscripten. Needs `EMSDK` set, e.g. by `source emsdk_env.sh`.   | `build/web`     |

Each build writes `compile_commands.json` for editors and `clang-tidy -p build/debug`.

## Tests

`play_test` runs against a play server and exits 77 (skipped) when it can not reach one. See [../README.md](../README.md#-tests-against-a-local-server) to start one.

```bash
cmake --preset debug && cmake --build --preset debug
PLAY_URL=http://127.0.0.1:8080 PLAY_TOKEN=... ctest --preset debug
```
