import { defineConfig } from "vite";
import solid from "vite-plugin-solid";

// The Homeworldz client's local static server. Web dev ports run one decade
// per deliverable — 4321x marketing site, 4322x this one — so the client takes
// 4323x rather than a spare number inside another site's decade.
const CLIENT_ORIGIN = "http://127.0.0.1:43230";

export default defineConfig({
  plugins: [solid()],
  server: {
    // Fixed and distinct from the marketing site's 43210, so both dev
    // servers can run side by side.
    port: 43220,
    proxy: {
      // The client is served from THIS origin under /app so it shares this
      // site's localStorage and needs no sign-in handoff (operator,
      // 2026-08-22). Production does the same through Cloudflare; locally the
      // dev server is the only thing that can put both on one origin.
      //
      // The prefix is stripped because the client's static export is served
      // at the root of its own server, so /app/foo.wasm is /foo.wasm there.
      "/app": {
        target: CLIENT_ORIGIN,
        rewrite: (path) => path.replace(/^\/app/, "") || "/",
        configure: (proxy) => {
          // A stale cached wasm module reads exactly like an unchanged build
          // failure — the client session lost hours to precisely that, with
          // the browser replaying a file that had already been deleted. Dev
          // proxying is never the place to want caching.
          proxy.on("proxyRes", (proxyRes) => {
            proxyRes.headers["cache-control"] = "no-store";
          });
        },
      },
    },
  },
});
