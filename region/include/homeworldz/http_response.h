#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace homeworldz::http {

inline constexpr std::string_view request_id_header = "X-Request-ID";

struct Response {
    int status_code;
    std::string request_id;
    std::string method;
    std::string path;
    std::string content;
};

std::optional<std::size_t> request_content_length(std::string_view request);
std::string request_header_value(std::string_view request, std::string_view name);
Response response_for(std::string_view request);
Response response_for(std::string_view request, std::string_view version);
Response response_for_content(std::string_view request, int status_code,
                              std::string_view content_type, std::string body);
// A 302 carrying only a Location, which is the whole protocol for the viewer's
// ServerReleaseNotes capability.
Response response_for_redirect(std::string_view request, std::string_view location);
// A 206 slice of full_body with Content-Range, for the ranged fetches viewer
// mesh loading performs (header first, then per-LOD extents).
Response response_for_range(std::string_view request, std::string_view content_type,
                            std::string_view full_body, std::size_t offset, std::size_t length);

// Add one header to an already-built response, after the status line. For the
// cases where a header is a property of the resource rather than of the reply
// shape - an ETag, Accept-Ranges - and so is not worth a parameter on every
// constructor. Does nothing to a malformed response rather than corrupting it.
void add_header(Response& response, std::string_view name, std::string_view value);

// True for the region's browser-facing session routes (/session/...). The
// Homeworldz client runs in a browser served from another origin, so these —
// and only these — need CORS. Viewer capabilities under /caps/ are spoken by
// Firestorm, which is not a browser and enforces no same-origin policy.
bool is_browser_session_path(std::string_view path);

// Cross-origin headers for a browser-facing session route.
//
// Allow-Origin is `*` and there is deliberately NO Allow-Credentials: these
// routes authenticate with an explicit `Authorization: Bearer <region ticket>`
// header and never a cookie, so a hostile page holds no ambient credential and
// `*` grants nothing a plain server-side fetch could not already do. That is
// also why no origin allowlist is configured here — there is nothing for one
// to protect, and a per-region list would have to be configured on regions the
// operator may not control (ADR 0028).
//
// Expose-Headers is load-bearing rather than decorative: ETag, Accept-Ranges
// and Content-Range are invisible to JavaScript unless named, so without it a
// client's revalidation and partial-heightmap fetches fail while the region
// answers perfectly — a defect that looks client-side from every angle. When
// `preflight`, the allowed methods and request headers are named too, which a
// browser demands before it will send Authorization, If-None-Match or Range at
// all.
void add_cors_headers(Response& response, bool preflight);

} // namespace homeworldz::http

