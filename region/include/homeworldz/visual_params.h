#ifndef HOMEWORLDZ_VISUAL_PARAMS_H
#define HOMEWORLDZ_VISUAL_PARAMS_H

#include "homeworldz/wearable.h"

#include <cstdint>
#include <vector>

namespace homeworldz::viewer {

// Assemble the AvatarAppearance visual_params byte array for an avatar wearing
// the given wearables. The transmitted set is the 253 group-0 (tweakable) plus
// group-3 (transmit-not-tweakable) visual params, in ascending param-ID order —
// the ordering the Second Life viewer / LibreMetaverse decoder expect. Each
// param takes its value from the first worn wearable that specifies it,
// otherwise its canonical default, quantized as
// floor((clamp(value, min, max) - min) / (max - min) * 255).
//
// With no wearables (or wearables that set no params) this yields the canonical
// default avatar's shape.
// appearance_version sets visual param 11000 (AppearanceMessage Version), which
// must match the AppearanceData version field in the AvatarAppearance message
// (0 = legacy, 1 = server-side appearance) or the viewer discards the message.
std::vector<std::uint8_t> build_visual_params(const std::vector<Wearable>& worn,
                                              std::uint8_t appearance_version = 0);

// The number of visual params transmitted (253).
std::size_t visual_param_count();

// Where param 11000 sits in that array, so a relayed appearance can be held to
// the same rule build_visual_params applies to a built one.
//
// The version is on the wire twice and the two halves arrive by different
// routes: inbound it is only ever visual param 11000 (AgentSetAppearance has no
// version field at all), outbound it is also the AppearanceData byte. Copying a
// viewer's params through while writing our own byte therefore forwards its
// answer and overrides it at the same time.
std::size_t appearance_version_param_index();

}  // namespace homeworldz::viewer

#endif  // HOMEWORLDZ_VISUAL_PARAMS_H
