// Bundles src/loader.js with @decky/ui into dist/pbos-qam.js, one script
// that pbos-qamd evaluates in Steam. @decky/ui is required (not imported)
// inside the loader, so it initializes only after Steam is up and React is
// found; esbuild keeps such modules lazy.
import { build } from "esbuild";
import { readFileSync } from "node:fs";

const ui = JSON.parse(readFileSync("node_modules/@decky/ui/package.json", "utf8"));
await build({
  entryPoints: ["src/loader.js"],
  outfile: "dist/pbos-qam.js",
  bundle: true,
  format: "iife",
  platform: "browser",
  target: "chrome100",
  minify: true,
  keepNames: true,
  legalComments: "inline",
  alias: {
    react: "./shims/react.js",
    "react-dom": "./shims/react-dom.js",
    "react/jsx-runtime": "./shims/jsx-runtime.js",
  },
  banner: {
    js: `/* PB-OS Quick Access loader (pb-os, external-and-mods/pbos-qam/frontend). Bundles @decky/ui ${ui.version} (SteamDeckHomebrew/decky-frontend-lib, LGPL-2.1: https://github.com/SteamDeckHomebrew/decky-frontend-lib). */`,
  },
  logLevel: "info",
});
