import { API_BASE_URL } from "../config";
import { getToken } from "./auth";

export class ApiError extends Error {
  constructor(status, body) {
    super((body && body.message) || `Request failed (${status})`);
    this.name = "ApiError";
    this.status = status;
    this.code = body && body.code;
    this.field = body && body.field;
  }
}

async function request(path, { method = "GET", body, auth = false } = {}) {
  const headers = { "X-Request-ID": crypto.randomUUID() };
  if (body !== undefined) {
    headers["Content-Type"] = "application/json";
  }
  if (auth) {
    const token = getToken();
    if (!token) {
      throw new ApiError(401, {
        code: "unauthenticated",
        message: "Login to continue.",
      });
    }
    headers.Authorization = `Bearer ${token}`;
  }

  const response = await fetch(`${API_BASE_URL}${path}`, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
  });

  if (response.status === 204) {
    return null;
  }

  let payload = null;
  const text = await response.text();
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch {
      payload = null;
    }
  }

  if (!response.ok) {
    throw new ApiError(response.status, payload);
  }
  return payload;
}

export function register(displayName, email) {
  return request("/registrations", {
    method: "POST",
    body: { displayName, email },
  });
}

export function verify(code, password) {
  return request("/verifications", {
    method: "POST",
    body: { code, password },
  });
}

export function resendVerification(userid) {
  return request("/verifications/resend", {
    method: "POST",
    body: { userid },
  });
}

// Asks for a reset link. The reply is 202 whatever happened - unknown account,
// unverified, no address on file, mail failure, success - because any difference
// would let this endpoint reveal which accounts exist (ADR 0034). Callers must
// therefore show one message regardless, and must not infer anything from a
// resolved promise beyond "the request was accepted".
export function requestPasswordReset(identifier) {
  return request("/password-resets", {
    method: "POST",
    body: { identifier },
  });
}

// Spends a reset token and sets the password. Unknown, already-used and expired
// tokens all arrive as 400 invalid_token, deliberately indistinguishable, so a
// caller should surface one message rather than guessing which applied.
export function consumePasswordReset(token, password) {
  return request(`/password-resets/${encodeURIComponent(token)}`, {
    method: "POST",
    body: { password },
  });
}

// The public grid statistics, read by the login and statistics pages. No
// token: the login page shows them to people who have no account yet.
export function getGridStats() {
  return request("/stats");
}

export function createToken(userid, password) {
  return request("/tokens", {
    method: "POST",
    body: { userid, password },
  });
}

export function getAccount() {
  return request("/account", { auth: true });
}

export function updateProfile(changes) {
  return request("/account/profile", {
    method: "PATCH",
    body: changes,
    auth: true,
  });
}

export function changePassword(currentPassword, newPassword) {
  return request("/account/password", {
    method: "PUT",
    body: { currentPassword, newPassword },
    auth: true,
  });
}

// --- Admin: users ---

export function listUsers({ search, cursor, limit } = {}) {
  const params = new URLSearchParams();
  if (search) {
    params.set("search", search);
  }
  if (cursor) {
    params.set("cursor", cursor);
  }
  if (limit) {
    params.set("limit", String(limit));
  }
  const query = params.toString();
  return request(`/admin/users${query ? `?${query}` : ""}`, { auth: true });
}

export function getUser(id) {
  return request(`/admin/users/${encodeURIComponent(id)}`, { auth: true });
}

export function adminUpdateUser(id, changes) {
  return request(`/admin/users/${encodeURIComponent(id)}`, {
    method: "PATCH",
    body: changes,
    auth: true,
  });
}

export function replacePrivileges(id, privs) {
  return request(`/admin/users/${encodeURIComponent(id)}/privileges`, {
    method: "PUT",
    body: { privs },
    auth: true,
  });
}

export function banUser(id, reason, expiresAt) {
  const body = { reason };
  if (expiresAt) {
    body.expiresAt = expiresAt;
  }
  return request(`/admin/users/${encodeURIComponent(id)}/ban`, {
    method: "PUT",
    body,
    auth: true,
  });
}

export function unbanUser(id) {
  return request(`/admin/users/${encodeURIComponent(id)}/ban`, {
    method: "DELETE",
    auth: true,
  });
}

export function setUserTags(id, kind, tags) {
  return request(`/admin/users/${encodeURIComponent(id)}/tags`, {
    method: "PUT",
    body: { kind, tags },
    auth: true,
  });
}

// --- Admin: regions ---

export function listRegions() {
  return request("/admin/regions", { auth: true });
}

export function getRegion(id) {
  return request(`/admin/regions/${encodeURIComponent(id)}`, { auth: true });
}

export function createRegion(region) {
  return request("/admin/regions", {
    method: "POST",
    body: region,
    auth: true,
  });
}

export function setRegionTags(id, kind, tags) {
  return request(`/admin/regions/${encodeURIComponent(id)}/tags`, {
    method: "PUT",
    body: { kind, tags },
    auth: true,
  });
}

// --- Admin: system ---

export function getSystemStatus() {
  return request("/admin/system/status", { auth: true });
}

export function updateRegion(id, changes) {
  return request(`/admin/regions/${encodeURIComponent(id)}`, {
    method: "PATCH",
    body: changes,
    auth: true,
  });
}

export function moveRegion(id, gridX, gridY) {
  return request(`/admin/regions/${encodeURIComponent(id)}/map-position`, {
    method: "PUT",
    body: { gridX, gridY },
    auth: true,
  });
}

export function deployRegion(id) {
  return request(`/admin/regions/${encodeURIComponent(id)}/deployment`, {
    method: "POST",
    auth: true,
  });
}

export function undeployRegion(id) {
  return request(`/admin/regions/${encodeURIComponent(id)}/deployment`, {
    method: "DELETE",
    auth: true,
  });
}
