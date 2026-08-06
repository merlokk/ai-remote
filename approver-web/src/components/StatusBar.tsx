"use client";

import { Badge, Card, HStack, Stack, Text } from "@chakra-ui/react";

import type { ResponderStatus } from "@/lib/types";

function Row({ label, value }: { label: string; value: string }) {
  return (
    <HStack gap={3} align="baseline">
      <Text fontSize="xs" color="fg.subtle" textTransform="uppercase" letterSpacing="wide" minW="20">
        {label}
      </Text>
      <Text fontSize="sm" fontFamily="mono" color="fg.muted" wordBreak="break-all">
        {value}
      </Text>
    </HStack>
  );
}

export function StatusBar({
  status,
  streamOpen,
}: {
  status: ResponderStatus | null;
  streamOpen: boolean;
}) {
  if (!status) {
    return (
      <Card.Root>
        <Card.Body>
          <Text color="fg.muted">connecting to the responder…</Text>
        </Card.Body>
      </Card.Root>
    );
  }

  const unsigned = status.signing_mode === "unsigned";

  return (
    <Card.Root>
      <Card.Body>
        <Stack gap={5}>
          <HStack gap={2} wrap="wrap">
            <Badge colorPalette={status.connected ? "brand" : "red"} variant="subtle">
              NATS {status.connected ? "connected" : "disconnected"}
            </Badge>
            <Badge colorPalette={streamOpen ? "brand" : "orange"} variant="subtle">
              stream {streamOpen ? "live" : "reconnecting"}
            </Badge>
            <Badge colorPalette={unsigned ? "orange" : "brand"} variant="subtle">
              {unsigned ? "unsigned — not registered" : `signing: ${status.signing_mode}`}
            </Badge>
          </HStack>

          <Stack gap={2}>
            <Row label="servers" value={status.servers} />
            <Row label="subject" value={`${status.subject} (queue: ${status.queue})`} />
            <Row label="key" value={`${status.key_id} / ${status.key_type}`} />
            <Row
              label="config"
              value={
                status.config_from_file
                  ? status.config_path
                  : `${status.config_path} (defaults — no file)`
              }
            />
          </Stack>

          {status.error ? (
            <Text fontSize="sm" color="red.fg">
              {status.error}
            </Text>
          ) : null}

          {unsigned ? (
            <Text fontSize="sm" color="fg.muted">
              No key yet, so decisions go out unsigned, <Text as="code">hook.py</Text> rejects
              them and Claude Code falls back to its own prompt. Register above to fix that.
            </Text>
          ) : null}
        </Stack>
      </Card.Body>
    </Card.Root>
  );
}
