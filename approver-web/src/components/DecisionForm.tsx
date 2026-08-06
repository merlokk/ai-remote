"use client";

/**
 * allow / deny for one request.
 *
 * There is no "skip" button on purpose: skipping is *not replying*, which is
 * what happens if you do nothing until the countdown runs out. Inventing a
 * button for it would suggest something is sent.
 *
 * Both buttons are solid and the same size. Green is this app's house colour
 * (CLAUDE.md, "Look and feel"), which is exactly why Allow must not be merely
 * "the green one": the two answers carry equal visual weight and are told apart
 * by their labels, not by colour alone.
 */
import {
  Button,
  Collapsible,
  Field,
  HStack,
  Input,
  Stack,
  Text,
  Textarea,
} from "@chakra-ui/react";
import { zodResolver } from "@hookform/resolvers/zod";
import { useState } from "react";
import { useForm } from "react-hook-form";

import { type Behavior, type DecisionFormValues, decisionFormSchema } from "@/lib/schemas";
import type { DecisionResult } from "@/lib/types";

export function DecisionForm({ nonce }: { nonce: string }) {
  const [sent, setSent] = useState<{ behavior: Behavior; result: DecisionResult } | null>(null);
  const [failure, setFailure] = useState<string | null>(null);
  const [detailsOpen, setDetailsOpen] = useState(false);

  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<DecisionFormValues>({
    resolver: zodResolver(decisionFormSchema),
    defaultValues: { reason: "", updatedInput: "" },
  });

  const submit = (behavior: Behavior) =>
    handleSubmit(async (values) => {
      setFailure(null);
      const text = values.updatedInput.trim();
      // Validated by the schema above, so this parse cannot throw here.
      const updated = behavior === "allow" && text ? JSON.parse(text) : null;

      try {
        const response = await fetch("/api/decision", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ nonce, behavior, reason: values.reason, updated_input: updated }),
        });
        const result = (await response.json()) as DecisionResult;
        if (result.ok) setSent({ behavior, result });
        else setFailure(result.error ?? "the server refused the decision");
      } catch (err) {
        setFailure(err instanceof Error ? err.message : String(err));
      }
    });

  if (sent) {
    return (
      <Text fontSize="sm" color="fg.muted">
        sent <Text as="strong">{sent.behavior}</Text>
        {sent.result.signed ? " (signed)" : " — unsigned, the hook will reject it"}
      </Text>
    );
  }

  // Both fields are optional, and open on every card they buried the buttons
  // under two screens of form. Collapsed by default — but forced open whenever
  // validation has something to say, or the message would be invisible.
  const invalid = !!errors.reason || !!errors.updatedInput;
  const expanded = detailsOpen || invalid;

  return (
    <Stack gap={5} width="100%">
      <Collapsible.Root
        open={expanded}
        onOpenChange={(e) => setDetailsOpen(e.open)}
        width="100%"
      >
        <Collapsible.Trigger
          fontSize="sm"
          fontWeight="medium"
          color="fg.muted"
          cursor="pointer"
          _hover={{ color: "fg" }}
        >
          {expanded ? "Hide options" : "Add a reason or replace the input"}
        </Collapsible.Trigger>

        <Collapsible.Content>
          <Stack gap={4} paddingTop={4}>
            <Field.Root invalid={!!errors.reason}>
              <Field.Label>Reason (optional, signed with the decision)</Field.Label>
              <Input {...register("reason")} placeholder="why you are allowing or denying this" />
              <Field.ErrorText>{errors.reason?.message}</Field.ErrorText>
            </Field.Root>

            <Field.Root invalid={!!errors.updatedInput}>
              <Field.Label>Replacement input (optional, allow only)</Field.Label>
              <Textarea
                {...register("updatedInput")}
                rows={3}
                fontFamily="mono"
                fontSize="sm"
                placeholder={'{"command": "npm ci"}'}
              />
              <Field.ErrorText>{errors.updatedInput?.message}</Field.ErrorText>
              <Field.HelperText>
                Leave empty to run the tool with its original input.
              </Field.HelperText>
            </Field.Root>
          </Stack>
        </Collapsible.Content>
      </Collapsible.Root>

      {failure ? (
        <Text fontSize="sm" color="red.fg">
          {failure}
        </Text>
      ) : null}

      <Stack gap={3}>
        <HStack gap={3} wrap="wrap">
          <Button
            colorPalette="brand"
            size="lg"
            minW="32"
            loading={isSubmitting}
            onClick={submit("allow")}
          >
            Allow
          </Button>
          <Button
            colorPalette="red"
            size="lg"
            minW="32"
            loading={isSubmitting}
            onClick={submit("deny")}
          >
            Deny
          </Button>
        </HStack>
        <Text fontSize="xs" color="fg.subtle">
          Doing nothing sends no reply — Claude Code then asks in its own terminal.
        </Text>
      </Stack>
    </Stack>
  );
}
