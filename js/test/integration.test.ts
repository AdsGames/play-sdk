// End to end test against a running play server, skipped without PLAY_URL.
//
//   PLAY_URL=http://127.0.0.1:8080 PLAY_TOKEN=<jwt> npm test
//
// Scores and unlocks need PLAY_TOKEN, and the game PLAY_GAME (default
// sdk-test) with a board "high-score" and an achievement "first-win".

import assert from "node:assert/strict";
import { describe, it } from "node:test";

import { PlayClient, PlayError, type Multiplayer } from "../src/index.ts";

const baseUrl = process.env.PLAY_URL;
const token = process.env.PLAY_TOKEN;
const game = process.env.PLAY_GAME ?? "sdk-test";

function until(room: Multiplayer, check: () => boolean): Promise<void> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      reject(new Error(`timed out in state ${room.state}`));
    }, 5000);
    const poll = (): void => {
      if (check()) {
        clearTimeout(timer);
        resolve();
      } else {
        setTimeout(poll, 5);
      }
    };
    poll();
  });
}

describe("play server", { skip: baseUrl === undefined && "PLAY_URL is not set" }, () => {
  const guest = new PlayClient({ game, baseUrl: baseUrl ?? "" });

  it("answers guests", async () => {
    assert.deepEqual(await guest.session(), { loggedIn: false });
    await assert.rejects(
      guest.submitScore("high-score", 10),
      (err: unknown) => err instanceof PlayError && err.notLoggedIn,
    );
  });

  it("keeps scores and unlocks", { skip: token === undefined && "PLAY_TOKEN is not set" }, async () => {
    const player = new PlayClient({ game, baseUrl: baseUrl ?? "", token });

    assert.equal((await player.session()).loggedIn, true);
    const res = await player.submitScore("high-score", 4300, { version: "test" });
    assert.ok(res.best.value >= 4300);

    const page = await player.leaderboard("high-score", { limit: 5 });
    assert.ok(page.me);

    const unlock = await player.unlock("first-win");
    assert.ok(unlock.unlockedAt);
    const achievements = await player.achievements();
    assert.ok(achievements.some((a) => a.key === "first-win" && a.unlockedAt !== null));
  });

  it("pairs two players", async () => {
    const host = guest.host({ version: 1 });
    await until(host, () => host.state === "waiting");
    assert.equal(host.code?.length, 4);

    const received: unknown[] = [];
    host.on("message", ({ data }) => received.push(data));

    const other = guest.join(host.code?.toLowerCase() ?? "", { version: 1 });
    await until(other, () => other.state === "paired" && host.state === "paired");

    other.send({ move: "a3-b4" });
    await until(host, () => received.length > 0);
    assert.deepEqual(received, [{ move: "a3-b4" }]);

    let left = false;
    host.on("peerLeft", () => {
      left = true;
    });
    other.leave();
    await until(host, () => left);
    assert.equal(host.state, "closed");
  });
});
