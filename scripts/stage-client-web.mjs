#!/usr/bin/env node
// Stage the Homeworldz client's web export into the management site's public
// tree, so one Cloudflare Pages project serves both from one origin (operator,
// 2026-08-22). Same origin is what lets the client read the site's stored
// token instead of needing a sign-in handoff.
//
// Vite copies web/public/ verbatim into dist/, so web/public/app/index.html
// becomes dist/app/index.html, which is what web/public/_redirects already
// routes. Nothing else has to know about this.
//
// The client repository is a separate project and is only ever READ here.
//
// This exists rather than a bare copy because Cloudflare Pages refuses any
// single asset over 25 MiB, and a rejection at deploy time names the limit but
// not the cause. Better to refuse locally, before anything is committed.

import { existsSync, mkdirSync, readdirSync, rmSync, statSync, copyFileSync } from "node:fs";
import { join, relative, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const source = process.argv[2] ?? join(root, "..", "client", "build-web");
const target = join(root, "web", "public", "app");

// developers.cloudflare.com/pages/platform/limits — "The maximum file size for
// a single Cloudflare Pages site asset is 25 MiB."
const PAGES_MAX_ASSET = 25 * 1024 * 1024;
const MiB = (bytes) => (bytes / 1024 / 1024).toFixed(1) + " MiB";

// Why an oversized asset here is not something the client can fix by building
// differently, recorded because the obvious guess is wrong and was guessed
// once already: index.side.wasm is Godot's ENGINE, not the extension. It
// carries no debug sections and matches the engine in the official release
// template byte for byte, so there is nothing to strip. A plain Godot 4.7.1
// nothreads export is 37 MiB, meaning Pages cannot host ANY Godot web export
// as-is, and Workers static assets share the same 25 MiB limit. R2 does not
// (5 TiB per object), so serving /app from R2 behind a Worker route on THIS
// hostname moves only the storage and leaves the origin — and so the shared
// token — intact. A custom engine template with unused modules disabled is
// the other route; the shipped engine contains NavigationServer, OpenXR and
// ufbx, none of which a client needs.
const OVERSIZED_ADVICE = [
  "If that is index.side.wasm, it is Godot's ENGINE, not the extension, and",
  "there is nothing to strip: no debug sections, and byte-identical to the",
  "engine in the official release template. A plain Godot 4.7.1 nothreads",
  "export is 37 MiB, so Pages cannot host ANY Godot web export as-is, and",
  "Workers static assets share the same 25 MiB limit. R2 does not (5 TiB per",
  "object): serving /app from R2 behind a Worker route on THIS hostname moves",
  "only the storage, leaving the origin and the shared token intact. A custom",
  "engine template with unused modules disabled is the other route.",
];

if (!existsSync(source)) {
  console.error(`[stage-client-web] no client export at ${source}`);
  console.error("[stage-client-web] build it in the client repository first, or pass a path.");
  process.exit(1);
}

const files = [];
(function walk(dir) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) walk(full);
    else if (entry.isFile()) files.push(full);
  }
})(source);

if (files.length === 0) {
  console.error(`[stage-client-web] ${source} is empty`);
  process.exit(1);
}

const oversized = files
  .map((file) => ({ file, size: statSync(file).size }))
  .filter((entry) => entry.size > PAGES_MAX_ASSET)
  .sort((a, b) => b.size - a.size);

if (oversized.length > 0) {
  console.error(
    `[stage-client-web] REFUSED: ${oversized.length} file(s) exceed Cloudflare Pages' ` +
      `${MiB(PAGES_MAX_ASSET)} per-asset limit, so this deployment would be rejected:`,
  );
  for (const { file, size } of oversized) {
    console.error(`  ${MiB(size).padStart(10)}  ${relative(source, file)}`);
  }
  for (const line of OVERSIZED_ADVICE) console.error(`[stage-client-web] ${line}`);
  process.exit(2);
}

rmSync(target, { recursive: true, force: true });
let total = 0;
for (const file of files) {
  const destination = join(target, relative(source, file));
  mkdirSync(dirname(destination), { recursive: true });
  copyFileSync(file, destination);
  total += statSync(file).size;
}

console.log(
  `[stage-client-web] staged ${files.length} file(s), ${MiB(total)} into ` +
    `${relative(root, target)} — served at /app/ after pnpm build`,
);
