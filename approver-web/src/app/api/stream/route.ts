/**
 * GET /api/stream — server-sent events carrying the whole snapshot.
 *
 * Sending the full {status, requests} on every change instead of deltas: the
 * list is a handful of entries, and a reconnecting browser is then correct
 * immediately with no replay logic.
 *
 * Opening the stream is also what starts the NATS connection — the app has no
 * "connect" button because having the page open *is* being a responder.
 */
import { getResponder } from "@/lib/responder";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const HEARTBEAT_MS = 15_000;

export async function GET(request: Request): Promise<Response> {
  const responder = getResponder();
  void responder.start(); // don't hold the response open on the TCP connect

  const encoder = new TextEncoder();

  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      const send = (data: unknown) => {
        try {
          controller.enqueue(encoder.encode(`data: ${JSON.stringify(data)}\n\n`));
        } catch {
          // Client vanished between the abort and our write; cleanup is below.
        }
      };

      send(responder.snapshot());
      const unsubscribe = responder.subscribe(send);

      // Comment frames keep proxies from closing an idle stream.
      const heartbeat = setInterval(() => {
        try {
          controller.enqueue(encoder.encode(": ping\n\n"));
        } catch {
          /* same as above */
        }
      }, HEARTBEAT_MS);

      const close = () => {
        clearInterval(heartbeat);
        unsubscribe();
        try {
          controller.close();
        } catch {
          /* already closed */
        }
      };

      if (request.signal.aborted) close();
      else request.signal.addEventListener("abort", close, { once: true });
    },
  });

  return new Response(stream, {
    headers: {
      "content-type": "text/event-stream; charset=utf-8",
      "cache-control": "no-store, no-transform",
      connection: "keep-alive",
    },
  });
}
