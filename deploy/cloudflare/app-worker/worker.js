// Serves the Homeworldz client at my.homeworldz.com/app/* out of R2.
//
// Why a Worker rather than the Pages project: Cloudflare Pages, Workers static
// assets, and Workers KV all cap a single asset at 25 MiB, and Godot's engine
// wasm is 42 MiB (37 MiB even for a plain export), so no Godot web export can
// live in any of them. R2 allows 5 TiB per object. Only the storage moves —
// the route is on the management site's own hostname, so the browser origin
// stays my.homeworldz.com and the client keeps reading the token the site
// stored under localStorage["homeworldz.auth"]. That shared origin is the
// whole reason there is no sign-in handoff to build.
//
// A route is more specific than the hostname the Pages project serves, so this
// Worker wins for /app/* and Pages continues to serve everything else.

// Content types are decided HERE rather than trusted from R2 metadata: an
// object uploaded without a type would otherwise arrive as
// application/octet-stream, and WebAssembly.instantiateStreaming refuses
// anything that is not application/wasm — a failure that looks like a broken
// build rather than a missing header.
const CONTENT_TYPES = new Map(Object.entries({
  wasm: "application/wasm",
  js: "text/javascript; charset=utf-8",
  html: "text/html; charset=utf-8",
  json: "application/json; charset=utf-8",
  css: "text/css; charset=utf-8",
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  svg: "image/svg+xml",
  webp: "image/webp",
  ico: "image/vnd.microsoft.icon",
  wav: "audio/wav",
  ogg: "audio/ogg",
  pck: "application/octet-stream",
  data: "application/octet-stream",
  worker: "text/javascript; charset=utf-8",
}));

function contentTypeFor(key) {
  const extension = key.slice(key.lastIndexOf(".") + 1).toLowerCase();
  return CONTENT_TYPES.get(extension) ?? "application/octet-stream";
}

// Godot's export writes stable filenames (index.wasm, index.side.wasm), so
// content changes under an unchanged name and `immutable` would pin a stale
// engine in every browser that had loaded it. Revalidation against R2's ETag
// is correct instead: a 304 is one small round trip and the 42 MiB body stays
// cached. Set CACHE_IMMUTABLE=true only once uploads go to a versioned prefix,
// where a name really cannot change meaning.
function cacheControl(env) {
  return env.CACHE_IMMUTABLE === "true"
    ? "public, max-age=31536000, immutable"
    : "public, max-age=0, must-revalidate";
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method !== "GET" && request.method !== "HEAD") {
      return new Response("method not allowed", {
        status: 405,
        headers: { allow: "GET, HEAD" },
      });
    }

    // /app and /app/ both mean the export's entry document.
    let key = url.pathname.replace(/^\/app\/?/, "");
    if (key === "") key = "index.html";
    // No traversal out of the prefix, and no directory listings.
    if (key.includes("..")) return new Response("not found", { status: 404 });
    if (env.KEY_PREFIX) key = `${env.KEY_PREFIX.replace(/\/$/, "")}/${key}`;

    const conditional = {
      etagMatches: request.headers.get("if-none-match") ?? undefined,
      etagDoesNotMatch: request.headers.get("if-match") ?? undefined,
    };
    const range = request.headers.get("range") ? request.headers : undefined;

    const object = await env.CLIENT_BUCKET.get(key, {
      onlyIf: conditional.etagMatches ? { etagMatches: conditional.etagMatches } : undefined,
      range,
    });

    if (object === null) return new Response("not found", { status: 404 });

    const headers = new Headers();
    // writeHttpMetadata first so anything stored on the object is present,
    // then the type is overridden below because it is the one header that must
    // not be left to whatever the upload happened to set.
    object.writeHttpMetadata(headers);
    headers.set("content-type", contentTypeFor(key));
    headers.set("etag", object.httpEtag);
    headers.set("cache-control", cacheControl(env));
    headers.set("accept-ranges", "bytes");
    // A browser reading this page has already been served by the same origin,
    // so no CORS headers are needed or wanted here.
    headers.set("x-content-type-options", "nosniff");

    // `body` is absent when the conditional matched: that is the 304.
    if (!("body" in object) || object.body === null) {
      return new Response(null, { status: 304, headers });
    }

    if (object.range && "offset" in object.range) {
      const start = object.range.offset ?? 0;
      const length = object.range.length ?? object.size - start;
      headers.set("content-range", `bytes ${start}-${start + length - 1}/${object.size}`);
      return new Response(request.method === "HEAD" ? null : object.body, {
        status: 206,
        headers,
      });
    }

    return new Response(request.method === "HEAD" ? null : object.body, { headers });
  },
};
