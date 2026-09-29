# @adsgames/play

Leaderboards, achievements and online multiplayer for A.D.S. Games browser games. No dependencies.

```bash
npm install @adsgames/play
```

## Usage

```ts
import { PlayClient, PlayError } from "@adsgames/play";

const play = new PlayClient({ game: "freecell" });

const session = await play.session(); // { loggedIn, userId?, username? }

try {
  const { improved, best } = await play.submitScore("fastest-win", 93_000, { moves: 104 });
  await play.unlock("first-win");
} catch (err) {
  if (err instanceof PlayError && err.notLoggedIn) {
    // Guest, ask them to log in on adsgames.net
  }
}

const page = await play.leaderboard("fastest-win", { limit: 10 }); // { leaderboard, scores, me }
const achievements = await play.achievements();
```

Every method rejects with a `PlayError` on failure. `status` is the HTTP status, or `0` when play could not be reached. Games should keep working when play is down. For example, GitHub Pages builds have no play server.

### Where requests go

The default base URL is `/api/play`. On adsgames.net, games are served from the site's own origin, so requests are same-origin and send the player's session cookie. The game needs no login code.

| Option | Use |
|---|---|
| `baseUrl` | Another play server, e.g. `http://localhost:8080` |
| `token` | adsgames.net JWT for clients without the cookie, sent as `Authorization: Bearer` |
| `fetch`, `WebSocket` | Replacements, for tests or other runtimes |

For local development, proxy `/api/play` to a local play server. With Vite:

```ts
// vite.config.ts
server: {
  proxy: {
    "/api/play": {
      target: "http://localhost:8080",
      rewrite: (path) => path.replace(/^\/api\/play/, ""),
      ws: true,
    },
  },
},
```

The play server must list the dev origin (for example `http://localhost:5173`) in `ALLOWED_ORIGINS`, or cookie writes get `403`.

## PlayGame: stats, achievements and scores

`PlayGame` wraps `PlayClient` with what every game needs, so games only say what happened. It matches the C++ SDK's `Game`.

```ts
import { PlayGame, PlayError } from "@adsgames/play";

const play = new PlayGame({ game: "mini-jim" });
await play.ready; // play.player: { status: "unknown" | "guest" | "loggedIn" | "offline", username? }

play.on("unlock", (notice) => showBanner(`Achievement unlocked: ${notice.title}`));
play.on("player", (player) => updateMenu(player));

// Gameplay
play.addStat("deaths");                      // A counter
play.addToStatSet("levels-finished", level); // Counts different values
play.unlock("pecked");                       // Saved, sent once, retried

try {
  await play.submitScore("level-1", timeMs, { deaths });
} catch (err) {
  // err.notLoggedIn for guests, without a request
}

play.dispose(); // Stop retries when the game closes
```

- **Stats** are saved per game in `localStorage`, or in the `storage` option you pass. Achievements with a stat rule in the game's `adsgames.json`, e.g. `"stat": "deaths", "atLeast": 100`, unlock on their own once the stat gets there.
- **Unlocks** are saved and sent once the player is logged in, then retried until play confirms them. Guests keep what they earn and get it when they log in. Confirmed unlocks are kept per player.
- **Notices** come with the achievement's title, description, icon and points, also for hidden achievements. Use `on("unlock")`, or `nextUnlock()` from a frame loop.
- **Who is playing** is checked on start, after `link()` or `unlink()`, and every minute while a guest or offline. Call `refresh()` when you know it changed, e.g. after a login in another tab.

`play.client` is the `PlayClient` for everything else, such as leaderboard pages and multiplayer.

## Log in outside the browser

Browser games on adsgames.net use the session cookie. Other clients, such as desktop apps, link the device to the player's account once and stay logged in:

```ts
const play = new PlayClient({
  game: "mini-jim",
  baseUrl: "https://www.adsgames.net/api/play",
  deviceToken: loadSavedToken(), // undefined the first time
  onDeviceToken: (token) => saveToken(token), // null when unlinked or removed
});

const { username } = await play.link({
  deviceName: "Mini Jim on Steam Deck",
  onCode: (code) => showCode(code.userCode, code.verificationUri),
  signal: controller.signal, // optional, to cancel
});

await play.unlink(); // Log out
```

`link()` rejects with a `PlayError` whose message is `access_denied` or `expired_token` when the player denies or waits too long. The client trades the device token for play tokens that last an hour and refreshes them when needed. A device stays logged in until the player removes it on their adsgames.net account page, or it goes unused for 14 days.

The JS client does not store the token itself, since it can run in a browser. Keep it with `onDeviceToken`. The C++ SDK shares its login between games on the same machine, see [../cpp/README.md](../cpp/README.md#log-in-on-desktop).

## Multiplayer

```ts
const room = play.host({ version: 1 }); // or play.join("AB3D", { version: 1 })

room.on("created", ({ code }) => showCode(code));
room.on("peerJoined", ({ name }) => startGame(name));
room.on("message", ({ data }) => applyMove(data));
room.on("peerLeft", () => winByForfeit());
room.on("error", ({ code, message }) => showError(message));

room.send({ kind: "move", from: 12, to: 16 });
room.leave();
```

The relay does not read messages, so each game checks the other player's moves itself. Players with a different `version` can not share a room. See [multiplayer limits](../README.md#-multiplayer-limits).

## Development

```bash
npm ci
npm test        # unit tests, plus server tests when PLAY_URL is set (see ../README.md)
npm run build   # dist/
```

See [../README.md](../README.md#-release) to release.
