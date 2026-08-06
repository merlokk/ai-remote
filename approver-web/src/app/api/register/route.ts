/**
 * POST /api/register — register this app's key with a one-time token (§6).
 *
 * The token never leaves the server beyond the `registrations` subject, and the
 * generated private key never leaves the server at all. A rejection (bad or
 * spent token, no handler listening) leaves any existing key untouched — 409,
 * because it is the request that is wrong, not the server.
 */
import { getResponder, RegistrationError } from "@/lib/responder";
import { registerRequestSchema } from "@/lib/schemas";
import type { RegisterResult } from "@/lib/types";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

function json(body: RegisterResult, status: number): Response {
  return Response.json(body, { status });
}

export async function POST(request: Request): Promise<Response> {
  let payload: unknown;
  try {
    payload = await request.json();
  } catch {
    return json({ ok: false, error: "body is not JSON" }, 400);
  }

  const parsed = registerRequestSchema.safeParse(payload);
  if (!parsed.success) {
    return json({ ok: false, error: parsed.error.issues[0]?.message ?? "bad token" }, 400);
  }

  try {
    const keyId = await getResponder().register(parsed.data.token);
    return json({ ok: true, key_id: keyId }, 200);
  } catch (err) {
    if (err instanceof RegistrationError) return json({ ok: false, error: err.message }, 409);
    const message = err instanceof Error ? err.message : String(err);
    console.error("[approver-web] registration failed:", message);
    return json({ ok: false, error: message }, 500);
  }
}
