"use client";

/**
 * Registering this app as a responder (§6).
 *
 * Collapsed once a key exists — rotation is rare — and open while there is
 * none, because until then every decision this app sends is rejected.
 *
 * The token is a bearer credential for one `key_id`, so it goes straight to the
 * server and is never kept in component state after the request.
 */
import {
  Button,
  Card,
  Code,
  Collapsible,
  Field,
  HStack,
  Input,
  Stack,
  Text,
} from "@chakra-ui/react";
import { zodResolver } from "@hookform/resolvers/zod";
import { useState } from "react";
import { useForm } from "react-hook-form";

import { type RegisterFormValues, registerFormSchema } from "@/lib/schemas";
import type { RegisterResult, ResponderStatus } from "@/lib/types";

export function RegisterPanel({ status }: { status: ResponderStatus | null }) {
  const registered = status?.registered ?? false;
  const [open, setOpen] = useState<boolean | null>(null);
  const [result, setResult] = useState<RegisterResult | null>(null);

  const {
    register,
    handleSubmit,
    reset,
    formState: { errors, isSubmitting },
  } = useForm<RegisterFormValues>({
    resolver: zodResolver(registerFormSchema),
    defaultValues: { token: "" },
  });

  // Open by default until a key exists; after that the operator decides.
  const expanded = open ?? !registered;

  const onSubmit = handleSubmit(async (values) => {
    setResult(null);
    try {
      const response = await fetch("/api/register", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ token: values.token }),
      });
      const body = (await response.json()) as RegisterResult;
      setResult(body);
      if (body.ok) reset({ token: "" }); // don't leave a spent token on screen
    } catch (err) {
      setResult({ ok: false, error: err instanceof Error ? err.message : String(err) });
    }
  });

  return (
    <Card.Root>
      <Card.Body>
        <Collapsible.Root open={expanded} onOpenChange={(e) => setOpen(e.open)}>
          <Collapsible.Trigger width="100%" cursor="pointer">
            <HStack justify="space-between" width="100%">
              <Text fontWeight="semibold">Register</Text>
              <Text fontSize="sm" color="fg.muted">
                {registered ? `${status?.key_id} — registered` : "no key yet"}
              </Text>
            </HStack>
          </Collapsible.Trigger>

          <Collapsible.Content>
            <Stack gap={4} paddingTop={4}>
              <Text fontSize="sm" color="fg.muted">
                Registers this app as a responder: it generates a P-256 key, publishes the
                public half with your one-time token, and stores the pair only if the handler
                accepts it. Mint a token with{" "}
                <Code size="sm">
                  py approver/registration_handler.py --get-token approver-web
                </Code>
                .
              </Text>

              <Field.Root invalid={!!errors.token}>
                <Field.Label>One-time token</Field.Label>
                <Input
                  {...register("token")}
                  fontFamily="mono"
                  placeholder="approver-web.<secret>"
                  autoComplete="off"
                  spellCheck={false}
                />
                <Field.ErrorText>{errors.token?.message}</Field.ErrorText>
                <Field.HelperText>
                  The token names the key_id it may register — it cannot claim another one.
                </Field.HelperText>
              </Field.Root>

              {result ? (
                <Text fontSize="sm" color={result.ok ? "brand.fg" : "red.fg"}>
                  {result.ok
                    ? `registered as ${result.key_id} — decisions from this tab are now signed`
                    : result.error}
                </Text>
              ) : null}

              {registered && status?.public_key ? (
                <Text fontSize="xs" color="fg.subtle" fontFamily="mono" wordBreak="break-all">
                  public key {status.public_key}
                </Text>
              ) : null}

              <HStack>
                <Button colorPalette="brand" loading={isSubmitting} onClick={onSubmit}>
                  {registered ? "Rotate key" : "Register"}
                </Button>
              </HStack>
            </Stack>
          </Collapsible.Content>
        </Collapsible.Root>
      </Card.Body>
    </Card.Root>
  );
}
