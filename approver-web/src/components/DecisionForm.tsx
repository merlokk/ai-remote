"use client";

/**
 * allow / deny for one request.
 *
 * There is no "skip" button on purpose: skipping is *not replying*, which is
 * what happens if you do nothing until the countdown runs out. Inventing a
 * button for it would suggest something is sent.
 */
import { Button, Field, HStack, Input, Stack, Text, Textarea } from "@chakra-ui/react";
import { zodResolver } from "@hookform/resolvers/zod";
import { useState } from "react";
import { useForm } from "react-hook-form";

import { type Behavior, type DecisionFormValues, decisionFormSchema } from "@/lib/schemas";
import type { DecisionResult } from "@/lib/types";

export function DecisionForm({ nonce }: { nonce: string }) {
  const [sent, setSent] = useState<{ behavior: Behavior; result: DecisionResult } | null>(null);
  const [failure, setFailure] = useState<string | null>(null);

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
        sent <strong>{sent.behavior}</strong>
        {sent.result.signed ? " (signed)" : " — unsigned, the hook will reject it"}
      </Text>
    );
  }

  return (
    <Stack gap={3} width="100%">
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

      {failure ? (
        <Text fontSize="sm" color="red.fg">
          {failure}
        </Text>
      ) : null}

      <HStack gap={3}>
        <Button colorPalette="green" loading={isSubmitting} onClick={submit("allow")}>
          Allow
        </Button>
        <Button colorPalette="red" variant="outline" loading={isSubmitting} onClick={submit("deny")}>
          Deny
        </Button>
        <Text fontSize="xs" color="fg.muted">
          doing nothing = no reply = Claude Code asks locally
        </Text>
      </HStack>
    </Stack>
  );
}
