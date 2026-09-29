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
