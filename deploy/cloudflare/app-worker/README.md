# Client app Worker

Serves the Homeworldz client at `https://my.homeworldz.com/app/*` from an R2
bucket.

## Why this exists

The client is served from the **management site's origin** so it can read the
token that site stores under `localStorage["homeworldz.auth"]`. That is what
removes the need for a sign-in handoff between origins — no authorize route, no
redirect allowlist, no code exchange.

The obvious way to do that is to publish the client's build into the management
site's Pages project. It cannot work: **Cloudflare Pages caps a single asset at
25 MiB**, and Godot's engine wasm is 42 MiB (37 MiB even for an export with no
extension). Workers static assets and Workers KV share the same 25 MiB cap, and
D1 allows 2 MB per value, so no Cloudflare storage but **R2** can hold the file
at all — R2 allows 5 TiB per object.

Only the storage moves. The route is on `my.homeworldz.com`, so the browser
origin is unchanged and nothing about CORS, MIME, or the shared token differs
from serving it out of Pages.

## What the operator has to do

Wrangler is needed here — Pages deploys from git without it, but a Worker
cannot.

1. **Create the bucket** (once):
   `npx wrangler r2 bucket create homeworldz-client-web`
2. **Upload an export**: from the repository root, with the client's
   `build-web/` built, run `node scripts/publish-client-web.mjs`. It sets each
   file's content type explicitly, because a `.wasm` served as
   `application/octet-stream` is refused by `WebAssembly.instantiateStreaming`
   and looks like a broken build rather than a missing header.
3. **Deploy the Worker** (once, and after any change to `worker.js`):
   `npx wrangler deploy` from this directory.
4. **Confirm the route wins over Pages.** Cloudflare's documented precedence is
   that the more specific pattern applies, and `my.homeworldz.com/app/*` is
   more specific than the hostname Pages answers on — but the docs' worked
   example is Worker-versus-Worker, so verify rather than assume: after
   deploying, `https://my.homeworldz.com/app/` must return the client's
   `index.html`, and `https://my.homeworldz.com/login` must still return the
   management site. If `/app/` returns the management site's HTML instead, the
   route is not taking effect.

`web/public/_redirects` still carries an `/app/*` rule. It becomes dead once
this Worker is deployed, since matching requests never reach Pages, and is kept
only so that an undeployed Worker fails as a 404 rather than as a blank page.

## Caching

`CACHE_IMMUTABLE` is `false` by default, which serves
`Cache-Control: public, max-age=0, must-revalidate` and revalidates against
R2's ETag. That is the correct default because Godot writes stable filenames
(`index.wasm`, `index.side.wasm`), so content changes under an unchanged name
and `immutable` would pin a stale engine in every browser that had loaded it.
Revalidation costs one small conditional request; the 42 MiB body still comes
from cache on a 304.

Measured on the live deployment: **Cloudflare strips the ETag from responses
it compresses**, so `text/html` and `text/javascript` arrive with no validator
and refetch in full on every load (`index.js` is 2.7 MB). `application/wasm` is
not compressed, keeps its ETag, and answers `304` with **zero bytes**
transferred — so the 42 MiB engine, the one that matters, revalidates for free.
A cold fetch of it measured 1.8 s.

Set it to `true` only once uploads go to a versioned prefix via `KEY_PREFIX`,
where a filename genuinely cannot change meaning. That is the real speed win
and worth doing before any public traffic.

## What this does not fix

The export is 42 MiB, about 10 MiB gzipped, and a cold visitor downloads all of
it before the client starts. Storage decides where the bytes live and caching
decides how far away they are, but only size sets the floor. A custom Godot
template with unused modules disabled is the only thing that lowers it — the
shipped engine contains `NavigationServer`, `OpenXR`, and `ufbx`, none of which
this client needs — and it would also take the engine back under 25 MiB, making
this Worker unnecessary.
