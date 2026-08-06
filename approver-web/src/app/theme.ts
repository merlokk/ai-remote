/**
 * The project's Chakra system — the whole visual direction lives here.
 *
 * See CLAUDE.md, "Look and feel": soft rounded panels, one green, calm
 * background with raised cards. The rule that shapes this file is that none of
 * it may leak into components as one-off props — a component asking for
 * `borderRadius="16px"` is a bug, because the radius is the signature and has to
 * stay consistent.
 *
 * Three levers do all the work:
 *
 *  1. **Semantic radii.** Chakra's recipes never hardcode a radius: `Card` uses
 *     `l3`, and `Button` / `Input` / `Textarea` / `Badge` use `l2` (verified in
 *     `@chakra-ui/react` 3.36.1). Remapping l1/l2/l3 rounds the entire UI at
 *     once, including components nobody has used yet.
 *  2. **A `brand` palette.** A full scale plus the eight semantic entries every
 *     Chakra palette needs (`solid`, `fg`, `subtle`, …) — without them
 *     `colorPalette="brand"` would render unstyled.
 *  3. **Recipe defaults.** Cards default to `elevated` (panel + shadow) instead
 *     of `outline`, so the heavy 1px grey boxes disappear repo-wide rather than
 *     component by component.
 *
 * What is deliberately *not* here: a global `colorPalette: "brand"`. Green is
 * the accent, not the paint — and on a page whose job is allow/deny, making
 * every control green by default is exactly how the two answers stop being
 * distinguishable.
 */
import { createSystem, defaultConfig, defineConfig } from "@chakra-ui/react";
import { cardAnatomy } from "@chakra-ui/react/anatomy";

const config = defineConfig({
  globalCss: {
    "html, body": {
      // The page itself is green; the cards on top of it stay bg.panel (white),
      // which is what gives them their lift. Tune the tint in `bg.canvas` below
      // rather than here.
      bg: "bg.canvas",
    },
  },
  theme: {
    tokens: {
      colors: {
        // One green, a little deeper and less neon than Chakra's built-in
        // `green`, which stays available for success/error semantics.
        brand: {
          50: { value: "#f2faf5" },
          100: { value: "#dff3e7" },
          200: { value: "#bde6cd" },
          300: { value: "#93d5ae" },
          400: { value: "#63bd8b" },
          500: { value: "#3aa06c" },
          600: { value: "#2a8256" },
          700: { value: "#226845" },
          800: { value: "#1c5137" },
          900: { value: "#15382a" },
          950: { value: "#0b2019" },
        },
      },
    },
    semanticTokens: {
      radii: {
        // Defaults are xs/sm/md (0.125 / 0.25 / 0.375rem) — barely rounded.
        l1: { value: "{radii.lg}" }, // 0.5rem  — small controls
        l2: { value: "{radii.2xl}" }, // 1rem   — buttons, inputs, badges
        l3: { value: "{radii.3xl}" }, // 1.5rem — cards and panels
      },
      colors: {
        bg: {
          // The page background — a light lettuce green, leaning a touch
          // warmer/yellower than the brand ramp so the white cards and the
          // deeper `brand.solid` buttons both stay legible against it.
          // One token: this is the only place the page tint is set.
          canvas: { value: { _light: "#e9f7dd", _dark: "{colors.brand.950}" } },
        },
        // Mirrors the shape Chakra defines for every built-in palette.
        brand: {
          contrast: { value: { _light: "white", _dark: "white" } },
          fg: { value: { _light: "{colors.brand.700}", _dark: "{colors.brand.300}" } },
          subtle: { value: { _light: "{colors.brand.100}", _dark: "{colors.brand.900}" } },
          muted: { value: { _light: "{colors.brand.200}", _dark: "{colors.brand.800}" } },
          emphasized: { value: { _light: "{colors.brand.300}", _dark: "{colors.brand.700}" } },
          // 4.7:1 against white — white button labels stay AA-legible.
          solid: { value: { _light: "{colors.brand.600}", _dark: "{colors.brand.600}" } },
          focusRing: { value: { _light: "{colors.brand.500}", _dark: "{colors.brand.500}" } },
          border: { value: { _light: "{colors.brand.500}", _dark: "{colors.brand.400}" } },
        },
      },
    },
    recipes: {
      badge: {
        // Pills, per the direction. l2 is already 1rem; `full` makes it explicit
        // and keeps small badges from looking like tiny rounded rectangles.
        base: { borderRadius: "full" },
      },
    },
    slotRecipes: {
      card: {
        // Raised panels instead of outlined boxes. `slots` is required even for
        // a defaults-only override, hence the anatomy import.
        slots: cardAnatomy.keys(),
        defaultVariants: { variant: "elevated" },
      },
    },
  },
});

export const system = createSystem(defaultConfig, config);
