import assert from "node:assert/strict";
import { describe, it } from "node:test";

import { Multiplayer, type MultiplayerErrorCode } from "../src/index.ts";

// Fake socket the test drives by hand
class FakeSocket extends EventTarget {
  public static last: FakeSocket | undefined;

  public readonly OPEN = 1;

  public readyState = 0;

  public readonly sent: unknown[] = [];

  public closed = false;

  public readonly url: string;

  public constructor(url: string) {
    super();
    this.url = url;
    FakeSocket.last = this;
  }

  public send(text: string): void {
    this.sent.push(JSON.parse(text));
  }

  public close(): void {
    this.closed = true;
  }

  public open(): void {
    this.readyState = 1;
    this.dispatchEvent(new Event("open"));
  }

  public receive(message: unknown): void {
    this.dispatchEvent(new MessageEvent("message", { data: JSON.stringify(message) }));
  }
}

const options = {
  url: "wss://example/multiplayer",
  game: "checkers",
  version: 2,
  WebSocket: FakeSocket as unknown as new (url: string) => WebSocket,
};

function socket(): FakeSocket {
  assert.ok(FakeSocket.last);
  return FakeSocket.last;
}

describe("Multiplayer", () => {
  it("hosts a room and relays messages once paired", () => {
    const room = Multiplayer.host<{ move: string }>(options);
    const events: string[] = [];
    const received: unknown[] = [];
    room.on("created", ({ code }) => events.push(`created ${code}`));
    room.on("peerJoined", ({ name }) => events.push(`peer ${name}`));
    room.on("message", ({ data }) => received.push(data));

    // Messages before pairing are dropped
    room.send({ move: "early" });
    socket().open();
    assert.deepEqual(socket().sent, [{ type: "create", game: "checkers", version: 2 }]);

    socket().receive({ type: "created", code: "AB3D", seat: 0 });
    assert.equal(room.state, "waiting");
    assert.equal(room.code, "AB3D");

    socket().receive({ type: "peer_joined", seat: 1, name: "bob" });
    assert.equal(room.state, "paired");
    assert.equal(room.peerName, "bob");

    room.send({ move: "a3-b4" });
    assert.deepEqual(socket().sent[1], { type: "send", data: { move: "a3-b4" } });

    socket().receive({ type: "message", from: 1, data: { move: "f6-e5" } });
    assert.deepEqual(received, [{ move: "f6-e5" }]);
    assert.deepEqual(events, ["created AB3D", "peer bob"]);
  });

  it("joins with an upper case code", () => {
    const room = Multiplayer.join({ ...options, code: "ab3d" });
    socket().open();
    assert.deepEqual(socket().sent, [{ type: "join", game: "checkers", version: 2, code: "AB3D" }]);

    socket().receive({ type: "joined", code: "AB3D", seat: 1 });
    assert.equal(room.state, "paired");
    assert.equal(room.seat, 1);
    assert.equal(room.peerName, undefined);
  });

  it("closes when a join fails", () => {
    const room = Multiplayer.join({ ...options, code: "ZZZZ" });
    const errors: MultiplayerErrorCode[] = [];
    room.on("error", ({ code }) => errors.push(code));

    socket().open();
    socket().receive({ type: "error", code: "room_not_found", message: "no room" });
    assert.deepEqual(errors, ["room_not_found"]);
    assert.equal(room.state, "closed");
    assert.equal(socket().closed, true);
  });

  it("closes when the other player leaves", () => {
    const room = Multiplayer.host(options);
    let left = false;
    room.on("peerLeft", () => {
      left = true;
    });

    socket().open();
    socket().receive({ type: "created", code: "AB3D", seat: 0 });
    socket().receive({ type: "peer_joined", seat: 1 });
    socket().receive({ type: "peer_left", seat: 1 });
    assert.equal(left, true);
    assert.equal(room.state, "closed");
  });

  it("ignores bad server messages", () => {
    const room = Multiplayer.host(options);
    socket().open();
    socket().dispatchEvent(new MessageEvent("message", { data: "not json" }));
    socket().receive(null);
    assert.equal(room.state, "connecting");
  });

  it("stops calling a handler after unsubscribe", () => {
    const room = Multiplayer.host(options);
    let calls = 0;
    const off = room.on("created", () => {
      calls += 1;
    });
    off();
    socket().open();
    socket().receive({ type: "created", code: "AB3D", seat: 0 });
    assert.equal(calls, 0);
  });
});
