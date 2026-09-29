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

| CMake option | Default | Use |
|---|---|---|
| `ADSGAMES_PLAY_NETWORK` | `ON` | `OFF` builds without network code. Requests then fail with status 0 |
| `ADSGAMES_PLAY_URL` | `https://www.adsgames.net/api/play` | Desktop base URL |

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

## Tests

`play_test` runs against a play server and exits 77 (skipped) when it can not reach one. See [../README.md](../README.md#-tests-against-a-local-server) to start one.

```bash
cmake -B build && cmake --build build
PLAY_URL=http://127.0.0.1:8080 PLAY_TOKEN=... ctest --test-dir build --output-on-failure
```
