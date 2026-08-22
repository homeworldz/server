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
  .map((f) => ({ file: f, size: statSync(f).size }))
  .filter((f) => f.size > PAGES_MAX_ASSET)
  .sort((a, b) => b.size - a.size);

if (oversized.length > 0) {
  console.error(
    `[stage-client-web] REFUSED: ${oversized.length} file(s) exceed Cloudflare Pages' ` +
      `${MiB(PAGES_MAX_ASSET)} per-asset limit, so this deployment would be rejected:`,
  );
  for (const { file, size } of oversized) {
    console.error(`  ${MiB(size).padStart(10)}  ${relative(source, file)}`);
  }
  console.error(
    "[stage-client-web] A wasm module this size is usually unstripped debug info.\n" +
      "[stage-client-web] Strip it in the client's export (wasm-opt --strip-debug\n" +
      "[stage-client-web] --strip-dwarf, or a release build without -g) rather than\n" +
      "[stage-client-web] splitting it across origins — cross-origin wasm needs CORS\n" +
      "[stage-client-web] and the correct MIME type, and loses the same-origin property\n" +
      "[stage-client-web] this arrangement exists for.",
  );
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
