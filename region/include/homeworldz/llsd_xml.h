#pragma once

// A small LLSD XML reader for capability bodies whose structure is nested —
// maps holding arrays holding binary members — where the flat key scanning of
// viewer_capabilities.cpp cannot answer. The mesh model upload of ADR 0033 M2
// is the first such body: its asset_resources carry whole mesh payloads as
// base64 binary inside arrays of maps.
//
// This reads; it does not write. Region replies remain hand-assembled XML,
// which keeps them greppable against the capability they serve.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace homeworldz::llsd {

struct Value {
    enum class Type {
        undefined, boolean, integer, real, string, uuid, uri, binary, map, array
    };
    Type type{Type::undefined};
    bool boolean{};
    std::int64_t integer{};
    double real{};
    std::string text; // string, uuid, and uri payloads
    std::vector<std::byte> binary;
    std::vector<std::pair<std::string, Value>> members; // map, in document order
    std::vector<Value> elements;                        // array

    // Map lookup; null when this is not a map or the key is absent.
    const Value* find(std::string_view key) const;
    // Numeric reads across LLSD's forgiving coercions: integer, real, and
    // boolean all answer; anything else is the fallback.
    std::int64_t as_integer(std::int64_t fallback = 0) const;
    double as_real(double fallback = 0.0) const;
};

// Parse one LLSD XML document (an optional <?xml?> declaration, then <llsd>
// wrapping exactly one value). Nothing (not a best-effort partial value) on
// malformed input, unknown elements, or exceeded bounds — capability bodies
// are machine-written, so anything irregular is refused rather than repaired.
std::optional<Value> parse_xml(std::string_view xml);

// Standard base64 to bytes. Public because it is the only decoder in the tree
// and a second one would drift from it; the encoder lives in session_protocol.h
// for the same reason, which is a split worth collapsing one day.
std::optional<std::vector<std::byte>> decode_base64(std::string_view text);

} // namespace homeworldz::llsd
