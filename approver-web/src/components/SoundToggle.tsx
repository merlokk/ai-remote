"use client";

import { Button } from "@chakra-ui/react";

import type { SoundState } from "@/lib/use-request-alert";

const LABEL: Record<SoundState, string> = {
  on: "Sound on",
  off: "Sound off",
  // The preference is on, but the browser has not had a gesture yet — clicking
  // this button *is* the gesture, so say what to do rather than what is wrong.
  blocked: "Sound — click to enable",
  unsupported: "Sound unavailable",
};

export function SoundToggle({
  sound,
  onToggle,
}: {
  sound: SoundState;
  onToggle: () => void;
}) {
  return (
    <Button
      size="xs"
      variant={sound === "on" ? "subtle" : "outline"}
      colorPalette={sound === "on" ? "brand" : sound === "blocked" ? "orange" : "gray"}
      disabled={sound === "unsupported"}
      onClick={onToggle}
      aria-pressed={sound === "on"}
    >
      {LABEL[sound]}
    </Button>
  );
}
