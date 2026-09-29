import { PlayError } from "./errors.ts";
import { type LinkOptions, PlayClient, type PlayOptions } from "./client.ts";
import type { Linked, ScoreMeta, StatRule, SubmitScoreResult } from "./types.ts";

/**
 * Everything a game needs from play, on top of PlayClient. Mirrors the C++
 * SDK's Game:
 *
 * - Who is playing: logged in, a guest, or play can not be reached.
 * - Stats saved on the device, e.g. deaths or levels finished. Achievements
 *   with a stat rule in the game's adsgames.json unlock on their own.
 * - A queue of unlocks. They are saved, sent once the player is logged in,
 *   and retried until play confirms them, so guests keep what they earn.
 * - Notices for newly unlocked achievements, with their titles, to show.
 *
 * The game still decides when things happen and how they look.
 */

export type PlayerStatus = "unknown" | "guest" | "loggedIn" | "offline";

export interface Player {
  status: PlayerStatus;
  userId?: string;
  username?: string;
}

// A newly unlocked achievement, for the game to show
export interface UnlockNotice {
  key: string;
  title: string;
  description: string;
  icon?: string;
  points: number;
}

// Where PlayGame saves its state. localStorage fits, and so does a Map-like
// object in Node or tests.
export interface PlayStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

export interface PlayGameOptions extends PlayOptions {
  // localStorage by default, state is only kept in memory without it
  storage?: PlayStorage;
}

export interface PlayGameEvents {
  unlock: UnlockNotice;
  player: Player;
}

interface SavedState {
  counters: Record<string, number>;
  sets: Record<string, number[]>;
  pending: string[];
  // Confirmed unlocks by user id
  confirmed: Record<string, string[]>;
}

// How long to wait before asking play again after a failure
const RETRY_MS = 30_000;

// How often to check who is playing while offline or a guest
const SESSION_RECHECK_MS = 60_000;

type Handler<P> = (payload: P) => void;

export class PlayGame {
  public readonly client: PlayClient;

  // Resolves once play first says who is playing, or can not be reached
  public readonly ready: Promise<void>;

  private readonly storage: PlayStorage | undefined;

  private readonly storageKey: string;

  private currentPlayer: Player = { status: "unknown" };

  private readonly counters = new Map<string, number>();

  private readonly sets = new Map<string, Set<number>>();

  private readonly pending = new Set<string>();

  private readonly sending = new Set<string>();

  private readonly confirmed = new Map<string, Set<string>>();

  private rules: StatRule[] | undefined;

  private readonly notices: UnlockNotice[] = [];

  private readonly handlers: {
    [K in keyof PlayGameEvents]: Set<Handler<PlayGameEvents[K]>>;
  } = { unlock: new Set(), player: new Set() };

  private readonly timers = new Set<ReturnType<typeof setTimeout>>();

  private sessionTimer: ReturnType<typeof setTimeout> | undefined;

  private disposed = false;

  public constructor(options: PlayGameOptions) {
    const { storage, onDeviceToken, ...clientOptions } = options;

    this.client = new PlayClient({
      ...clientOptions,
      // Check who is playing when the device logs in or out
      onDeviceToken: (token) => {
        onDeviceToken?.(token);
        void this.refresh();
      },
    });

    this.storage = storage ?? defaultStorage();
    this.storageKey = `adsgames-play:${this.client.siteUrl}:${this.client.game}`;
    this.load();

    this.ready = this.refresh();
  }

  public get player(): Player {
    return this.currentPlayer;
  }

  public on<K extends keyof PlayGameEvents>(
    event: K,
    handler: Handler<PlayGameEvents[K]>,
  ): () => void {
    this.handlers[event].add(handler);
    return () => {
      this.handlers[event].delete(handler);
    };
  }

  // Ask play who is playing, e.g. after the player logs in in another tab
  public async refresh(): Promise<void> {
    if (this.disposed) {
      return;
    }
    clearTimeout(this.sessionTimer);

    const wasLoggedIn = this.currentPlayer.status === "loggedIn";
    try {
      const session = await this.client.session();
      this.setPlayer(
        session.loggedIn
          ? { status: "loggedIn", userId: session.userId, username: session.username }
          : { status: "guest" },
      );
    } catch (err) {
      const status = err instanceof PlayError ? err.status : 0;
      this.setPlayer({ status: unreachable(status) ? "offline" : "guest" });
    }

    if (this.currentPlayer.status === "loggedIn") {
      if (!wasLoggedIn) {
        // Progress made before this login counts for this player
        await this.loadRules();
        this.applyRules();
        this.sendUnlocks();
      }
    } else {
      // A guest may log in elsewhere, and play may come back
      this.sessionTimer = this.later(SESSION_RECHECK_MS, () => void this.refresh());
    }
  }

  // --- Stats, saved on this device ---

  // A counter or the size of a set, 0 if the game never set it
  public stat(name: string): number {
    return this.counters.get(name) ?? this.sets.get(name)?.size ?? 0;
  }

  // Add to a counter, e.g. addStat("deaths")
  public addStat(name: string, amount = 1): void {
    this.counters.set(name, (this.counters.get(name) ?? 0) + amount);
    this.save();
    this.applyRules();
  }

  // Add a value to a set, the stat is how many different values it has
  public addToStatSet(name: string, value: number): void {
    const set = this.sets.get(name) ?? new Set<number>();
    if (set.has(value)) {
      return;
    }
    set.add(value);
    this.sets.set(name, set);
    this.save();
    this.applyRules();
  }

  // --- Achievements ---

  // Unlock an achievement. Safe to call again, it is sent once. Saved and
  // sent once the player is logged in.
  public unlock(key: string): void {
    if (this.unlocked(key) || this.pending.has(key)) {
      return;
    }
    this.pending.add(key);
    this.save();
    this.sendUnlocks();
  }

  // True once play confirmed the achievement for the logged in player
  public unlocked(key: string): boolean {
    const { status, userId } = this.currentPlayer;
    return (
      status === "loggedIn" && userId !== undefined && this.confirmed.get(userId)?.has(key) === true
    );
  }

  // The next newly unlocked achievement to show, for games with a frame loop.
  // on("unlock") gets the same notices as they arrive.
  public nextUnlock(): UnlockNotice | undefined {
    return this.notices.shift();
  }

  // --- Leaderboards ---

  // Keeps the player's best. Guests get a 401 PlayError without a request.
  public submitScore(key: string, value: number, meta?: ScoreMeta): Promise<SubmitScoreResult> {
    const { status } = this.currentPlayer;
    if (status === "guest") {
      return Promise.reject(new PlayError("log in to save scores", 401));
    }
    if (status === "offline") {
      return Promise.reject(new PlayError("could not reach play", 0));
    }

    return this.client.submitScore(key, value, meta).catch((err: unknown) => {
      if (err instanceof PlayError && err.notLoggedIn) {
        void this.refresh();
      }
      throw err;
    });
  }

  // --- Device login, see PlayClient.link() ---

  public async link(options: LinkOptions): Promise<Linked> {
    const linked = await this.client.link(options);
    await this.refresh();
    return linked;
  }

  public async unlink(): Promise<void> {
    await this.client.unlink();
    await this.refresh();
  }

  // Stop retries and re-checks, e.g. when the game closes
  public dispose(): void {
    this.disposed = true;
    for (const timer of this.timers) {
      clearTimeout(timer);
    }
    this.timers.clear();
  }

  private setPlayer(player: Player): void {
    const changed =
      player.status !== this.currentPlayer.status || player.userId !== this.currentPlayer.userId;
    this.currentPlayer = player;
    if (changed) {
      this.emit("player", player);
    }
  }

  private async loadRules(): Promise<void> {
    if (this.rules !== undefined) {
      return;
    }
    try {
      this.rules = await this.client.achievementRules();
    } catch {
      this.later(RETRY_MS, () => {
        void this.loadRules().then(() => {
          this.applyRules();
        });
      });
    }
  }

  private applyRules(): void {
    for (const rule of this.rules ?? []) {
      if (this.stat(rule.stat) >= rule.atLeast) {
        this.unlock(rule.key);
      }
    }
  }

  private sendUnlocks(): void {
    const { status, userId } = this.currentPlayer;
    if (status !== "loggedIn" || userId === undefined) {
      return;
    }

    for (const key of this.pending) {
      if (this.sending.has(key)) {
        continue;
      }
      this.sending.add(key);
      void this.sendUnlock(key, userId);
    }
  }

  private async sendUnlock(key: string, userId: string): Promise<void> {
    try {
      const res = await this.client.unlock(key);
      this.pending.delete(key);
      const confirmed = this.confirmed.get(userId) ?? new Set<string>();
      confirmed.add(key);
      this.confirmed.set(userId, confirmed);
      this.save();
      if (res.newlyUnlocked) {
        await this.notify(key);
      }
    } catch (err) {
      const status = err instanceof PlayError ? err.status : 0;
      if (status === 404) {
        // Not in the game's manifest, sending again will not help
        this.pending.delete(key);
        this.save();
      } else {
        if (status === 401) {
          // The login ran out or was removed, find out who is playing now
          void this.refresh();
        }
        // Still pending, so sent again if the player is logged in by then
        this.later(RETRY_MS, () => {
          this.sendUnlocks();
        });
      }
    } finally {
      this.sending.delete(key);
    }
  }

  // Hidden achievements only have a title once unlocked, so ask play
  private async notify(key: string): Promise<void> {
    const notice: UnlockNotice = { key, title: key, description: "", points: 0 };
    try {
      const found = (await this.client.achievements()).find((a) => a.key === key);
      if (found) {
        notice.title = found.title;
        notice.description = found.description;
        notice.points = found.points;
        if (found.icon !== undefined) {
          notice.icon = found.icon;
        }
      }
    } catch {
      // Show the key rather than nothing
    }
    this.notices.push(notice);
    this.emit("unlock", notice);
  }

  private emit<K extends keyof PlayGameEvents>(event: K, payload: PlayGameEvents[K]): void {
    for (const handler of this.handlers[event]) {
      handler(payload);
    }
  }

  private later(ms: number, run: () => void): ReturnType<typeof setTimeout> {
    const timer = setTimeout(() => {
      this.timers.delete(timer);
      if (!this.disposed) {
        run();
      }
    }, ms);
    this.timers.add(timer);
    return timer;
  }

  private load(): void {
    let state: Partial<SavedState> | undefined;
    try {
      const raw = this.storage?.getItem(this.storageKey);
      state = raw ? (JSON.parse(raw) as Partial<SavedState>) : undefined;
    } catch {
      // A damaged save starts over
    }
    if (typeof state !== "object" || state === null) {
      return;
    }

    for (const [name, value] of Object.entries(state.counters ?? {})) {
      if (Number.isSafeInteger(value)) {
        this.counters.set(name, value);
      }
    }
    for (const [name, values] of Object.entries(state.sets ?? {})) {
      if (Array.isArray(values)) {
        this.sets.set(name, new Set(values.filter((v) => Number.isSafeInteger(v))));
      }
    }
    for (const key of Array.isArray(state.pending) ? state.pending : []) {
      if (typeof key === "string") {
        this.pending.add(key);
      }
    }
    for (const [user, keys] of Object.entries(state.confirmed ?? {})) {
      if (Array.isArray(keys)) {
        this.confirmed.set(user, new Set(keys.filter((k) => typeof k === "string")));
      }
    }
  }

  private save(): void {
    const state: SavedState = {
      counters: Object.fromEntries(this.counters),
      sets: Object.fromEntries([...this.sets].map(([name, set]) => [name, [...set]])),
      pending: [...this.pending],
      confirmed: Object.fromEntries([...this.confirmed].map(([user, keys]) => [user, [...keys]])),
    };
    try {
      this.storage?.setItem(this.storageKey, JSON.stringify(state));
    } catch {
      // Storage can be full or blocked, the state then lasts for this session
    }
  }
}

function unreachable(status: number): boolean {
  return status === 0 || status === 404 || status >= 500;
}

function defaultStorage(): PlayStorage | undefined {
  try {
    return typeof localStorage === "undefined" ? undefined : localStorage;
  } catch {
    // Accessing localStorage throws when the browser blocks it
    return undefined;
  }
}
