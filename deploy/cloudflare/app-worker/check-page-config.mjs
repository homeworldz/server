// Exercises the page configuration this Worker injects, without deploying it.
//
// The injected script is the only code here that runs in someone's browser
// rather than at the edge, and its failure modes are quiet ones: a token
// handed over without a usable expiry is refused by the client and looks like
// a page that named nothing, and a thrown exception in it stops the engine
// from starting at all. Neither shows up in a deploy that "worked".
//
//   node deploy/cloudflare/app-worker/check-page-config.mjs
//
// Exits non-zero on the first failure, so it is usable as a gate.
import { pageConfigScript } from "./worker.js";

let failures = 0;
function check(what, passed) {
  console.log(`${passed ? "ok  " : "FAIL"}  ${what}`);
  if (!passed) failures += 1;
}

const configured = pageConfigScript({
  API_ORIGIN: "https://api.homeworldz.com",
  SIGN_IN_URL: "/login",
  SIGN_OUT_URL: "/logout",
  SITE_URL: "/",
  START_LOCATION: "",
});

for (const key of ["apiOrigin", "signInUrl", "signOutUrl", "siteUrl"])
  check(`${key} is injected when configured`, configured.includes(`"${key}"`));
// Absent means absent: the client reports what it was not given rather than
// guessing, so an unset var must not become an empty string in the object.
check("an unset var is omitted rather than sent empty", !configured.includes('"start"'));
// The token is read in the browser. If it ever appears in what the Worker
// returns, it has been baked into a cacheable document for whoever asks.
check("no token in the served bytes", !configured.includes('"accountToken":'));

const bare = pageConfigScript({});
check("a Worker with no vars injects no settings", bare.includes("var config = {};"));
check("and still reads the stored token", bare.includes("homeworldz.auth"));

// The token half, run the way a browser runs it.
const body = configured.replace(/^<script>|<\/script>$/g, "");
const now = Math.floor(Date.now() / 1000);
const iso = (offset) => new Date((now + offset) * 1000).toISOString();

for (const [name, stored, wantsToken] of [
  ["a live session hands over both halves", JSON.stringify({ accessToken: "tok", expiresAt: iso(600) }), true],
  ["an expired session hands over neither", JSON.stringify({ accessToken: "tok", expiresAt: iso(-600) }), false],
  // The trap this exists for: the client refuses an expiry of zero rather
  // than reading it as "never expires", so a token that cannot be given a
  // real one must not be handed over at all.
  ["a token with no expiry hands over neither", JSON.stringify({ accessToken: "tok" }), false],
  ["an empty store hands over neither", null, false],
  ["an unreadable store hands over neither, and does not throw", "{not json", false],
]) {
  const window = { localStorage: { getItem: () => stored } };
  let threw = false;
  try {
    new Function("window", body)(window);
  } catch {
    threw = true;
  }
  const config = window.HOMEWORLDZ ?? {};
  const hasToken = typeof config.accountToken === "string";
  const hasExpiry = Number.isFinite(config.accountTokenExpiresAt) && config.accountTokenExpiresAt > 0;
  check(name, !threw && hasToken === wantsToken && hasExpiry === wantsToken);
}

if (failures !== 0) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log("page configuration checks passed");
