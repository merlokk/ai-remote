"use client";

import { Badge, Box, HStack, Stack, Text } from "@chakra-ui/react";

import type { ResponderStatus } from "@/lib/types";

function Field({ label, value }: { label: string; value: string }) {
  return (
    <HStack gap={2}>
      <Text fontSize="xs" color="fg.muted" textTransform="uppercase">
        {label}
      </Text>
      <Text fontSize="sm" fontFamily="mono">
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
      <Box borderWidth="1px" borderRadius="md" p={4}>
        <Text color="fg.muted">connecting to the responder…</Text>
      </Box>
    );
  }

  return (
    <Box borderWidth="1px" borderRadius="md" p={4}>
      <Stack gap={3}>
        <HStack gap={3} wrap="wrap">
          <Badge colorPalette={status.connected ? "green" : "red"}>
            NATS {status.connected ? "connected" : "disconnected"}
          </Badge>
          <Badge colorPalette={streamOpen ? "green" : "orange"}>
            stream {streamOpen ? "live" : "reconnecting"}
          </Badge>
          <Badge colorPalette={status.signing_mode === "unsigned" ? "orange" : "green"}>
            {status.signing_mode === "unsigned" ? "unsigned (phase 1)" : status.signing_mode}
          </Badge>
        </HStack>

        <Stack gap={1}>
          <Field label="servers" value={status.servers} />
          <Field label="subject" value={`${status.subject} (queue: ${status.queue})`} />
          <Field label="key" value={`${status.key_id} / ${status.key_type}`} />
          <Field
            label="config"
            value={status.config_from_file ? status.config_path : `${status.config_path} (defaults — no file)`}
          />
        </Stack>

        {status.error ? (
          <Text fontSize="sm" color="red.fg">
            {status.error}
          </Text>
        ) : null}

        {status.signing_mode === "unsigned" ? (
          <Text fontSize="sm" color="fg.muted">
            Decisions are sent without a signature, so <code>hook.py</code> rejects them and
            Claude Code falls back to its own prompt. Signing arrives in phase 2.
          </Text>
        ) : null}
      </Stack>
    </Box>
  );
}
