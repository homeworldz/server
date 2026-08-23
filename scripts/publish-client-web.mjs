#!/usr/bin/env node
// Upload the Homeworldz client's web export to the R2 bucket the app Worker
// serves at my.homeworldz.com/app/* (see deploy/cloudflare/app-worker).
//
// Replaces the earlier stage-into-Pages script: Cloudflare Pages caps a single
// asset at 25 MiB and Godot's engine wasm is 42 MiB, so the export cannot live
// in the Pages project at all. R2 allows 5 TiB per object.
//
// Content types are set per file rather than left to the uploader. A .wasm
// stored as application/octet-stream is refused by
// WebAssembly.instantiateStreaming, and that failure reads as a broken build
// rather than a missing header. The Worker also sets the type on the way out,
// so this is the belt to its braces.
//
// The client repository is a separate project and is only ever READ here.

import { existsSync, readdirSync, statSync } from "node:fs";
import { join, relative, dirname, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const args = process.argv.slice(2);
// First non-flag argument, so --dry-run is not mistaken for a path.
const source = args.find((a) => !a.startsWith("-")) ?? join(root, "..", "client", "build-web");
const bucket = process.env.HOMEWORLDZ_CLIENT_BUCKET ?? "homeworldz-client-web";
const prefix = (process.env.HOMEWORLDZ_CLIENT_PREFIX ?? "").replace(/^\/+|\/+$/g, "");
const dryRun = args.includes("--dry-run");

const CONTENT_TYPES = new Map(Object.entries({
  wasm: "application/wasm",
  js: "text/javascript",
  html: "text/html",
  json: "application/json",
  css: "text/css",
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  svg: "image/svg+xml",
  webp: "image/webp",
  ico: "image/vnd.microsoft.icon",
  wav: "audio/wav",
  ogg: "audio/ogg",
}));
const typeFor = (key) =>
  CONTENT_TYPES.get(key.slice(key.lastIndexOf(".") + 1).toLowerCase()) ??
  "application/octet-stream";
// This runs through a shell because it has to: Windows will not spawn
// npx.cmd directly (Node refuses .cmd without a shell), and a shell re-splits
// whatever it is handed. Unquoted, a value containing a space or a semicolon
// becomes two arguments — wrangler reported `Unknown argument: charset=utf-8`
// on exactly that, and any path with a space would break identically.
//
// The command is assembled as one already-quoted string rather than passed as
// an args array with shell:true, which Node deprecates (DEP0190) for this very
// reason: with shell:true the args are concatenated, not escaped. Content types
// here also carry no "; charset=", since the Worker sets the outgoing type
// authoritatively and the stored one is only a fallback — so the awkward value
// never reaches a command line at all.
const quote = (value) =>
  process.platform === "win32"
    ? `"${value}"`
    : `'${String(value).replace(/'/g, "'\''")}'`;

const MiB = (bytes) => (bytes / 1024 / 1024).toFixed(1) + " MiB";

if (!existsSync(source)) {
  console.error(`[publish-client-web] no client export at ${source}`);
  console.error("[publish-client-web] build it in the client repository first, or pass a path.");
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

// An export missing its entry document or engine uploads "successfully" and
// then fails in the browser with nothing naming the cause.
const names = new Set(files.map((f) => relative(source, f).split(sep).join("/")));
const required = ["index.html", "index.js", "index.wasm"];
const missing = required.filter((name) => !names.has(name));
if (missing.length > 0) {
  console.error(`[publish-client-web] ${source} does not look like a Godot web export.`);
  console.error(`[publish-client-web] missing: ${missing.join(", ")}`);
  process.exit(1);
}

let uploaded = 0;
let total = 0;
for (const file of files) {
  const relativeKey = relative(source, file).split(sep).join("/");
  const key = prefix ? `${prefix}/${relativeKey}` : relativeKey;
  const size = statSync(file).size;
  const type = typeFor(key);
  total += size;

  if (dryRun) {
    console.log(`[publish-client-web] would put ${key} (${MiB(size)}, ${type})`);
    continue;
  }

  const command = [
    "npx", "--yes", "wrangler", "r2", "object", "put", quote(`${bucket}/${key}`),
    quote(`--file=${file}`), quote(`--content-type=${type}`), "--remote",
  ].join(" ");
  const result = spawnSync(command, {
    stdio: ["ignore", "inherit", "inherit"],
    shell: true,
  });
  if (result.error || result.status !== 0) {
    console.error(`[publish-client-web] FAILED on ${key}`);
    if (result.error) console.error(`[publish-client-web] ${result.error.message}`);
    console.error("[publish-client-web] Is wrangler authenticated? Try: npx wrangler whoami");
    process.exit(result.status ?? 1);
  }
  ++uploaded;
  console.log(`[publish-client-web] put ${key} (${MiB(size)}, ${type})`);
}

console.log(
  dryRun
    ? `[publish-client-web] dry run: ${files.length} file(s), ${MiB(total)} to ${bucket}`
    : `[publish-client-web] uploaded ${uploaded} file(s), ${MiB(total)} to ${bucket}`,
);
