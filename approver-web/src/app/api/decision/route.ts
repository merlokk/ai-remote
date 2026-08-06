/**
 * POST /api/decision — sign an allow/deny and answer the waiting hook.
 *
 * 409 means the request is no longer answerable (expired, or another tab got
 * there first). That is not an error worth retrying: the hook has already
 * fallen back to its own prompt.
 */
import { DecisionError, getResponder } from "@/lib/responder";
import { decisionRequestSchema } from "@/lib/schemas";
import type { DecisionResult } from "@/lib/types";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

function json(body: DecisionResult, status: number): Response {
  return Response.json(body, { status });
}

export async function POST(request: Request): Promise<Response> {
  let payload: unknown;
  try {
    payload = await request.json();
  } catch {
    return json({ ok: false, error: "body is not JSON" }, 400);
  }

  const parsed = decisionRequestSchema.safeParse(payload);
  if (!parsed.success) {
    const detail = parsed.error.issues
      .map((i) => `${i.path.join(".") || "body"}: ${i.message}`)
      .join("; ");
    return json({ ok: false, error: detail }, 400);
  }

  const { nonce, behavior, reason, updated_input, sig } = parsed.data;
  try {
    await getResponder().decide(
      nonce,
      { behavior, reason, updatedInput: updated_input ?? null },
      sig,
    );
    return json({ ok: true }, 200);
  } catch (err) {
    if (err instanceof DecisionError) return json({ ok: false, error: err.message }, 409);
    // Nothing was sent. The Python responders stay silent here on purpose (§7):
    // no reply, hook falls back. Report it, do not guess a verdict.
    const message = err instanceof Error ? err.message : String(err);
    console.error("[approver-web] could not answer:", message);
    return json({ ok: false, error: `could not answer: ${message}` }, 500);
  }
}
