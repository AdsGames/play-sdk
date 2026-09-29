# play SDKs

Leaderboards, achievements and online multiplayer for A.D.S. Games games. Games use these clients instead of calling the play API directly.

| SDK | Use in |
|---|---|
| [js](js) | Browser games (Phaser, plain TypeScript), package `@adsgames/play` |
| [cpp](cpp) | C++ games (asw), desktop and emscripten, CMake target `adsgames::play` |

Both cover the same API: session, leaderboards, scores, achievements and the multiplayer relay. Change both SDKs in the same pull request.

## 🎮 Multiplayer limits

The relay pairs two players with a 4 character room code and forwards messages between them. It does not read game messages, so each game checks the other player's moves itself.

- Messages are JSON of at most 4 KB.
- Players with a different `game` or `version` can not share a room.
- Rooms nobody joins close after 30 minutes. When a player leaves, the room closes and its code can not be used again.
- 20 messages per second per connection, with bursts up to 40.
- Too many failed joins from one address get `rate_limited` for the rest of the minute.
- A deploy of play disconnects running games, and games longer than an hour disconnect. Games should handle a closed room.

## 🧪 Tests against a local server

The unit tests run without a server. The server tests run when `PLAY_URL` is set, and CI runs them on every pull request from this repo.

The play server is not public. Team members start one from its repo, then:

```bash
# Test game the SDK tests use
curl -X PUT -H "X-Api-Key: $API_KEY" localhost:8080/games/sdk-test/manifest -d @scripts/sdk-test.json

export PLAY_URL=http://127.0.0.1:8080
export PLAY_TOKEN=$(JWT_SECRET=<local server secret> python3 scripts/dev-token.py)

(cd js && npm ci && npm test)
(cd cpp && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure)
```

`dev-token.py` signs a token like adsgames.net does. Use it as the `access-token` cookie to test a web build as a logged in player.

## 🚀 Release

Both SDKs share one version.

1. Set the same version in `js/package.json` and `project(... VERSION)` in `cpp/CMakeLists.txt`.
2. Push a tag `v<version>`, for example `v0.2.0`.

The release workflow checks both versions against the tag, runs the tests, publishes `@adsgames/play` to npm and creates a GitHub release. C++ games use the tag in `CPMAddPackage`.
