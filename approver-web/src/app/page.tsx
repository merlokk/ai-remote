"use client";

import { Box, Container, Heading, Stack, Text } from "@chakra-ui/react";

import { RequestCard } from "@/components/RequestCard";
import { StatusBar } from "@/components/StatusBar";
import { useApprovalStream, useNow } from "@/lib/use-approval-stream";

export default function Page() {
  const { snapshot, streamOpen } = useApprovalStream();
  const now = useNow();
  const requests = snapshot?.requests ?? [];

  return (
    <Container maxW="4xl" py={8}>
      <Stack gap={6}>
        <Stack gap={1}>
          <Heading size="2xl">approver-web</Heading>
          <Text color="fg.muted">
            Claude Code permission requests, live off the bus. Keep this tab open — it is the
            responder.
          </Text>
        </Stack>

        <StatusBar status={snapshot?.status ?? null} streamOpen={streamOpen} />

        {requests.length === 0 ? (
          <Box borderWidth="1px" borderStyle="dashed" borderRadius="md" p={10} textAlign="center">
            <Text color="fg.muted">
              No pending requests. Trigger one in Claude Code and it shows up here.
            </Text>
          </Box>
        ) : (
          <Stack gap={4}>
            {requests.map((pending) => (
              <RequestCard key={pending.nonce} pending={pending} now={now} />
            ))}
          </Stack>
        )}
      </Stack>
    </Container>
  );
}
