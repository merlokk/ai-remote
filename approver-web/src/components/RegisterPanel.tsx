"use client";

/**
 * Registering this browser as a responder (§6).
 *
 * Collapsed once this browser holds a key — rotation is rare — and open while
 * it does not, because until then every decision it sends is rejected.
 *
 * The key pair is generated **here**, non-extractable, and only the public half
 * is ever sent (see `browser-key.ts`). The token is a bearer credential for one
 * `key_id`, so it is not kept in component state after the request.
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

import { useBrowserKey } from "@/lib/browser-key-context";
import { type RegisterFormValues, registerFormSchema } from "@/lib/schemas";
import type { RegisterResult, ResponderStatus } from "@/lib/types";

export function RegisterPanel({ status }: { status: ResponderStatus | null }) {
  const { key, ready, register: registerKey } = useBrowserKey();
  const registered = key !== null;
  // A key here that the allowlist no longer knows: someone rotated this key_id
  // elsewhere, and every decision would be rejected with no visible reason.
  const stale = key !== null && status?.public_key != null && status.public_key !== key.public_key;
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

  // Open by default until this browser has a usable key; after that the
  // operator decides — except a stale key, which needs attention.
  const expanded = open ?? (!registered || stale);

  const onSubmit = handleSubmit(async (values) => {
    setResult(null);
    try {
      const body = await registerKey(values.token);
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
              <Text fontSize="sm" color={stale ? "red.fg" : "fg.muted"}>
                {!ready
                  ? "checking this browser…"
                  : stale
                    ? "this browser's key is not the registered one"
                    : registered
                      ? `${key?.key_id} — key held by this browser`
                      : "no key in this browser"}
              </Text>
            </HStack>
          </Collapsible.Trigger>

          <Collapsible.Content>
            <Stack gap={4} paddingTop={4}>
              <Text fontSize="sm" color="fg.muted">
                Registers <em>this browser</em> as a responder. The P-256 key is generated
                here and marked non-extractable — the private half never leaves the browser,
                not even to this app&apos;s own server, and it survives closing the browser.
                Only the public half is published, with your one-time token. Mint one with{" "}
                <Code size="sm">
                  py approver/registration_handler.py --get-token approver-web
                </Code>
                .
              </Text>

              {stale ? (
                <Text fontSize="sm" color="red.fg">
                  The allowlist holds a different key for {status?.key_id}. Decisions from
                  this browser will be rejected until you register again.
                </Text>
              ) : null}

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

              {key ? (
                <Text fontSize="xs" color="fg.subtle" fontFamily="mono" wordBreak="break-all">
                  public key {key.public_key}
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
