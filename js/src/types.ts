// Response shapes of the play API. Times are ISO 8601 strings.

export interface Session {
  loggedIn: boolean;
  userId?: string;
  username?: string;
}

export type LeaderboardOrder = "asc" | "desc";

export type LeaderboardFormat = "integer" | "time_ms";

export interface Leaderboard {
  key: string;
  title: string;
  // "asc" means lower is better, for example times
  order: LeaderboardOrder;
  format: LeaderboardFormat;
  minValue?: number;
  maxValue?: number;
}

export type ScoreMeta = Record<string, unknown>;

export interface Score {
  rank: number;
  userId: string;
  username: string;
  value: number;
  meta: ScoreMeta | null;
  updatedAt: string;
}

export interface LeaderboardPage {
  leaderboard: Leaderboard;
  scores: Score[];
  // The player's own best, null for guests or players with no score
  me: Score | null;
}

export interface SubmitScoreResult {
  // False when the player's best did not change
  improved: boolean;
  best: Score;
}

export interface Achievement {
  // Left out for hidden achievements the player has not unlocked
  key?: string;
  title: string;
  description: string;
  icon?: string;
  hidden: boolean;
  points: number;
  unlockCount: number;
  unlockedAt: string | null;
}

// Unlocks achievement `key` once the game's stat reaches `atLeast`, see
// PlayGame.addStat()
export interface StatRule {
  key: string;
  stat: string;
  atLeast: number;
}

// Code for the player to enter on adsgames.net/link, see PlayClient.link()
export interface LinkCode {
  // Short code to show, e.g. "BCDF-GHJK"
  userCode: string;
  // Page where the player enters the code
  verificationUri: string;
  // The same page with the code filled in, e.g. for a QR code or a browser
  verificationUriComplete: string;
  // Seconds until the code runs out
  expiresIn: number;
}

// The player a device was linked to
export interface Linked {
  userId: string;
  username: string;
}

export interface UnlockResult {
  // False when the player already had it
  newlyUnlocked: boolean;
  unlockedAt: string;
}

export interface UserAchievement {
  game: string;
  key: string;
  title: string;
  description: string;
  icon?: string;
  points: number;
  unlockedAt: string;
}

export interface UserAchievements {
  achievements: UserAchievement[];
  points: number;
}
