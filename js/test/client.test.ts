import assert from "node:assert/strict";
import { describe, it } from "node:test";

import { PlayClient, PlayError } from "../src/index.ts";

interface Call {
  url: string;
  method: string;
  headers: Record<string, string>;
  body: unknown;
}

// Fake fetch that records requests and answers with a fixed response
function fakeFetch(status: number, body: unknown): { calls: Call[]; fetch: typeof fetch } {
  const calls: Call[] = [];
  const fn = (input: string | URL | Request, init?: RequestInit): Promise<Response> => {
    calls.push({
      url: String(input),
      method: init?.method ?? "GET",
      headers: (init?.headers ?? {}) as Record<string, string>,
      body: typeof init?.body === "string" ? JSON.parse(init.body) : undefined,
    });
    const text = typeof body === "string" ? body : JSON.stringify(body);
    return Promise.resolve(new Response(text, { status }));
  };
  return { calls, fetch: fn as typeof fetch };
}

describe("PlayClient", () => {
  it("defaults to the same-origin /api/play path", async () => {
    const { calls, fetch } = fakeFetch(200, { loggedIn: false });
    const play = new PlayClient({ game: "freecell", fetch });

    assert.deepEqual(await play.session(), { loggedIn: false });
    assert.equal(calls[0]?.url, "/api/play/games/freecell/session");
    assert.equal(calls[0]?.method, "GET");
    assert.equal(calls[0]?.headers.Authorization, undefined);
  });

  it("submits scores with meta and a bearer token", async () => {
    const best = { rank: 1, userId: "u", username: "jim", value: 900, meta: null, updatedAt: "" };
    const { calls, fetch } = fakeFetch(201, { improved: true, best });
    const play = new PlayClient({
      game: "freecell",
      baseUrl: "https://play.example/",
      token: "abc",
      fetch,
    });

    const res = await play.submitScore("fastest-win", 900, { moves: 80 });
    assert.equal(res.improved, true);
    assert.equal(
      calls[0]?.url,
      "https://play.example/games/freecell/leaderboards/fastest-win/scores",
    );
    assert.equal(calls[0]?.method, "POST");
    assert.equal(calls[0]?.headers.Authorization, "Bearer abc");
    assert.deepEqual(calls[0]?.body, { value: 900, meta: { moves: 80 } });
  });

  it("rejects scores that are not whole numbers", async () => {
    const { calls, fetch } = fakeFetch(200, {});
    const play = new PlayClient({ game: "freecell", fetch });

    await assert.rejects(play.submitScore("fastest-win", 1.5), TypeError);
    assert.equal(calls.length, 0);
  });

  it("unwraps list responses", async () => {
    const { calls, fetch } = fakeFetch(200, { achievements: [{ key: "first-win" }] });
    const play = new PlayClient({ game: "freecell", fetch });

    assert.deepEqual(await play.achievements(), [{ key: "first-win" }]);
    assert.equal(calls[0]?.url, "/api/play/games/freecell/achievements");
  });

  it("passes the leaderboard limit", async () => {
    const { calls, fetch } = fakeFetch(200, { leaderboard: {}, scores: [], me: null });
    const play = new PlayClient({ game: "freecell", fetch });

    await play.leaderboard("fewest-moves", { limit: 25 });
    assert.equal(calls[0]?.url, "/api/play/games/freecell/leaderboards/fewest-moves?limit=25");
  });

  it("turns server errors into PlayError", async () => {
    const { fetch } = fakeFetch(401, { error: "not logged in" });
    const play = new PlayClient({ game: "freecell", fetch });

    await assert.rejects(play.unlock("first-win"), (err: unknown) => {
      assert.ok(err instanceof PlayError);
      assert.equal(err.status, 401);
      assert.equal(err.notLoggedIn, true);
      assert.equal(err.message, "not logged in");
      return true;
    });
  });

  it("reports hosts without play", async () => {
    const { fetch } = fakeFetch(404, "<html>Not found</html>");
    const play = new PlayClient({ game: "freecell", fetch });

    await assert.rejects(
      play.session(),
      (err: unknown) => err instanceof PlayError && err.status === 404,
    );
  });

  it("reports network failures with status 0", async () => {
    const fetch = (() => Promise.reject(new Error("offline"))) as typeof globalThis.fetch;
    const play = new PlayClient({ game: "freecell", fetch });

    await assert.rejects(
      play.session(),
      (err: unknown) => err instanceof PlayError && err.status === 0,
    );
  });

  it("builds the multiplayer URL next to the API", () => {
    const play = new PlayClient({ game: "freecell", baseUrl: "https://www.adsgames.net/api/play" });
    assert.equal(play.multiplayerUrl(), "wss://www.adsgames.net/api/play/multiplayer");

    const local = new PlayClient({ game: "freecell", baseUrl: "http://localhost:8080" });
    assert.equal(local.multiplayerUrl(), "ws://localhost:8080/multiplayer");
  });
});
