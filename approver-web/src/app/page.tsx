"use client";

import { Box, Card, Container, HStack, Heading, Stack, Text } from "@chakra-ui/react";

import { RegisterPanel } from "@/components/RegisterPanel";
import { RequestCard } from "@/components/RequestCard";
import { SoundToggle } from "@/components/SoundToggle";
import { StatusBar } from "@/components/StatusBar";
import { StatuslinePlaque } from "@/components/StatuslinePlaque";
import { useApprovalStream, useNow } from "@/lib/use-approval-stream";
import { useRequestAlert } from "@/lib/use-request-alert";

export default function Page() {
  const { snapshot, streamOpen } = useApprovalStream();
  const now = useNow();
  const requests = snapshot?.requests ?? [];
  const live = snapshot?.status.connected ?? false;
  // null until the first snapshot, so the alert can tell "page just loaded"
  // from "a request just arrived".
  const { sound, toggleSound } = useRequestAlert(snapshot?.requests ?? null);

  return (
    <Container maxW="3xl" paddingY={12}>
      <Stack gap={8}>
        <Stack gap={2}>
          <HStack gap={3} justify="space-between">
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
            <SoundToggle sound={sound} onToggle={toggleSound} />
          </HStack>
          <Text color="fg.muted">
            Claude Code permission requests, live off the bus. Keep this tab open — it is the
            responder.
          </Text>
        </Stack>

        {/* Above the cards because it is context for them: how much of the rate
            limits is already spent is part of deciding whether to spend more.
            Five short rows, no controls — see StatuslinePlaque. */}
        <StatuslinePlaque doc={snapshot?.statusline ?? null} now={now} />

        {/* The requests come first: they are what the page is watched for, and
            they are the only part that changes. Registration is a once-per-browser
            errand and the status bar is reference material, so both sit below
            rather than pushing every card down the page. */}
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

        <RegisterPanel status={snapshot?.status ?? null} />

        <StatusBar status={snapshot?.status ?? null} streamOpen={streamOpen} />
      </Stack>
    </Container>
  );
}
