import { render } from "solid-js/web";
import { Show } from "solid-js";
import { Navigate, Route, Router } from "@solidjs/router";
import "vitre-css/vitre-base.css";
import "vitre-css/vitre.css";
import { Vitre } from "vitre-js";
import "./styles.css";
import { App } from "./App";
import { AccountPage } from "./pages/AccountPage";
import { LoginPage } from "./pages/LoginPage";
import { StatsPage } from "./pages/StatsPage";
import { RegisterPage } from "./pages/RegisterPage";
import { VerifyPage } from "./pages/VerifyPage";
import { ForgotPasswordPage } from "./pages/ForgotPasswordPage";
import { ResetPasswordPage } from "./pages/ResetPasswordPage";
import { AdminPage } from "./pages/AdminPage";
import { AdminUsersPage } from "./pages/AdminUsersPage";
import { AdminUserPage } from "./pages/AdminUserPage";
import { AdminRegionsPage } from "./pages/AdminRegionsPage";
import { AdminRegionPage } from "./pages/AdminRegionPage";
import { AdminSystemPage } from "./pages/AdminSystemPage";
import { currentIdentity, isAuthenticated } from "./lib/auth";
import { hasAnyAdmin } from "./lib/privileges";

function RequireAuth(props) {
  return (
    <Show when={isAuthenticated()} fallback={<Navigate href="/login" />}>
      {props.children}
    </Show>
  );
}

// Gates the admin area: unauthenticated visitors go to login; authenticated
// accounts without any admin privilege are sent to their own account page.
function RequireAdmin(props) {
  return (
    <Show when={isAuthenticated()} fallback={<Navigate href="/login" />}>
      <Show
        when={hasAnyAdmin(currentIdentity()?.privs ?? "")}
        fallback={<Navigate href="/account" />}
      >
        {props.children}
      </Show>
    </Show>
  );
}

const root = document.getElementById("root");

if (!root) {
  throw new Error("The application root element is missing.");
}

render(
  () => (
    <Router root={App}>
      <Route path="/" component={() => <Navigate href="/account" />} />
      <Route path="/register" component={RegisterPage} />
      <Route path="/verify" component={VerifyPage} />
      <Route path="/login" component={LoginPage} />
      <Route path="/stats" component={StatsPage} />
      <Route path="/forgot" component={ForgotPasswordPage} />
      {/* The emailed link is {reset_url}/{token}, so the token is a path
          segment rather than a query parameter. */}
      <Route path="/reset/:token" component={ResetPasswordPage} />
      <Route
        path="/account"
        component={() => (
          <RequireAuth>
            <AccountPage />
          </RequireAuth>
        )}
      />
      <Route
        path="/admin"
        component={() => (
          <RequireAdmin>
            <AdminPage />
          </RequireAdmin>
        )}
      />
      <Route
        path="/admin/users"
        component={() => (
          <RequireAdmin>
            <AdminUsersPage />
          </RequireAdmin>
        )}
      />
      <Route
        path="/admin/users/:id"
        component={() => (
          <RequireAdmin>
            <AdminUserPage />
          </RequireAdmin>
        )}
      />
      <Route
        path="/admin/regions"
        component={() => (
          <RequireAdmin>
            <AdminRegionsPage />
          </RequireAdmin>
        )}
      />
      <Route
        path="/admin/regions/:id"
        component={() => (
          <RequireAdmin>
            <AdminRegionPage />
          </RequireAdmin>
        )}
      />
      <Route
        path="/admin/system"
        component={() => (
          <RequireAdmin>
            <AdminSystemPage />
          </RequireAdmin>
        )}
      />
    </Router>
  ),
  root,
);

Vitre.apply();
