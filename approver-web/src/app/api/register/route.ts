/**
 * POST /api/register — register the browser's key with a one-time token (§6).
 *
 * The browser generates the pair and sends only public material; there is no
 * private key on this side to leak. The token goes no further than the
 * `registrations` subject. A rejection (bad or spent token, no handler
 * listening) leaves any existing key untouched — 409, because it is the request
 * that is wrong, not the server.
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

  const { token, public_key, public_key_raw } = parsed.data;
  try {
    const keyId = await getResponder().register(token, public_key, public_key_raw);
    return json({ ok: true, key_id: keyId }, 200);
  } catch (err) {
    if (err instanceof RegistrationError) return json({ ok: false, error: err.message }, 409);
    const message = err instanceof Error ? err.message : String(err);
    console.error("[approver-web] registration failed:", message);
    return json({ ok: false, error: message }, 500);
  }
}
