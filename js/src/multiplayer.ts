/**
 * Client for the play multiplayer relay. The relay pairs two players in a room
 * named by a 4 character code and forwards messages between them. It does not
 * read game messages, so each game checks the other player's moves itself.
 */

export type WebSocketConstructor = new (url: string) => WebSocket;

export type Seat = 0 | 1;

export type MultiplayerErrorCode =
  | "bad_request"
  | "already_in_room"
  | "not_in_room"
  | "room_not_found"
  | "room_full"
  | "version_mismatch"
  | "room_expired"
  | "server_full"
  | "rate_limited"
  // The socket failed or closed, not a server error
  | "connection";

export type MultiplayerState =
  // Opening the socket
  | "connecting"
  // In a room, waiting for the other player
  | "waiting"
  // Both players are in the room
  | "paired"
  // The room or socket closed, see the error or peerLeft events
  | "closed";

export interface MultiplayerEvents<T> {
  // Room opened, share the code with the other player
  created: { code: string; seat: Seat };
  // Joined a room, the host is already in it
  joined: { code: string; seat: Seat; peerName?: string };
  peerJoined: { seat: Seat; name?: string };
  message: { from: Seat; data: T };
  peerLeft: { seat: Seat };
  error: { code: MultiplayerErrorCode; message: string };
  stateChange: { state: MultiplayerState };
}

export interface MultiplayerOptions {
  // wss://…/api/play/multiplayer
  url: string;
  // adsgames.net game slug
  game: string;
  // Players with a different version can not share a room
  version: number;
  WebSocket?: WebSocketConstructor;
}

type Handler<P> = (payload: P) => void;

type ServerMessage =
  | { type: "created"; code: string; seat: Seat }
  | { type: "joined"; code: string; seat: Seat; peerName?: string }
  | { type: "peer_joined"; seat: Seat; name?: string }
  | { type: "message"; from: Seat; data: unknown }
  | { type: "peer_left"; seat: Seat }
  | { type: "error"; code: MultiplayerErrorCode; message: string };

export class Multiplayer<T = unknown> {
  public state: MultiplayerState = "connecting";

  // Room code, set once the room is created or joined
  public code: string | undefined;

  public seat: Seat | undefined;

  // Other player's adsgames.net username, undefined for guests
  public peerName: string | undefined;

  private readonly socket: WebSocket;

  private readonly handlers = new Map<keyof MultiplayerEvents<T>, Set<Handler<never>>>();

  private constructor(options: MultiplayerOptions, first: object) {
    const Socket = options.WebSocket ?? globalThis.WebSocket;
    this.socket = new Socket(options.url);

    const hello = { game: options.game, version: options.version, ...first };
    this.socket.addEventListener("open", () => {
      this.socket.send(JSON.stringify(hello));
    });
    this.socket.addEventListener("message", (event: MessageEvent) => {
      this.handle(event.data);
    });
    this.socket.addEventListener("close", () => {
      if (this.state !== "closed") {
        this.emit("error", { code: "connection", message: "disconnected from server" });
        this.setState("closed");
      }
    });
    this.socket.addEventListener("error", () => {
      if (this.state !== "closed") {
        this.emit("error", { code: "connection", message: "could not reach server" });
        this.setState("closed");
      }
    });
  }

  // Connect and open a new room
  public static host<T = unknown>(options: MultiplayerOptions): Multiplayer<T> {
    return new Multiplayer<T>(options, { type: "create" });
  }

  // Connect and join the room with the given code, codes are case insensitive
  public static join<T = unknown>(options: MultiplayerOptions & { code: string }): Multiplayer<T> {
    return new Multiplayer<T>(options, { type: "join", code: options.code.toUpperCase() });
  }

  // Listen for an event, returns a function that stops listening
  public on<K extends keyof MultiplayerEvents<T>>(
    event: K,
    handler: Handler<MultiplayerEvents<T>[K]>,
  ): () => void {
    let set = this.handlers.get(event);
    if (set === undefined) {
      set = new Set();
      this.handlers.set(event, set);
    }
    set.add(handler);
    return () => {
      set.delete(handler);
    };
  }

  // Send data to the other player, ignored until paired
  public send(data: T): void {
    if (this.state === "paired") {
      this.socket.send(JSON.stringify({ type: "send", data }));
    }
  }

  // Leave the room, the other player gets peerLeft
  public leave(): void {
    if (this.state === "closed") {
      return;
    }
    this.setState("closed");
    if (this.socket.readyState === this.socket.OPEN) {
      this.socket.send(JSON.stringify({ type: "leave" }));
    }
    this.socket.close(1000, "left");
  }

  private handle(raw: unknown): void {
    // Anything the server sends is untrusted input
    let msg: ServerMessage;
    try {
      msg = JSON.parse(String(raw)) as ServerMessage;
    } catch {
      return;
    }
    if (typeof msg !== "object" || msg === null || this.state === "closed") {
      return;
    }

    switch (msg.type) {
      case "created":
        this.code = msg.code;
        this.seat = msg.seat;
        this.setState("waiting");
        this.emit("created", { code: msg.code, seat: msg.seat });
        break;
      case "joined":
        this.code = msg.code;
        this.seat = msg.seat;
        this.peerName = msg.peerName;
        this.setState("paired");
        this.emit("joined", withName({ code: msg.code, seat: msg.seat }, "peerName", msg.peerName));
        break;
      case "peer_joined":
        this.peerName = msg.name;
        this.setState("paired");
        this.emit("peerJoined", withName({ seat: msg.seat }, "name", msg.name));
        break;
      case "message":
        this.emit("message", { from: msg.from, data: msg.data as T });
        break;
      case "peer_left":
        // The server closes the room when a player leaves
        this.emit("peerLeft", { seat: msg.seat });
        this.leave();
        break;
      case "error":
        this.emit("error", { code: msg.code, message: msg.message });
        // Without a room there is nothing left to do on this socket
        if (this.state === "connecting") {
          this.leave();
        }
        break;
      default:
        break;
    }
  }

  private setState(state: MultiplayerState): void {
    if (this.state !== state) {
      this.state = state;
      this.emit("stateChange", { state });
    }
  }

  private emit<K extends keyof MultiplayerEvents<T>>(
    event: K,
    payload: MultiplayerEvents<T>[K],
  ): void {
    this.handlers.get(event)?.forEach((handler) => {
      (handler as Handler<MultiplayerEvents<T>[K]>)(payload);
    });
  }
}

// Adds an optional name only when the server sent one
function withName<P extends object, K extends string>(
  payload: P,
  key: K,
  name: string | undefined,
): P & { [key in K]?: string } {
  return name === undefined ? payload : { ...payload, [key]: name };
}
