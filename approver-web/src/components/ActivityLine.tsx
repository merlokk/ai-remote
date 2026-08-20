"use client";

/**
 * What Claude is doing, off the `activity` subject — `statusline/CLAUDE.md` §9.10.
 *
 * Directly under the plaque, because the two answer neighbouring questions about
 * the same session: that one is what it is *spending*, this one is what it is
 * *doing*. A card of its own rather than a fourth row inside the plaque, because
 * the publishers are independent — hooks fire on tool calls, the status line on
 * renders — and either can arrive with the other absent.
 *
 * One line, and quieter than the plaque: a dot for the state, the tool and its
 * summary in mono, the age on the right. No control, no colour but the house
 * green, nothing red — a busy session is not a problem, and the two buttons on
 * each request card stay the heaviest thing on the page (CLAUDE.md, "Look and
 * feel", constraint 1).
 */
import { Box, Card, HStack, Text } from "@chakra-ui/react";

import {
  type ActivityDoc,
  activityHeadline,
  isActivityStale,
  STATE_LABELS,
  STATE_PALETTE,
} from "@/lib/activity";
import { relativeAge } from "@/lib/statusline";

/**
 * The dot: filled and green while there is work, hollow when the turn is over.
 *
 * Deliberately the same shape as the connection dot in the page header and the
 * one at the head of the terminal line (§9.2) — a lamp, read at a glance, not a
 * badge competing with the model name above it.
 */
function StateDot({ palette, hollow }: { palette: string; hollow: boolean }) {
  return (
    <Box
      width="2"
      height="2"
      borderRadius="full"
      colorPalette={palette}
      bg={hollow ? "transparent" : "colorPalette.solid"}
      borderWidth={hollow ? "2px" : "0"}
      borderColor="border.emphasized"
      flexShrink={0}
    />
  );
}

export function ActivityLine({ doc, now }: { doc: ActivityDoc | null; now: number }) {
  if (!doc) {
    // Said rather than hidden, like the empty plaque: an absent row otherwise
    // reads as a bug here, when the usual cause is that no session has run a
    // tool since this process started (§9.10 publishes no history either).
    return (
      <Card.Root variant="subtle">
        <Card.Body paddingY={3}>
          <Text fontSize="xs" color="fg.muted">
            What Claude is doing shows up here once a session runs a tool.
          </Text>
        </Card.Body>
      </Card.Root>
    );
  }

  const idle = doc.state === "idle";
  const stale = isActivityStale(doc, now);

  return (
    <Card.Root variant="subtle">
      <Card.Body paddingY={3}>
        <HStack gap={3} justify="space-between">
          <HStack gap={3} minW="0">
            <StateDot palette={STATE_PALETTE[doc.state]} hollow={idle || stale} />
            <Text fontSize="xs" color="fg.subtle" minW="14">
              {STATE_LABELS[doc.state]}
            </Text>
            {/* One line, truncated: the publisher already cut the summary to 80
                characters (§9.10), and a narrow window may not have room even for
                that. `title` is what makes the whole of it recoverable. */}
            <Text
              fontSize="sm"
              fontFamily="mono"
              color={idle || stale ? "fg.muted" : "fg"}
              truncate
              title={activityHeadline(doc)}
            >
              {activityHeadline(doc)}
            </Text>
          </HStack>
          {/* Coarse, like everything else on this page: the exact second a tool
              started is not something anyone acts on. */}
          <Text fontSize="xs" color="fg.subtle" flexShrink={0}>
            {relativeAge(doc.ts, now)}
          </Text>
        </HStack>
      </Card.Body>
    </Card.Root>
  );
}
