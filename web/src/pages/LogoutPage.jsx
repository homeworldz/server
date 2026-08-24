import { onMount } from "solid-js";
import { A } from "@solidjs/router";
import { logout } from "../lib/auth";

// Where the Homeworldz client sends somebody who signed out from inside the
// world, and the only URL on this site that actually signs out: everywhere else
// sign-out is a click handler, so navigating to an ordinary page leaves the
// token in place and the next visit to /app adopts it again.
//
// It clears on mount rather than asking. By the time this page loads the client
// has already retired the avatar and closed both channels, and none of that can
// be undone — a confirmation here would offer a "cancel" that restores nothing
// and leaves somebody out of the world, still signed in, with the app they came
// from gone. A prompt belongs on the client's side of the handover, before the
// teardown, if it belongs anywhere.
//
// Clearing is idempotent, which matters more than it sounds: the client hands
// over without checking whether it still holds a token, and a second click
// before the navigation completes arrives here twice.
export function LogoutPage() {
  onMount(() => logout());

  return (
    <section class="auth-page" aria-labelledby="logout-title">
      <div class="auth-card">
        <h1 id="logout-title">Signed out</h1>

        {/* Two facts, named apart. Leaving the world and leaving the account
            are different things, and somebody who did one wants to know
            whether they also did the other. */}
        <p class="lede">Your avatar is logged out from the world.</p>
        <p class="lede">
          You are also signed out of your account on this site, on this device.
        </p>

        <p class="auth-alt">
          <A href="/login">Log in again</A>
        </p>
      </div>
    </section>
  );
}
