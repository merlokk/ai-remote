"use client";

import { Badge, Box, Card, Code, HStack, Separator, Stack, Text } from "@chakra-ui/react";

import type { PendingRequest } from "@/lib/types";

import { DecisionForm } from "./DecisionForm";

function Meta({ label, value }: { label: string; value: string }) {
  return (
    <HStack gap={3} align="baseline">
      <Text fontSize="xs" color="fg.subtle" textTransform="uppercase" letterSpacing="wide" minW="16">
        {label}
      </Text>
      <Text fontSize="sm" fontFamily="mono" color="fg.muted" wordBreak="break-all">
        {value}
      </Text>
    </HStack>
  );
}

export function RequestCard({ pending, now }: { pending: PendingRequest; now: number }) {
  const { request } = pending;
  const remaining = Math.max(0, Math.ceil((pending.expires_at - now) / 1000));
  const urgent = remaining <= 15;

  return (
    <Card.Root>
      <Card.Header>
        <HStack justify="space-between" wrap="wrap" gap={3}>
          <HStack gap={2}>
            <Badge size="lg" variant="solid" colorPalette="gray">
              {request.tool_name}
            </Badge>
            {request.permission_mode ? (
              <Badge variant="outline" colorPalette="gray">
                {request.permission_mode}
              </Badge>
            ) : null}
          </HStack>
          <Badge variant="subtle" colorPalette={urgent ? "red" : "gray"}>
            {remaining}s left
          </Badge>
        </HStack>
      </Card.Header>

      <Card.Body>
        <Stack gap={5}>
          {/* The command is the subject of the decision, so it outweighs
              everything else on the card — including the buttons below. */}
          <Stack gap={2}>
            <Text fontSize="sm" fontWeight="medium">
              Claude Code is asking to run:
            </Text>
            <Code
              size="lg"
              variant="subtle"
              colorPalette="gray"
              display="block"
              whiteSpace="pre-wrap"
              wordBreak="break-all"
              padding={4}
              lineHeight="tall"
            >
              {JSON.stringify(request.tool_input ?? {}, null, 2)}
            </Code>
          </Stack>

          <Box>
            <Stack gap={2}>
              <Meta label="session" value={request.session_id} />
              {request.cwd ? <Meta label="cwd" value={request.cwd} /> : null}
              <Meta label="input sha" value={request.input_sha256} />
            </Stack>
          </Box>
        </Stack>
      </Card.Body>

      <Separator />

      <Card.Footer paddingTop={5}>
        <DecisionForm nonce={pending.nonce} request={request} />
      </Card.Footer>
    </Card.Root>
  );
}
