import assert from "node:assert/strict";
import { describe, it } from "node:test";

import { PlayClient, PlayError, defaultSiteUrl } from "../src/index.ts";

type Handler = (body: Record<string, unknown>, auth: string | undefined) => [number, unknown];

// Fake fetch that answers by path and records each call
function router(routes: Record<string, Handler>) {
  const calls: {
    path: string;
    auth: string | undefined;
    body: Record<string, unknown>;
  }[] = [];
  const fn = (input: string | URL | Request, init?: RequestInit): Promise<Response> => {
    const url = new URL(String(input), "http://site.test");
    const headers = (init?.headers ?? {}) as Record<string, string>;
    const body =
      typeof init?.body === "string" ? (JSON.parse(init.body) as Record<string, unknown>) : {};
    calls.push({ path: url.pathname, auth: headers.Authorization, body });
    const handler = routes[url.pathname];
    if (!handler) {
      return Promise.resolve(new Response(JSON.stringify({ error: "not found" }), { status: 404 }));
    }
    const [status, res] = handler(body, headers.Authorization);
    return Promise.resolve(
      status === 204
        ? new Response(null, { status })
        : new Response(JSON.stringify(res), { status }),
    );
  };
  return { calls, fetch: fn as typeof fetch };
}

const player = { userId: "u1", username: "jim" };

describe("defaultSiteUrl", () => {
  it("takes the site in front of /api/play", () => {
    assert.equal(defaultSiteUrl("/api/play"), "");
    assert.equal(
      defaultSiteUrl("https://adsgames.sites.foxd.dev/api/play"),
      "https://adsgames.sites.foxd.dev",
    );
    assert.equal(defaultSiteUrl("http://127.0.0.1:8080"), "https://www.adsgames.net");
  });
});

describe("device login", () => {
  it("links, saves the device token and sends play tokens", async () => {
    let polls = 0;
    const { calls, fetch } = router({
      "/api/device/start": (body) => {
        assert.deepEqual(body, { game: "mini-jim", name: "Test device" });
        return [
          200,
          {
            deviceCode: "dc",
            userCode: "BCDF-GHJK",
            verificationUri: "https://x/link",
            verificationUriComplete: "https://x/link?code=BCDF-GHJK",
            expiresIn: 600,
            interval: 1,
          },
        ];
      },
      "/api/device/token": (body) => {
        assert.equal(body.deviceCode, "dc");
        polls++;
        return polls < 2
          ? [400, { error: "authorization_pending" }]
          : [
              200,
              {
                deviceToken: "device-1",
                accessToken: "access-1",
                expiresIn: 3600,
                ...player,
              },
            ];
      },
      "/api/play/games/mini-jim/session": (_body, auth) => [
        200,
        { loggedIn: auth === "Bearer access-1", ...player },
      ],
    });

    const saved: (string | null)[] = [];
    const play = new PlayClient({
      game: "mini-jim",
      fetch,
      onDeviceToken: (t) => saved.push(t),
    });

    let shown = "";
    const linked = await play.link({
      deviceName: "Test device",
      onCode: (code) => (shown = code.userCode),
    });

    assert.equal(shown, "BCDF-GHJK");
    assert.deepEqual(linked, player);
    assert.equal(polls, 2);
    assert.deepEqual(saved, ["device-1"]);
    assert.equal(play.linked, true);

    const session = await play.session();
    assert.equal(session.loggedIn, true);
    // No refresh needed, the link gave a play token
    assert.equal(calls.filter((c) => c.path === "/api/device/refresh").length, 0);
  });

  it("refreshes a saved device token once for requests at the same time", async () => {
    let refreshes = 0;
    const { calls, fetch } = router({
      "/api/device/refresh": (body) => {
        assert.equal(body.deviceToken, "saved");
        refreshes++;
        return [200, { accessToken: "access-2", expiresIn: 3600, ...player }];
      },
      "/api/play/games/mini-jim/session": () => [200, { loggedIn: true }],
      "/api/play/games/mini-jim/leaderboards": () => [200, { leaderboards: [] }],
    });

    const play = new PlayClient({
      game: "mini-jim",
      fetch,
      deviceToken: "saved",
    });
    await Promise.all([play.session(), play.leaderboards()]);

    assert.equal(refreshes, 1);
    const playCalls = calls.filter((c) => c.path.startsWith("/api/play/"));
    assert.deepEqual(
      playCalls.map((c) => c.auth),
      ["Bearer access-2", "Bearer access-2"],
    );
  });

  it("forgets a device the player removed on the site", async () => {
    const { calls, fetch } = router({
      "/api/device/refresh": () => [401, { error: "invalid_token" }],
      "/api/play/games/mini-jim/session": () => [200, { loggedIn: false }],
    });

    const saved: (string | null)[] = [];
    const play = new PlayClient({
      game: "mini-jim",
      fetch,
      deviceToken: "revoked",
      onDeviceToken: (t) => saved.push(t),
    });

    assert.deepEqual(await play.session(), { loggedIn: false });
    assert.deepEqual(saved, [null]);
    assert.equal(play.linked, false);
    assert.equal(calls.at(-1)?.auth, undefined);
  });

  it("unlinks on the site", async () => {
    const { calls, fetch } = router({
      "/api/device/unlink": () => [204, null],
    });
    const saved: (string | null)[] = [];
    const play = new PlayClient({
      game: "mini-jim",
      fetch,
      deviceToken: "device-1",
      onDeviceToken: (t) => saved.push(t),
    });

    await play.unlink();

    assert.deepEqual(saved, [null]);
    assert.equal(play.linked, false);
    assert.deepEqual(calls[0]?.body, { deviceToken: "device-1" });
  });

  it("ignores a refresh that finishes after unlink", async () => {
    // oxlint-disable-next-line unicorn/consistent-function-scoping
    let release = () => {};
    const gate = new Promise<void>((resolve) => {
      release = resolve;
    });
    const { calls, fetch } = router({
      "/api/device/unlink": () => [204, null],
      "/api/play/games/mini-jim/session": (_body, auth) => [200, { loggedIn: auth !== undefined }],
    });
    // Holds the refresh answer until the device is unlinked
    const slowFetch = (async (input: string | URL | Request, init?: RequestInit) => {
      if (new URL(String(input), "http://site.test").pathname === "/api/device/refresh") {
        await gate;
        return new Response(JSON.stringify({ accessToken: "late", expiresIn: 3600, ...player }), {
          status: 200,
        });
      }
      return fetch(input, init);
    }) as typeof globalThis.fetch;
    const play = new PlayClient({
      game: "mini-jim",
      fetch: slowFetch,
      deviceToken: "device-1",
    });

    const first = play.session();
    await play.unlink();
    release();

    assert.deepEqual(await first, { loggedIn: false });
    assert.deepEqual(await play.session(), { loggedIn: false });
    assert.equal(
      calls.filter((c) => c.path.startsWith("/api/play/")).every((c) => c.auth === undefined),
      true,
    );
  });

  it("rejects when the player denies", async () => {
    const { fetch } = router({
      "/api/device/start": () => [
        200,
        {
          deviceCode: "dc",
          userCode: "C",
          verificationUri: "",
          verificationUriComplete: "",
          expiresIn: 600,
          interval: 1,
        },
      ],
      "/api/device/token": () => [400, { error: "access_denied" }],
    });
    const play = new PlayClient({ game: "mini-jim", fetch });

    await assert.rejects(
      play.link({ deviceName: "d", onCode: () => undefined }),
      (err: unknown) =>
        err instanceof PlayError && err.message === "access_denied" && err.status === 400,
    );
  });

  it("stops when aborted", async () => {
    const { fetch } = router({
      "/api/device/start": () => [
        200,
        {
          deviceCode: "dc",
          userCode: "C",
          verificationUri: "",
          verificationUriComplete: "",
          expiresIn: 600,
          interval: 5,
        },
      ],
    });
    const play = new PlayClient({ game: "mini-jim", fetch });
    const controller = new AbortController();

    const pending = play.link({
      deviceName: "d",
      onCode: () => controller.abort(new Error("cancelled")),
      signal: controller.signal,
    });
    await assert.rejects(pending, /cancelled/u);
  });
});
