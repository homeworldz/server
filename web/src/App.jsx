import { Show } from "solid-js";
import { A, useNavigate } from "@solidjs/router";
import homeworldzMark from "./assets/brand/homeworldz-mark.svg";
import { currentIdentity, isAuthenticated, logout } from "./lib/auth";
import { hasAnyAdmin } from "./lib/privileges";

// The marketing site is its own repository and deployment; the two sites
// share no build and no session machinery, so links back are plain anchors.
const MARKETING_URL = "https://homeworldz.com";

export function App(props) {
  const navigate = useNavigate();
  const showAdmin = () =>
    isAuthenticated() && hasAnyAdmin(currentIdentity()?.privs ?? "");

  const closeMobileNavigation = (event) => {
    event.currentTarget.closest("details")?.removeAttribute("open");
  };

  const handleLogout = (event) => {
    event.preventDefault();
    closeMobileNavigation(event);
    logout();
    navigate("/login", { replace: true });
  };

  return (
    <>
      <header class="site-header">
        <nav aria-label="Primary navigation">
          <div class="nav-start">
            <details class="mobile-nav">
              <summary aria-label="Open navigation menu">
                <span class="hamburger-icon" aria-hidden="true"></span>
              </summary>
              <div class="mobile-nav-panel">
                <A href="/" onClick={closeMobileNavigation}>Home</A>
                <A href="/stats" onClick={closeMobileNavigation}>Statistics</A>
                <Show
                  when={isAuthenticated()}
                  fallback={
                    <>
                      <A href="/register" onClick={closeMobileNavigation}>Register</A>
                      <A href="/login" onClick={closeMobileNavigation}>Login</A>
                    </>
                  }
                >
                  <A href="/account" onClick={closeMobileNavigation}>Account</A>
                  <Show when={showAdmin()}>
                    <A href="/admin" onClick={closeMobileNavigation}>Admin</A>
                  </Show>
                  <a href="/login" onClick={handleLogout}>Logout</a>
                </Show>
              </div>
            </details>
            <a href={MARKETING_URL} class="brand" aria-label="Homeworldz.com">
              <img src={homeworldzMark} alt="" width="64" height="64" />
              <span>Homeworldz</span>
            </a>
          </div>
          <div class="nav-controls">
            <div class="nav-links">
              <A href="/" end>Home</A>
              <A href="/stats">Statistics</A>
              <Show
                when={isAuthenticated()}
                fallback={
                  <>
                    <A href="/register" role="button" data-variant="outline">Register</A>
                    <A href="/login">Login</A>
                  </>
                }
              >
                <A href="/account">Account</A>
                <Show when={showAdmin()}>
                  <A href="/admin">Admin</A>
                </Show>
                <a href="/login" onClick={handleLogout}>Logout</a>
              </Show>
            </div>
            <span data-kind="theme-toggle" aria-label="Choose color theme"></span>
          </div>
        </nav>
      </header>

      <main>{props.children}</main>
    </>
  );
}
