# Homeworldz management site

The **management site** served at `my.homeworldz.com`: login, registration,
email verification, account self-service, and user/region/system
administration. It is a thin client over the public `/v1` API in
[`grid/internal/api`](../grid/internal/api), which is why it lives in this
repository — every page it renders is a `/v1` call, so the two halves of that
contract version in lockstep and cannot drift.

The marketing site (`homeworldz.com`, its own repository) links here with
ordinary anchors; the two share no build and no session machinery. The
management pages were moved here from that repository on 2026-07-26, verbatim
where possible, so the two sites intentionally share conventions: SolidJS with
plain JSX (never TypeScript), Vite, the Vitre design system, pnpm exclusively.

Per [docs/STYLE.md](../docs/STYLE.md), this is the management site, never the
"frontend" — that word is reserved for the Homeworldz client's rendering
layers.

## Commands

- `pnpm install` — install locked dependencies
- `pnpm dev` — Vite dev server at `http://127.0.0.1:43220/` (fixed in
  `vite.config.js`, distinct from the marketing site's 43210 so both can run
  side by side)
- `pnpm build` — static bundle in `dist/`
- `pnpm preview` — serve the built bundle

The dev server also proxies `/app` to `http://127.0.0.1:43230`, where the
Homeworldz client serves its static web export. The client is deliberately on
**this** origin rather than one of its own, so it shares this site's
`localStorage` token and needs no sign-in handoff; the proxy is what puts both
on one origin locally, as Cloudflare does in production. Web dev ports run one
decade per deliverable — 4321x marketing, 4322x here, 4323x the client.

Two things bite when serving a web export through the proxy: `.wasm` must
arrive as `application/wasm` or streaming instantiation refuses it, and a
stale cached module is indistinguishable from an unchanged build failure, so
the proxy forces `Cache-Control: no-store` on everything it returns.

The client's *deployed* copy does not live here. Cloudflare Pages caps a single
asset at 25 MiB and Godot's engine wasm is 42 MiB, so the export cannot be part
of this project at all — it is served from R2 by a Worker routed on
`my.homeworldz.com/app/*`, which keeps the origin (and so the shared token)
identical to serving it from here. See
[deploy/cloudflare/app-worker](../deploy/cloudflare/app-worker/) and
`scripts/publish-client-web.mjs`.

There is no test suite; a successful `pnpm build` is the minimum validation,
plus a manual pass over affected routes.

## Layout

- **`src/index.jsx`** — entry point and routes. `/` redirects to `/account`;
  `RequireAuth` gates `/account`, `RequireAdmin` gates `/admin/*`.
- **`src/App.jsx`** — shared header and navigation. The link back to the
  marketing site is a plain `href`.
- **`src/pages/`** — Login, Register, Verify, Account, Stats, and the Admin
  pages (dashboard, users, user, regions, region, system). `/stats` is public,
  as is the statistics column beside the login form
  (`src/components/GridStats.jsx`, [ADR 0039](../docs/adr/0039-grid-statistics-and-event-log.md));
  both read `GET /v1/stats` with no token.
- **`src/lib/api.js`** — the `/v1` fetch wrapper; `src/config.js` reads
  `VITE_API_BASE_URL` and defaults to `https://api.homeworldz.com/v1`, and
  `VITE_GRID_BASE_URL` (defaulting to `https://grid.homeworldz.com`) for the
  files the grid serves directly, such as `stats.csv`. For local development
  against a local stack, put both in `web/.env.local`.
- **`src/styles.css`** — moved whole from the marketing site; it still
  contains that site's roadmap/landing rules, which are inert here and can be
  trimmed as the two sites' styles diverge.
- **`public/_redirects`** — SPA fallback (`/* /index.html 200`) for static
  hosting; direct navigation to a router path must reach `index.html`. The
  `/app/*` rule above it belongs to the Homeworldz client and must stay above
  it: first match wins, and without it a client path that is not a real file
  is served this site's `index.html`, which matches no route here and renders
  a blank page with a 200.

## Server-side configuration this site depends on

Two grid-side settings point at this site; both were repointed when
`my.homeworldz.com` went live on 2026-07-26:

- `[website] allowed_origins` must include `https://my.homeworldz.com` or the
  browser blocks every API call — it is exact-match, and a miss is a 403
  `origin_forbidden`. Add each dev origin too: `http://127.0.0.1:43220` and
  `http://my.homeworldz.local:43220`. Since the client shares this origin, it
  needs no entry of its own.
- `[mail] verification_url` is `https://my.homeworldz.com/verify`, so emailed
  verification links land on this site's `/verify` page directly. `reset_url`
  is the same shape. Both now default to this origin in `config.go` as well as
  in the deployed file (ADR 0034), so a fresh deployment no longer sends links
  to the marketing site.
