import { PlayError } from "./errors.ts";
import {
  Multiplayer,
  type MultiplayerOptions,
  type WebSocketConstructor,
} from "./multiplayer.ts";
import type {
  Achievement,
  Leaderboard,
  LeaderboardPage,
  ScoreMeta,
  Session,
  SubmitScoreResult,
  UnlockResult,
  UserAchievements,
} from "./types.ts";

// Same-origin path on adsgames.net, the load balancer routes it to play
export const DEFAULT_BASE_URL = "/api/play";

export interface PlayOptions {
  // adsgames.net game slug, e.g. "freecell"
  game: string;
  // "/api/play" by default, so web builds on adsgames.net send the session cookie
  baseUrl?: string;
  // adsgames.net JWT for clients without the cookie, sent as a bearer token
  token?: string;
  fetch?: typeof fetch;
  WebSocket?: WebSocketConstructor;
}

export interface MultiplayerRoomOptions {
  // Players with a different version can not share a room
  version: number;
}

/**
 * Client for the play service of one game.
 *
 * Every method rejects with a PlayError on failure. Guests get status 401 from
 * submitScore and unlock, so a game can check `error.notLoggedIn` and carry on.
 */
export class PlayClient {
  public readonly game: string;

  public readonly baseUrl: string;

  private readonly token: string | undefined;

  private readonly fetchFn: typeof fetch;

  private readonly webSocket: WebSocketConstructor | undefined;

  public constructor(options: PlayOptions) {
    this.game = options.game;
    this.baseUrl = (options.baseUrl ?? DEFAULT_BASE_URL).replace(/\/+$/u, "");
    this.token = options.token;
    // Browsers need fetch called on globalThis
    this.fetchFn = options.fetch ?? globalThis.fetch.bind(globalThis);
    this.webSocket = options.WebSocket;
  }

  public session(): Promise<Session> {
    return this.request("GET", this.gamePath("session"));
  }

  public async leaderboards(): Promise<Leaderboard[]> {
    const res = await this.request<{ leaderboards: Leaderboard[] }>(
      "GET",
      this.gamePath("leaderboards"),
    );
    return res.leaderboards;
  }

  // Top scores of a board, and the player's own best when logged in
  public leaderboard(
    key: string,
    options: { limit?: number } = {},
  ): Promise<LeaderboardPage> {
    const query = options.limit === undefined ? "" : `?limit=${options.limit}`;
    return this.request(
      "GET",
      `${this.gamePath("leaderboards", key)}${query}`,
    );
  }

  // Keeps the player's best. Values are whole numbers, use milliseconds for times.
  public submitScore(
    key: string,
    value: number,
    meta?: ScoreMeta,
  ): Promise<SubmitScoreResult> {
    if (!Number.isSafeInteger(value)) {
      return Promise.reject(
        new TypeError(`score must be a whole number, got ${value}`),
      );
    }
    return this.request(
      "POST",
      `${this.gamePath("leaderboards", key)}/scores`,
      meta === undefined ? { value } : { value, meta },
    );
  }

  public async achievements(): Promise<Achievement[]> {
    const res = await this.request<{ achievements: Achievement[] }>(
      "GET",
      this.gamePath("achievements"),
    );
    return res.achievements;
  }

  // Safe to call again for an achievement the player already has
  public unlock(key: string): Promise<UnlockResult> {
    return this.request(
      "POST",
      `${this.gamePath("achievements", key)}/unlock`,
    );
  }

  // A player's unlocks in every game
  public userAchievements(userId: string): Promise<UserAchievements> {
    return this.request("GET", `/users/${encodeURIComponent(userId)}/achievements`);
  }

  // WebSocket URL of the multiplayer relay, next to the REST API
  public multiplayerUrl(): string {
    const base = new URL(`${this.baseUrl}/multiplayer`, currentPage());
    base.protocol = base.protocol === "https:" ? "wss:" : "ws:";
    return base.toString();
  }

  // Open a room, share `room.code` with the other player
  public host(options: MultiplayerRoomOptions): Multiplayer {
    return Multiplayer.host(this.roomOptions(options));
  }

  // Join a room by its 4 character code
  public join(code: string, options: MultiplayerRoomOptions): Multiplayer {
    return Multiplayer.join({ ...this.roomOptions(options), code });
  }

  private roomOptions(options: MultiplayerRoomOptions): MultiplayerOptions {
    return {
      url: this.multiplayerUrl(),
      game: this.game,
      version: options.version,
      ...(this.webSocket === undefined ? {} : { WebSocket: this.webSocket }),
    };
  }

  private gamePath(...parts: string[]): string {
    return ["", "games", this.game, ...parts]
      .map((part) => encodeURIComponent(part))
      .join("/");
  }

  private async request<T>(
    method: string,
    path: string,
    body?: unknown,
  ): Promise<T> {
    const headers: Record<string, string> = { Accept: "application/json" };
    if (body !== undefined) {
      headers["Content-Type"] = "application/json";
    }
    if (this.token !== undefined) {
      headers.Authorization = `Bearer ${this.token}`;
    }

    let res: Response;
    try {
      res = await this.fetchFn(`${this.baseUrl}${path}`, {
        method,
        headers,
        ...(body === undefined ? {} : { body: JSON.stringify(body) }),
      });
    } catch (err) {
      throw new PlayError(`could not reach play: ${String(err)}`, 0);
    }

    let data: unknown;
    try {
      data = await res.json();
    } catch {
      // For example the HTML 404 page of a host without play
      throw new PlayError(`play answered ${res.status} without JSON`, res.ok ? 0 : res.status);
    }

    if (!res.ok) {
      const message =
        typeof data === "object" && data !== null && "error" in data
          ? String(data.error)
          : `request failed with ${res.status}`;
      throw new PlayError(message, res.status);
    }

    return data as T;
  }
}

function currentPage(): string | undefined {
  return typeof location === "undefined" ? undefined : location.href;
}
