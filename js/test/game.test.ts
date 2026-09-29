import assert from "node:assert/strict";
import { describe, it, mock } from "node:test";

import { PlayGame, type PlayStorage, type UnlockNotice } from "../src/index.ts";

type Handler = (auth: string | undefined) => [number, unknown];

// Fake play that answers by method and path, and counts calls
function fakePlay(routes: Record<string, Handler>) {
  const calls: string[] = [];
  const fetch = ((input: string | URL | Request, init?: RequestInit) => {
    const route = `${init?.method ?? "GET"} ${new URL(String(input), "http://site.test").pathname}`;
    calls.push(route);
    const handler = routes[route];
    if (!handler) {
      return Promise.resolve(new Response(JSON.stringify({ error: "not found" }), { status: 404 }));
    }
    const headers = (init?.headers ?? {}) as Record<string, string>;
    const [status, body] = handler(headers.Authorization);
    return Promise.resolve(new Response(JSON.stringify(body), { status }));
  }) as typeof globalThis.fetch;
  return { calls, fetch };
}

function memoryStorage(): PlayStorage & { data: Map<string, string> } {
  const data = new Map<string, string>();
  return {
    data,
    getItem: (key) => data.get(key) ?? null,
    setItem: (key, value) => {
      data.set(key, value);
    },
  };
}

// Let queued promises settle
const settle = () => new Promise((resolve) => setTimeout(resolve, 10));

// Let queued promises settle without timers, for mocked timers
const flush = async () => {
  for (let i = 0; i < 20; i++) {
    await new Promise((resolve) => setImmediate(resolve));
  }
};

const G = "/api/play/games/mini-jim";
const rules = () =>
  [200, { rules: [{ key: "persistent", stat: "deaths", atLeast: 3 }] }] as [number, unknown];
const achievements = () =>
  [
    200,
    {
      achievements: [
        {
          key: "persistent",
          title: "Persistent",
          description: "Die 3 times",
          hidden: false,
          points: 10,
          unlockCount: 1,
          unlockedAt: "now",
        },
        {
          key: "pecked",
          title: "Pecked",
          description: "Get pecked",
          hidden: true,
          points: 5,
          unlockCount: 1,
          unlockedAt: "now",
        },
      ],
    },
  ] as [number, unknown];

describe("PlayGame", () => {
  it("keeps guest progress and unlocks it once logged in", async () => {
    const storage = memoryStorage();

    const guestPlay = fakePlay({
      [`GET ${G}/session`]: () => [200, { loggedIn: false }],
    });
    const guest = new PlayGame({
      game: "mini-jim",
      fetch: guestPlay.fetch,
      storage,
    });
    await guest.ready;
    assert.equal(guest.player.status, "guest");

    guest.addStat("deaths", 3);
    guest.unlock("pecked");
    await settle();
    assert.equal(guest.stat("deaths"), 3);
    assert.equal(
      guestPlay.calls.filter((c) => c.startsWith("POST")).length,
      0,
      "guests send nothing",
    );

    await assert.rejects(guest.submitScore("level-1", 1000), { status: 401 });
    assert.equal(
      guestPlay.calls.some((c) => c.includes("/scores")),
      false,
      "no score request for guests",
    );
    guest.dispose();

    // Next start, same device, now logged in
    const unlocks: string[] = [];
    const playerPlay = fakePlay({
      [`GET ${G}/session`]: () => [200, { loggedIn: true, userId: "u1", username: "jim" }],
      [`GET ${G}/achievements/rules`]: rules,
      [`GET ${G}/achievements`]: achievements,
      [`POST ${G}/achievements/pecked/unlock`]: () => (
        unlocks.push("pecked"),
        [201, { newlyUnlocked: true, unlockedAt: "now" }]
      ),
      [`POST ${G}/achievements/persistent/unlock`]: () => (
        unlocks.push("persistent"),
        [201, { newlyUnlocked: true, unlockedAt: "now" }]
      ),
    });
    const player = new PlayGame({
      game: "mini-jim",
      fetch: playerPlay.fetch,
      storage,
    });
    const shown: UnlockNotice[] = [];
    player.on("unlock", (notice) => shown.push(notice));
    await player.ready;
    await settle();

    assert.equal(player.player.username, "jim");
    assert.deepEqual(
      unlocks.toSorted(),
      ["pecked", "persistent"],
      "saved unlock and stat rule are sent",
    );
    assert.ok(player.unlocked("pecked") && player.unlocked("persistent"));
    assert.deepEqual(
      shown.map((n) => n.title).toSorted(),
      ["Pecked", "Persistent"],
      "notices have titles",
    );
    assert.equal(player.nextUnlock()?.key !== undefined, true, "notices can also be polled");

    // Confirmed, so never sent again
    player.unlock("pecked");
    player.addStat("deaths");
    await settle();
    assert.equal(unlocks.length, 2);
    player.dispose();
  });

  it("retries an unlock after a 401 when still logged in", async () => {
    let tries = 0;
    const { fetch } = fakePlay({
      [`GET ${G}/session`]: () => [200, { loggedIn: true, userId: "u1", username: "jim" }],
      [`GET ${G}/achievements/rules`]: () => [200, { rules: [] }],
      [`GET ${G}/achievements`]: achievements,
      [`POST ${G}/achievements/pecked/unlock`]: () =>
        ++tries === 1
          ? [401, { error: "invalid token" }]
          : [201, { newlyUnlocked: true, unlockedAt: "now" }],
    });
    const game = new PlayGame({
      game: "mini-jim",
      fetch,
      storage: memoryStorage(),
    });
    await game.ready;

    mock.timers.enable({ apis: ["setTimeout"] });
    try {
      game.unlock("pecked");
      await flush();
      assert.equal(tries, 1);
      assert.equal(game.unlocked("pecked"), false);

      mock.timers.tick(30_000);
      await flush();
      assert.equal(tries, 2);
      assert.equal(game.unlocked("pecked"), true);
    } finally {
      game.dispose();
      mock.timers.reset();
    }
  });

  it("drops unlocks play does not know", async () => {
    const storage = memoryStorage();
    const { calls, fetch } = fakePlay({
      [`GET ${G}/session`]: () => [200, { loggedIn: true, userId: "u1", username: "jim" }],
      [`GET ${G}/achievements/rules`]: () => [200, { rules: [] }],
    });
    const game = new PlayGame({ game: "mini-jim", fetch, storage });
    await game.ready;

    game.unlock("no-such-key");
    await settle();
    assert.equal(calls.filter((c) => c.includes("no-such-key")).length, 1);
    assert.equal(
      JSON.parse(storage.data.values().next().value ?? "{}").pending.length,
      0,
      "not kept for retry",
    );
    game.dispose();
  });

  it("is offline when play can not be reached", async () => {
    const fetch = (() => Promise.reject(new TypeError("network down"))) as typeof globalThis.fetch;
    const game = new PlayGame({
      game: "mini-jim",
      fetch,
      storage: memoryStorage(),
    });
    await game.ready;
    assert.equal(game.player.status, "offline");
    await assert.rejects(game.submitScore("level-1", 1000), { status: 0 });
    game.dispose();
  });

  it("counts different values in a stat set", () => {
    const game = new PlayGame({
      game: "mini-jim",
      fetch: fakePlay({}).fetch,
      storage: memoryStorage(),
    });
    game.addToStatSet("levels-finished", 0);
    game.addToStatSet("levels-finished", 0);
    game.addToStatSet("levels-finished", 3);
    assert.equal(game.stat("levels-finished"), 2);
    game.dispose();
  });
});
