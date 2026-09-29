/**
 * A failed request. `status` is the HTTP status, or 0 when the server could
 * not be reached or did not answer with JSON.
 */
export class PlayError extends Error {
  public readonly status: number;

  public constructor(message: string, status: number) {
    super(message);
    this.name = "PlayError";
    this.status = status;
  }

  // The player is a guest, scores and unlocks need an adsgames.net login
  public get notLoggedIn(): boolean {
    return this.status === 401;
  }

  // A score was posted again before the server's score interval ended
  public get rateLimited(): boolean {
    return this.status === 429;
  }
}
