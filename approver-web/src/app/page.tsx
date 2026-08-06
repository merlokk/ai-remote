"use client";

import { Box, Card, Container, HStack, Heading, Stack, Text } from "@chakra-ui/react";

import { RegisterPanel } from "@/components/RegisterPanel";
import { RequestCard } from "@/components/RequestCard";
import { StatusBar } from "@/components/StatusBar";
import { useApprovalStream, useNow } from "@/lib/use-approval-stream";

export default function Page() {
  const { snapshot, streamOpen } = useApprovalStream();
  const now = useNow();
  const requests = snapshot?.requests ?? [];
  const live = snapshot?.status.connected ?? false;

  return (
    <Container maxW="3xl" paddingY={12}>
      <Stack gap={8}>
        <Stack gap={2}>
          <HStack gap={3}>
            {/* The one purely decorative use of the house colour. */}
            <Box
              width="2.5"
              height="2.5"
              borderRadius="full"
              bg={live ? "brand.solid" : "border.emphasized"}
            />
            <Heading size="2xl" letterSpacing="tight">
              approver-web
            </Heading>
          </HStack>
          <Text color="fg.muted">
            Claude Code permission requests, live off the bus. Keep this tab open — it is the
            responder.
          </Text>
        </Stack>

        <RegisterPanel status={snapshot?.status ?? null} />

        <StatusBar status={snapshot?.status ?? null} streamOpen={streamOpen} />

        {requests.length === 0 ? (
          <Card.Root variant="subtle">
            <Card.Body paddingY={16}>
              <Stack gap={1} textAlign="center">
                <Text fontWeight="medium">Nothing waiting</Text>
                <Text color="fg.muted" fontSize="sm">
                  Trigger a permission request in Claude Code and it shows up here.
                </Text>
              </Stack>
            </Card.Body>
          </Card.Root>
        ) : (
          <Stack gap={6}>
            {requests.map((pending) => (
              <RequestCard key={pending.nonce} pending={pending} now={now} />
            ))}
          </Stack>
        )}
      </Stack>
    </Container>
  );
}
