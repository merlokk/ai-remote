import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // `next dev` otherwise appends a generated block to CLAUDE.md on every run.
  // This repo's CLAUDE.md files are hand-written contracts; the one useful thing
  // that block said is kept, in our own words, in the "Stack" section.
  agentRules: false,
  // The NATS client is a plain Node client (node:net / node:tls). Keep it out of
  // the bundler and let Node load it at runtime.
  serverExternalPackages: ["@nats-io/transport-node"],
};

export default nextConfig;
