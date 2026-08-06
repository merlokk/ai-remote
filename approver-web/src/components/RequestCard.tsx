"use client";

import { Badge, Card, Code, HStack, Stack, Text } from "@chakra-ui/react";

import type { PendingRequest } from "@/lib/types";

import { DecisionForm } from "./DecisionForm";

function Meta({ label, value }: { label: string; value: string }) {
  return (
    <HStack gap={2} align="baseline">
      <Text fontSize="xs" color="fg.muted" textTransform="uppercase" minW="16">
        {label}
      </Text>
      <Text fontSize="sm" fontFamily="mono" wordBreak="break-all">
        {value}
      </Text>
    </HStack>
  );
}

export function RequestCard({ pending, now }: { pending: PendingRequest; now: number }) {
  const { request } = pending;
  const remaining = Math.max(0, Math.ceil((pending.expires_at - now) / 1000));

  return (
    <Card.Root>
      <Card.Header>
        <HStack justify="space-between" wrap="wrap" gap={2}>
          <HStack gap={2}>
            <Badge colorPalette="purple" size="lg">
              {request.tool_name}
            </Badge>
            {request.permission_mode ? (
              <Badge variant="outline">{request.permission_mode}</Badge>
            ) : null}
          </HStack>
          <Badge colorPalette={remaining <= 15 ? "red" : "gray"}>{remaining}s left</Badge>
        </HStack>
      </Card.Header>

      <Card.Body>
        <Stack gap={3}>
          <Stack gap={1}>
            <Meta label="session" value={request.session_id} />
            {request.cwd ? <Meta label="cwd" value={request.cwd} /> : null}
            <Meta label="input sha" value={request.input_sha256} />
          </Stack>

          <Code
            display="block"
            whiteSpace="pre-wrap"
            wordBreak="break-all"
            p={3}
            borderRadius="md"
            fontSize="sm"
          >
            {JSON.stringify(request.tool_input ?? {}, null, 2)}
          </Code>
        </Stack>
      </Card.Body>

      <Card.Footer>
        <DecisionForm nonce={pending.nonce} />
      </Card.Footer>
    </Card.Root>
  );
}
