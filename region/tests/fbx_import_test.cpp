// What the FBX importer does with input that is not a good FBX.
//
// It cannot test what a *good* FBX becomes: the corpus that proves that is the
// Character Creator bodies, which are local-only for licence reasons and cannot
// be committed (tests/fixtures/README.md). `homeworldz-fbx-diag` is where that
// side is checked, against the real files, by a person who has them.
//
// What is testable here is the half that matters for a service: this code runs
// in the conversion worker on bytes a stranger uploaded (ADR 0035), and every
// one of these cases must come back as a refusal with a reason rather than a
// crash, a hang, or an "ok" carrying nothing.
#include "homeworldz/fbx_import.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    if (!text.empty()) std::memcpy(out.data(), text.data(), text.size());
    return out;
}

} // namespace

int main() {
    // Nothing at all.
    {
        const auto result = homeworldz::mesh::gltf_from_fbx({});
        if (result.ok) return 1;
        if (result.error.empty()) return 2;
        if (!result.meshes.empty()) return 3;
    }

    // Bytes that are not any file format.
    {
        const auto result = homeworldz::mesh::gltf_from_fbx(bytes("not an FBX, not anything"));
        if (result.ok) return 4;
        if (result.error.empty()) return 5;
    }

    // The binary FBX magic and then nothing — a truncated file, which is the
    // shape a cancelled upload arrives in and the one most likely to walk off
    // the end of a buffer if the reader trusts a declared length.
    {
        auto truncated = bytes("Kaydara FBX Binary  ");
        truncated.push_back(std::byte{0x00});
        truncated.push_back(std::byte{0x1a});
        truncated.push_back(std::byte{0x00});
        const auto result = homeworldz::mesh::gltf_from_fbx(truncated);
        if (result.ok) return 6;
        if (result.error.empty()) return 7;
    }

    // A GLB, offered as an FBX. Every format this server reads should refuse
    // the others by name rather than half-parse one as the other; the mesh
    // pipeline has a GLB path and this is not it.
    {
        const auto result = homeworldz::mesh::gltf_from_fbx(bytes("glTF\x02\x00\x00\x00"));
        if (result.ok) return 8;
        if (result.error.empty()) return 9;
    }

    // An ASCII FBX header with no content. ufbx parses ASCII and binary by
    // different paths, so a refusal on one is not evidence about the other.
    {
        const auto result = homeworldz::mesh::gltf_from_fbx(
            bytes("; FBX 7.4.0 project file\n\nFBXHeaderExtension:  {\n}\n"));
        if (result.ok) return 10;
        if (result.error.empty()) return 11;
    }

    // A refusal reason is shown to the creator, so it has to be a sentence
    // rather than an enum name. Every case above went through the same path;
    // this checks that path says something.
    {
        const auto result = homeworldz::mesh::gltf_from_fbx(bytes("rubbish"));
        if (result.error.size() < 8) return 12;
    }

    // The dispatch sniff. meshsmith uses this to decide which converter a
    // canonical blob wants, so a wrong answer sends a GLB down the FBX path or
    // an FBX down the type-49 one, and the failure surfaces as a converter
    // error about the wrong format entirely.
    {
        using homeworldz::mesh::looks_like_fbx;
        if (!looks_like_fbx(bytes("Kaydara FBX Binary  \x00\x1a\x00"))) return 13;
        if (!looks_like_fbx(bytes("; FBX 7.4.0 project file"))) return 14;
        // The two formats this worker converts between must not be mistaken
        // for it. A GLB begins "glTF" and a type-49 asset begins with its own
        // header; neither is an FBX.
        if (looks_like_fbx(bytes("glTF\x02\x00\x00\x00"))) return 15;
        if (looks_like_fbx({})) return 16;
        // Short inputs must not read past the end to answer.
        if (looks_like_fbx(bytes("Kay"))) return 17;
        if (looks_like_fbx(bytes(";"))) return 18;
    }

    return 0;
}
