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
// Confirmed live 2026-08-22: /app/ returns the client, /login still returns
// the management site.

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
}));

function contentTypeFor(key) {
  const extension = key.slice(key.lastIndexOf(".") + 1).toLowerCase();
  return CONTENT_TYPES.get(extension) ?? "application/octet-stream";
}

// Godot's export writes stable filenames (index.wasm, index.side.wasm), so
// content changes under an unchanged name and `immutable` would pin a stale
// engine in every browser that had loaded it. Revalidating against R2's ETag
// is correct instead: a 304 is one small round trip and the 42 MiB body stays
// cached. Set CACHE_IMMUTABLE=true only once uploads go to a versioned prefix,
// where a name really cannot change meaning.
// Measured live 2026-08-22: Cloudflare STRIPS the ETag from responses it
// compresses, so text/html and text/javascript arrive with no validator and
// cannot revalidate — with max-age=0 they refetch in full every load.
// application/wasm is not compressed, so it keeps its ETag and answers 304
// with zero bytes transferred, which is the case that matters: that is the
// 42 MiB engine. The policy is left correct-rather-than-fast while the client
// is still being debugged, because a stale asset is worse than a refetched
// one there. Versioned prefixes plus CACHE_IMMUTABLE is the real fix and
// belongs before any public traffic.
const cacheControl = (env) =>
  env.CACHE_IMMUTABLE === "true"
    ? "public, max-age=31536000, immutable"
    : "public, max-age=0, must-revalidate";

function baseHeaders(object, key, env) {
  const headers = new Headers();
  // writeHttpMetadata first so anything stored on the object is present, then
  // the type is overridden because it is the one header that must not be left
  // to whatever the upload happened to set.
  object.writeHttpMetadata(headers);
  headers.set("content-type", contentTypeFor(key));
  headers.set("etag", object.httpEtag);
  headers.set("cache-control", cacheControl(env));
  headers.set("accept-ranges", "bytes");
  headers.set("x-content-type-options", "nosniff");
  // A browser loading this page was served by the same origin, so no CORS
  // headers are needed or wanted.
  return headers;
}

// If-None-Match is a list, may be `*`, and entries may be weak. Compare on the
// opaque value so W/"x" and "x" match.
function etagMatches(header, httpEtag) {
  const bare = (value) => value.trim().replace(/^W\//, "").replace(/^"|"$/g, "");
  if (header.trim() === "*") return true;
  const target = bare(httpEtag);
  return header.split(",").some((candidate) => bare(candidate) === target);
}

// The configuration the client reads before its engine starts, as
// `window.HOMEWORLDZ` (the contract is documented in the client repository at
// frontends/godot/project/page_config.gd).
//
// It is injected here because there is no other page. The export's own
// index.html comes out of R2 unchanged, this Worker is what serves it, and the
// client deliberately reads a page global rather than reaching into the site's
// storage itself — the token is the site's to hand over.
//
// The static half comes from this Worker's vars; the token half cannot. It
// lives in the browser under localStorage["homeworldz.auth"] on this origin,
// which a Worker cannot see, so what is injected is a small script that reads
// it in the browser. Same origin is what makes that possible and is the whole
// reason there is no sign-in handoff to build.
//
// Absent means absent, deliberately: a key this Worker has no value for is not
// written at all, because the client reports what it was not given rather than
// guessing. signOutUrl in particular has no default — pointed at an ordinary
// page it would navigate away and leave the token in place, which reads as a
// sign-out and is not one.
export function pageConfigScript(env) {
  const configured = {
    apiOrigin: env.API_ORIGIN,
    signInUrl: env.SIGN_IN_URL,
    signOutUrl: env.SIGN_OUT_URL,
    siteUrl: env.SITE_URL,
    start: env.START_LOCATION,
  };
  const settings = {};
  for (const [name, value] of Object.entries(configured))
    if (typeof value === "string" && value !== "") settings[name] = value;

  // The token is read at load, in the browser, from the key the account site
  // writes. Both halves or neither: the client refuses a token whose expiry is
  // zero rather than treating it as permanent, so a token that cannot be given
  // a real expiry is not handed over at all.
  return `<script>
(function () {
  var config = ${JSON.stringify(settings)};
  try {
    var stored = window.localStorage.getItem("homeworldz.auth");
    if (stored) {
      var session = JSON.parse(stored);
      var expiresAt = Math.floor(Date.parse(session && session.expiresAt) / 1000);
      if (session && session.accessToken && isFinite(expiresAt) && expiresAt > Date.now() / 1000) {
        config.accountToken = session.accessToken;
        config.accountTokenExpiresAt = expiresAt;
      }
    }
  } catch (error) {
    // A blocked or unreadable store is a page with no token, which is a state
    // the client already handles by sending the person to sign in. Throwing
    // here would instead stop the engine from starting at all.
  }
  window.HOMEWORLDZ = config;
})();
</script>`;
}

const notFound = () => new Response("not found", { status: 404 });

export default {
  async fetch(request, env) {
    try {
      if (request.method !== "GET" && request.method !== "HEAD") {
        return new Response("method not allowed", {
          status: 405,
          headers: { allow: "GET, HEAD" },
        });
      }

      const url = new URL(request.url);
      // /app and /app/ both mean the export's entry document.
      let key = url.pathname.replace(/^\/app\/?/, "");
      if (key === "") key = "index.html";
      if (key.includes("..")) return notFound();
      if (env.KEY_PREFIX) key = `${env.KEY_PREFIX.replace(/\/$/, "")}/${key}`;

      // Conditionals are answered from metadata rather than through R2's
      // `onlyIf`. That option returns a bodiless object whether the
      // precondition passed OR failed, which made the two cases
      // indistinguishable here and produced a 500 on every revalidation
      // (found live, 2026-08-22). head() says exactly what is being asked.
      // The entry document is rewritten below, so its bytes are not the bytes
      // R2 holds and its ETag does not describe what is served. A 304 here
      // would hand back a cached copy carrying the configuration this Worker
      // injected the last time its vars were different.
      const isDocument = key.endsWith("index.html");

      const ifNoneMatch = request.headers.get("if-none-match");
      if (ifNoneMatch && !isDocument) {
        const metadata = await env.CLIENT_BUCKET.head(key);
        if (metadata === null) return notFound();
        if (etagMatches(ifNoneMatch, metadata.httpEtag)) {
          return new Response(null, { status: 304, headers: baseHeaders(metadata, key, env) });
        }
      }

      // Only a request that actually carried a Range gets a 206. R2 populates
      // `object.range` even for an unranged get, so branching on it alone
      // answered 206 to everything — including the plain document fetch
      // (found live, 2026-08-22).
      const wantsRange = request.headers.has("range");
      const object = await env.CLIENT_BUCKET.get(
        key,
        wantsRange ? { range: request.headers } : undefined,
      );
      if (object === null) return notFound();

      const headers = baseHeaders(object, key, env);
      const body = request.method === "HEAD" ? null : object.body;

      if (isDocument) {
        // No validator and no caching for the document: it carries
        // configuration that changes when this Worker's vars change, under a
        // filename that does not. It is a few kilobytes; the 42 MiB engine
        // beside it still caches and revalidates normally.
        headers.delete("etag");
        headers.set("cache-control", "no-store");
        const document = new Response(body, { headers });
        if (request.method === "HEAD") return document;
        return new HTMLRewriter()
          .on("head", {
            element(element) {
              // Prepended so it runs before the engine bootstrap, which is
              // what "before the engine starts" in the contract means.
              element.prepend(pageConfigScript(env), { html: true });
            },
          })
          .transform(document);
      }

      if (wantsRange && object.range && "offset" in object.range) {
        const start = object.range.offset ?? 0;
        const length = object.range.length ?? object.size - start;
        headers.set("content-range", `bytes ${start}-${start + length - 1}/${object.size}`);
        return new Response(body, { status: 206, headers });
      }

      return new Response(body, { headers });
    } catch (error) {
      // Without this a fault is an opaque 500 and the cause has to be guessed
      // at, which cost a debugging round already.
      console.error(`app-worker failed for ${request.url}: ${error?.stack ?? error}`);
      return new Response("internal error", { status: 500 });
    }
  },
};
