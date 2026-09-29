import { PlayError } from "./errors.ts";
import { Multiplayer, type MultiplayerOptions, type WebSocketConstructor } from "./multiplayer.ts";
import type {
  Achievement,
  Leaderboard,
  LeaderboardPage,
  LinkCode,
  Linked,
  ScoreMeta,
  StatRule,
  Session,
  SubmitScoreResult,
  UnlockResult,
  UserAchievements,
} from "./types.ts";

// Same-origin path on adsgames.net, the load balancer routes it to play
export const DEFAULT_BASE_URL = "/api/play";

// Where devices link when the play URL is not under a site's /api/play
export const DEFAULT_SITE_URL = "https://www.adsgames.net";

// Refresh play tokens a minute early, so a request never carries an expired one
const TOKEN_MARGIN_MS = 60_000;

export interface PlayOptions {
  // adsgames.net game slug, e.g. "freecell"
  game: string;
  // "/api/play" by default, so web builds on adsgames.net send the session cookie
  baseUrl?: string;
  // adsgames.net JWT for clients without the cookie, sent as a bearer token
  token?: string;
  // Device token saved from an earlier link(). The client trades it for short
  // lived play tokens, so the player stays logged in.
  deviceToken?: string;
  // Called with a new device token after link(), and with null once the device
  // is unlinked or the player removed it on adsgames.net. Save it to keep the
  // player logged in on this device.
  onDeviceToken?: (token: string | null) => void;
  // adsgames.net, where devices link. Defaults to the site in front of
  // baseUrl, e.g. "" for "/api/play", or https://www.adsgames.net.
  siteUrl?: string;
  fetch?: typeof fetch;
  WebSocket?: WebSocketConstructor;
}

export interface LinkOptions {
  // Shown to the player when approving, e.g. "Mini Jim on Steam Deck"
  deviceName: string;
  // Show the code to the player
  onCode: (code: LinkCode) => void;
  // Stops waiting, link() then rejects with the signal's reason
  signal?: AbortSignal;
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

  public readonly siteUrl: string;

  private readonly token: string | undefined;

  private readonly onDeviceToken: PlayOptions["onDeviceToken"];

  private deviceToken: string | undefined;

  private accessToken: string | undefined;

  private accessExpires = 0;

  // One refresh at a time, requests wait for it
  private refreshing: Promise<void> | undefined;

  private readonly fetchFn: typeof fetch;

  private readonly webSocket: WebSocketConstructor | undefined;

  public constructor(options: PlayOptions) {
    this.game = options.game;
    this.baseUrl = (options.baseUrl ?? DEFAULT_BASE_URL).replace(/\/+$/u, "");
    this.token = options.token;
    this.deviceToken = options.deviceToken;
    this.onDeviceToken = options.onDeviceToken;
    this.siteUrl = (options.siteUrl ?? defaultSiteUrl(this.baseUrl)).replace(/\/+$/u, "");
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
  public leaderboard(key: string, options: { limit?: number } = {}): Promise<LeaderboardPage> {
    const query = options.limit === undefined ? "" : `?limit=${options.limit}`;
    return this.request("GET", `${this.gamePath("leaderboards", key)}${query}`);
  }

  // Keeps the player's best. Values are whole numbers, use milliseconds for times.
  public submitScore(key: string, value: number, meta?: ScoreMeta): Promise<SubmitScoreResult> {
    if (!Number.isSafeInteger(value)) {
      return Promise.reject(new TypeError(`score must be a whole number, got ${value}`));
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
    return this.request("POST", `${this.gamePath("achievements", key)}/unlock`);
  }

  // Achievements that unlock from a stat, including hidden ones
  public async achievementRules(): Promise<StatRule[]> {
    const res = await this.request<{ rules: StatRule[] }>(
      "GET",
      `${this.gamePath("achievements")}/rules`,
    );
    return res.rules;
  }

  // A player's unlocks in every game
  public userAchievements(userId: string): Promise<UserAchievements> {
    return this.request("GET", `/users/${encodeURIComponent(userId)}/achievements`);
  }

  // True when the client has a device token
  public get linked(): boolean {
    return this.deviceToken !== undefined;
  }

  /**
   * Log this device in to an adsgames.net account. `onCode` gets a code for
   * the player to enter at `code.verificationUri`. Resolves with the player
   * once they approve, and `onDeviceToken` gets the token to save. Rejects
   * with a PlayError "access_denied" or "expired_token" otherwise.
   */
  public async link(options: LinkOptions): Promise<Linked> {
    const { signal } = options;
    const start = await this.siteRequest<{
      deviceCode: string;
      userCode: string;
      verificationUri: string;
      verificationUriComplete: string;
      expiresIn: number;
      interval: number;
    }>("/api/device/start", { game: this.game, name: options.deviceName });

    options.onCode({
      userCode: start.userCode,
      verificationUri: start.verificationUri,
      verificationUriComplete: start.verificationUriComplete,
      expiresIn: start.expiresIn,
    });

    let intervalMs = Math.max(start.interval, 1) * 1000;
    const expiresIn = start.expiresIn > 0 ? start.expiresIn : 600;
    const expires = Date.now() + expiresIn * 1000;

    for (;;) {
      await sleep(intervalMs, signal);
      if (Date.now() >= expires) {
        throw new PlayError("expired_token", 400);
      }

      try {
        const tokens = await this.siteRequest<DeviceTokens>("/api/device/token", {
          deviceCode: start.deviceCode,
        });
        if (!tokens.deviceToken) {
          throw new PlayError("unexpected response from site", 200);
        }
        this.setAccess(tokens);
        this.setDeviceToken(tokens.deviceToken);
        return { userId: tokens.userId, username: tokens.username };
      } catch (err) {
        if (!(err instanceof PlayError)) {
          throw err;
        }
        // Keep polling while the player has not decided, or the site is down
        if (err.status === 0 || err.message === "authorization_pending") {
          continue;
        }
        if (err.message === "slow_down") {
          intervalMs += 5000;
          continue;
        }
        throw err;
      }
    }
  }

  // Log this device out, and unlink it on adsgames.net
  public async unlink(): Promise<void> {
    const token = this.deviceToken;
    this.accessToken = undefined;
    if (token === undefined) {
      return;
    }
    this.setDeviceToken(null);
    try {
      await this.siteRequest("/api/device/unlink", { deviceToken: token });
    } catch {
      // Already logged out here, the site forgets it when removed there
    }
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
    return ["", "games", this.game, ...parts].map((part) => encodeURIComponent(part)).join("/");
  }

  private setDeviceToken(token: string | null): void {
    this.deviceToken = token ?? undefined;
    this.onDeviceToken?.(token);
  }

  private setAccess(tokens: { accessToken: string; expiresIn: number }): void {
    this.accessToken = tokens.accessToken;
    this.accessExpires = Date.now() + Math.max(tokens.expiresIn * 1000 - TOKEN_MARGIN_MS, 30_000);
  }

  // Trades the device token for a play token
  private refreshAccess(): Promise<void> {
    this.refreshing ??= (async () => {
      const deviceToken = this.deviceToken;
      if (deviceToken === undefined) {
        return;
      }
      try {
        const tokens = await this.siteRequest<DeviceTokens>("/api/device/refresh", {
          deviceToken,
        });
        // The device logged out or linked again meanwhile, so the answer is
        // for an old token
        if (this.deviceToken === deviceToken) {
          this.setAccess(tokens);
        }
      } catch (err) {
        // The player removed this device on adsgames.net
        if (err instanceof PlayError && err.status === 401 && this.deviceToken === deviceToken) {
          this.accessToken = undefined;
          this.setDeviceToken(null);
        }
        // If the site could not be reached, requests go out as a guest
      }
    })().finally(() => {
      this.refreshing = undefined;
    });
    return this.refreshing;
  }

  private siteRequest<T>(path: string, body: unknown): Promise<T> {
    return this.send<T>(`${this.siteUrl}${path}`, "POST", undefined, body);
  }

  private async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    // A linked device needs a current play token first. Refresh again if the
    // device token changed while the last refresh ran.
    while (
      this.deviceToken !== undefined &&
      (this.accessToken === undefined || Date.now() >= this.accessExpires)
    ) {
      const deviceToken = this.deviceToken;
      await this.refreshAccess();
      if (this.deviceToken === deviceToken) {
        break;
      }
    }

    const playToken = this.accessToken;
    try {
      return await this.send<T>(`${this.baseUrl}${path}`, method, playToken ?? this.token, body);
    } catch (err) {
      // play turned the token down, get a new one for the next request
      // Keep a newer token got while this request ran.
      if (
        playToken !== undefined &&
        this.accessToken === playToken &&
        err instanceof PlayError &&
        err.notLoggedIn
      ) {
        this.accessToken = undefined;
      }
      throw err;
    }
  }

  private async send<T>(
    url: string,
    method: string,
    token: string | undefined,
    body?: unknown,
  ): Promise<T> {
    const headers: Record<string, string> = { Accept: "application/json" };
    if (body !== undefined) {
      headers["Content-Type"] = "application/json";
    }
    if (token !== undefined) {
      headers.Authorization = `Bearer ${token}`;
    }

    let res: Response;
    try {
      res = await this.fetchFn(url, {
        method,
        headers,
        ...(body === undefined ? {} : { body: JSON.stringify(body) }),
      });
    } catch (err) {
      throw new PlayError(`could not reach play: ${String(err)}`, 0);
    }

    // Unlink answers 204 with no body
    if (res.status === 204) {
      return undefined as T;
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

interface DeviceTokens {
  // Only in the answer to a link
  deviceToken?: string;
  accessToken: string;
  expiresIn: number;
  userId: string;
  username: string;
}

// The site in front of a play URL, "https://x/api/play" gives "https://x"
export function defaultSiteUrl(baseUrl: string): string {
  const suffix = "/api/play";
  return baseUrl.endsWith(suffix) ? baseUrl.slice(0, -suffix.length) : DEFAULT_SITE_URL;
}

function sleep(ms: number, signal: AbortSignal | undefined): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted === true) {
      reject(signal.reason);
      return;
    }
    const timer = setTimeout(() => {
      signal?.removeEventListener("abort", onAbort);
      resolve();
    }, ms);
    const onAbort = () => {
      clearTimeout(timer);
      reject(signal?.reason);
    };
    signal?.addEventListener("abort", onAbort, { once: true });
  });
}

function currentPage(): string | undefined {
  return typeof location === "undefined" ? undefined : location.href;
}
