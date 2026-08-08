"use client";

/**
 * The model and the rate limits, off the `status` subject — `statusline/CLAUDE.md`
 * §9.7 rendered as a plaque instead of a terminal line.
 *
 * Below the requests and above the register panel, which is the same ordering rule
 * everything else on this page follows: the cards are what it is watched for, and
 * nothing that only needs a glance may push them down. Ahead of registration
 * because unlike that once-per-browser errand these numbers keep changing.
 *
 * Three rows: the model, the two rate-limit windows side by side, the context
 * window. The windows share a row because they are the same measurement over two
 * periods and are read as a pair — which is also how the terminal line prints
 * them.
 *
 * Quiet on purpose. The bars are thin, the type is small, and there is no control
 * on it: this is a readout, and the two buttons on each card must stay the
 * heaviest thing on the page (CLAUDE.md, "Look and feel", constraint 1).
 */
import { Badge, Card, HStack, Progress, Stack, Text } from "@chakra-ui/react";

import {
  countdown,
  type Gauge,
  gaugeTone,
  isStale,
  relativeAge,
  type StatusDoc,
  type StatusWindow,
  type Tone,
} from "@/lib/statusline";

/**
 * §9.2's traffic light in this app's palette: the house green for healthy, and the
 * orange the status bar already uses for "worth a look" rather than a raw yellow,
 * which does not survive a white card.
 */
const TONE_PALETTE: Record<Tone, string> = { green: "brand", yellow: "orange", red: "red" };

function GaugeRow({
  label,
  title,
  gauge,
  used,
  note,
}: {
  label: string;
  /** Spelled out for the progress bar's accessible name — `5h` alone says nothing. */
  title: string;
  gauge: Gauge;
  used: number;
  note?: string;
}) {
  return (
    // flex="1" + minW: two of these share a row and wrap to two when the card is
    // too narrow for both, rather than squeezing the bars to nothing.
    <HStack gap={3} flex="1" minW="15rem">
      {/* Spelled the way the terminal line spells them — `5h`, `7d`, `ctx` — not
          upper-cased into labels of their own. */}
      <Text fontSize="xs" fontFamily="mono" color="fg.subtle" minW="8">
        {label}
      </Text>
      {/* Capped rather than full-width: bars spanning the card turn the readout
          into the loudest thing on the page, and the cards above have to keep that
          job. The terminal line gets by on eight cells. */}
      <Progress.Root
        flex="1"
        maxW="9rem"
        size="sm"
        shape="full"
        value={used}
        colorPalette={TONE_PALETTE[gaugeTone(gauge, used)]}
        translations={{ value: ({ percent }) => `${title}: ${Math.round(percent)}% spent` }}
      >
        <Progress.Track>
          <Progress.Range />
        </Progress.Track>
      </Progress.Root>
      {/* Tabular figures: the numbers tick once a second and must not shuffle
          the row sideways as they change width. */}
      <Text fontSize="sm" fontVariantNumeric="tabular-nums" minW="10" textAlign="right">
        {Math.round(used)}%
      </Text>
      <Text fontSize="xs" color="fg.muted" fontVariantNumeric="tabular-nums" minW="12">
        {note ?? ""}
      </Text>
    </HStack>
  );
}

/**
 * The countdown, recomputed against our own ticking clock where possible.
 *
 * `resets_in_text` came resolved against the publisher's clock at `ts` (§9.7), so
 * on a page left open it goes stale while `resets_at` does not. It stays the
 * fallback for a window that arrived without one.
 */
function resetNote(window: StatusWindow, nowSeconds: number): string | undefined {
  if (window.resets_at !== undefined) return countdown(window.resets_at, nowSeconds);
  return window.resets_in_text;
}

export function StatuslinePlaque({ doc, now }: { doc: StatusDoc | null; now: number }) {
  if (!doc) {
    // Explained rather than hidden: an empty plaque otherwise reads as a bug in
    // this app, when the usual cause is that nothing has rendered a status line
    // since this process started (§9.7 publishes no history to catch up on).
    return (
      <Card.Root variant="subtle">
        <Card.Body paddingY={4}>
          <Text fontSize="sm" color="fg.muted">
            Model and limits show up here once a Claude Code status line publishes them.
          </Text>
        </Card.Body>
      </Card.Root>
    );
  }

  const nowSeconds = Math.floor(now / 1000);
  const five = doc.rate_limits?.five_hour;
  const seven = doc.rate_limits?.seven_day;
  const context = doc.context_window;
  const stale = isStale(doc.ts, now);

  return (
    <Card.Root>
      <Card.Body paddingY={5}>
        <Stack gap={4}>
          <HStack justify="space-between" wrap="wrap" gap={3}>
            <HStack gap={3} wrap="wrap">
              <Text fontWeight="semibold">{doc.model?.display_name ?? "unknown model"}</Text>
              {doc.model?.id ? (
                <Text fontSize="xs" fontFamily="mono" color="fg.subtle">
                  {doc.model.id}
                </Text>
              ) : null}
            </HStack>
            <HStack gap={2}>
              {stale ? (
                <Badge variant="subtle" colorPalette="orange">
                  stale
                </Badge>
              ) : null}
              <Text fontSize="xs" color="fg.subtle">
                {relativeAge(doc.ts, now)}
              </Text>
            </HStack>
          </HStack>

          {five || seven || context ? (
            <Stack gap={2}>
              {/* Both rate-limit windows on one row: same measurement over two
                  periods, read as a pair, and the terminal line puts them side by
                  side too. `wrap` is what keeps a narrow window honest. */}
              {five || seven ? (
                <HStack gap={6} wrap="wrap">
                  {five ? (
                    <GaugeRow
                      label="5h"
                      title="five-hour rate limit"
                      gauge="window"
                      used={five.used_percentage}
                      note={resetNote(five, nowSeconds)}
                    />
                  ) : null}
                  {seven ? (
                    <GaugeRow
                      label="7d"
                      title="seven-day rate limit"
                      gauge="window"
                      used={seven.used_percentage}
                      note={resetNote(seven, nowSeconds)}
                    />
                  ) : null}
                </HStack>
              ) : null}
              {context ? (
                <GaugeRow
                  label="ctx"
                  title="context window"
                  gauge="context"
                  used={context.used_percentage}
                />
              ) : null}
            </Stack>
          ) : (
            <Text fontSize="sm" color="fg.muted">
              limits n/a — that session has no rate limits to report (an API key rather than a
              subscription, or no API response yet).
            </Text>
          )}

          {/* Which session these numbers are from. Every session on the machine
              publishes to the same subject, so the newest one wins — without this
              the plaque would look like it belongs to the request below it. */}
          {doc.cwd ? (
            <Text fontSize="xs" fontFamily="mono" color="fg.subtle" wordBreak="break-all">
              {doc.cwd}
            </Text>
          ) : null}
        </Stack>
      </Card.Body>
    </Card.Root>
  );
}
