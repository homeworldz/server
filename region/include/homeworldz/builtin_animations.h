#pragma once

#include <optional>
#include <string_view>

namespace homeworldz::region {

// The asset id of a built-in avatar animation a script names rather than
// carries in its object's contents — llStartAnimation("sit"), the default sit
// a pose script stops before starting its own (ADR 0040). Nothing for a name
// that is not built in.
std::optional<std::string_view> builtin_animation_id(std::string_view name);

} // namespace homeworldz::region
