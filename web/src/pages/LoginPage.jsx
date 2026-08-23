import { createSignal, Show } from "solid-js";
import { A, useNavigate } from "@solidjs/router";
import { ApiError, createToken } from "../lib/api";
import { login } from "../lib/auth";
import { PasswordField } from "../components/PasswordField";
import { GridStats } from "../components/GridStats";
import homeworldzLogo from "../assets/brand/homeworldz.svg";

export function LoginPage() {
  const navigate = useNavigate();
  const [userid, setUserid] = createSignal("");
  const [password, setPassword] = createSignal("");
  const [error, setError] = createSignal(null);
  const [submitting, setSubmitting] = createSignal(false);

  const submit = async (event) => {
    event.preventDefault();
    setError(null);
    setSubmitting(true);
    try {
      const token = await createToken(userid().trim(), password());
      login(token);
      navigate("/account", { replace: true });
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) {
        setError("Incorrect name/login ID or password.");
      } else if (err instanceof ApiError && err.status === 429) {
        setError("Too many attempts. Please wait a moment and try again.");
      } else if (err instanceof ApiError) {
        setError(err.message);
      } else {
        setError("We couldn't reach the login service. Please try again.");
      }
    } finally {
      setSubmitting(false);
    }
  };

  // Three columns on a desktop - logo, form, statistics - collapsing to one
  // on anything narrower. The form is the middle column at every width and is
  // first in the source, so a keyboard or a screen reader reaches it before
  // the decoration and the figures either side of it.
  return (
    <div class="login-layout">
      <section class="auth-page login-form-column" aria-labelledby="login-title">
        <form class="auth-card" onSubmit={submit} novalidate>
          <h1 id="login-title">Login</h1>

          <div class="field">
            <label for="userid">Login ID</label>
            <input
              id="userid"
              name="userid"
              type="text"
              autocomplete="username"
              value={userid()}
              onInput={(event) => setUserid(event.currentTarget.value)}
              required
            />
          </div>

          <div class="field">
            <label for="password">Password</label>
            <PasswordField
              id="password"
              name="password"
              autocomplete="current-password"
              value={password()}
              onInput={(event) => setPassword(event.currentTarget.value)}
              required
            />
          </div>

          <Show when={error()}>
            <p class="form-error" role="alert">
              {error()}
            </p>
          </Show>

          <div class="auth-actions">
            <button type="submit" disabled={submitting()}>
              {submitting() ? "Logging in…" : "Login"}
            </button>
          </div>

          <p class="auth-alt">
            <A href="/forgot">Forgot your password?</A>
          </p>
          <p class="auth-alt">
            Need an avatar? <A href="/register">Register</A>.
          </p>
        </form>
      </section>

      <div class="login-logo-column" aria-hidden="true">
        <img src={homeworldzLogo} alt="" />
      </div>

      <div class="login-stats-column">
        <GridStats />
      </div>
    </div>
  );
}
