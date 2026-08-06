"use client";

/**
 * The "a request is waiting" chirp — synthesised, not a file.
 *
 * Two oscillators through a gain envelope: no asset to ship, no dependency to
 * approve (root §1), nothing to fetch at runtime. The envelope matters — a bare
 * `start()`/`stop()` on a square-edged gain produces an audible click at both
 * ends, which sounds like a fault rather than a notification.
 *
 * Browsers refuse to play audio until the page has had a user gesture, so the
 * context starts suspended and {@link enableAudio} must be called from a click.
 * That is why the UI has a sound toggle at all: it doubles as the gesture.
 */

let ctx: AudioContext | null = null;

type Ctor = typeof AudioContext;

function audioContextCtor(): Ctor | null {
  if (typeof window === "undefined") return null;
  return (
    window.AudioContext ??
    (window as unknown as { webkitAudioContext?: Ctor }).webkitAudioContext ??
    null
  );
}

export function audioSupported(): boolean {
  return audioContextCtor() !== null;
}

/** "running" once the browser has let us make noise; "suspended" until a gesture. */
export function audioState(): AudioContextState | "absent" {
  return ctx?.state ?? "absent";
}

/** Create/resume the context. Call from a click handler, or it stays suspended. */
export async function enableAudio(): Promise<boolean> {
  const Ctor = audioContextCtor();
  if (!Ctor) return false;
  ctx ??= new Ctor();
  if (ctx.state === "suspended") {
    try {
      await ctx.resume();
    } catch {
      return false;
    }
  }
  return ctx.state === "running";
}

function tone(at: number, frequency: number, duration: number, peak: number): void {
  if (!ctx) return;
  const osc = ctx.createOscillator();
  const gain = ctx.createGain();
  osc.type = "sine";
  osc.frequency.setValueAtTime(frequency, at);
  // Fade in and out: 8 ms each side is enough to kill the click.
  gain.gain.setValueAtTime(0, at);
  gain.gain.linearRampToValueAtTime(peak, at + 0.008);
  gain.gain.setValueAtTime(peak, at + duration - 0.008);
  gain.gain.linearRampToValueAtTime(0, at + duration);
  osc.connect(gain).connect(ctx.destination);
  osc.start(at);
  osc.stop(at + duration);
}

/**
 * A two-note rising chirp. Deliberately short and quiet: this fires every time
 * Claude Code asks for permission, and an alert you start muting is worse than
 * no alert.
 */
export async function playAlert(): Promise<void> {
  if (!(await enableAudio()) || !ctx) return;
  const now = ctx.currentTime;
  tone(now, 660, 0.11, 0.14);
  tone(now + 0.13, 990, 0.15, 0.14);
}
