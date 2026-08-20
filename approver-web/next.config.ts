import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // `next dev` otherwise appends a generated block to CLAUDE.md on every run.
  // This repo's CLAUDE.md files are hand-written contracts; the one useful thing
  // that block said is kept, in our own words, in the "Stack" section.
  agentRules: false,
  // The NATS client is a plain Node client (node:net / node:tls). Keep it out of
  // the bundler and let Node load it at runtime.
  serverExternalPackages: ["@nats-io/transport-node"],
  // Which hosts `next dev` will serve its own client chunks and HMR socket to.
  //
  // Without this the dev server answers only the origin it prints — `localhost`
  // — and `http://127.0.0.1:3000/` is a *different* origin by that rule: the page
  // arrives server-rendered and looks right, never hydrates, and every live part
  // of it (the cards, the buttons, the register panel) sits in its loading state
  // forever. The only clue is a `Blocked cross-origin request to Next.js dev
  // resource` line in this server's own output, which nobody is reading.
  //
  // So both spellings of loopback are here, and the private ranges with them: a
  // second responder on a phone (approver-web-phone, `CLAUDE.md` "Registering
  // more than one browser") reaches this machine by LAN address, not by name.
  //
  // **Dev only, and it opens nothing.** It gates who may fetch a chunk, not who
  // may reach the page: `next dev` binds every interface either way, and the page
  // has no authentication (see the last section of `CLAUDE.md`). Whether this may
  // be reachable off this machine at all is that decision, not this list.
  allowedDevOrigins: [
    "localhost",
    "127.0.0.1",
    "[::1]",
    "10.*.*.*",
    "172.*.*.*",
    "192.168.*.*",
  ],
};

export default nextConfig;
