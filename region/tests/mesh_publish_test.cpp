// The publish queue's mechanics: that work submitted from the loop actually
// runs on the worker, that every job reports back exactly once, and that
// shutdown neither hangs nor drops what it accepted.
//
// This deliberately does not test `publish_glb` itself, which needs a grid and
// a vault and belongs to the integration path. What it tests is the part that
// is new and concurrent, and that is where the interesting failures are: a
// worker that never wakes, a result that arrives twice, a destructor that joins
// a thread still waiting on a queue it will never see again.
//
// The factories throw, so no sqlite file is opened and no socket is dialled.
// That is the whole point — a job still has to reach the worker, be attempted,
// and come back carrying the reason, which is exactly the path a region with an
// unreachable grid takes.
#include "homeworldz/mesh_publish.h"

#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>

namespace {

using homeworldz::mesh::PublishQueue;

std::vector<std::byte> some_bytes(std::size_t count) {
    return std::vector<std::byte>(count, std::byte{0x42});
}

// Collect results until `expected` have arrived or the wait runs out. Polling
// rather than sleeping a fixed time: a fixed sleep either flakes on a slow
// machine or wastes the difference on a fast one.
std::vector<PublishQueue::Result> drain(PublishQueue& queue, std::size_t expected) {
    std::vector<PublishQueue::Result> all;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (all.size() < expected && std::chrono::steady_clock::now() < deadline) {
        for (auto& result : queue.take_completed()) all.push_back(std::move(result));
        if (all.size() < expected) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return all;
}

} // namespace

int main() {
    // The glTF material document a face is given when a TextureEntry cannot
    // describe it. `publish_glb` needs a grid and a vault, but this part of it
    // is a pure function and the format has one detail that must not drift: an
    // image's `uri` is a bare asset UUID, because that is what Firestorm parses
    // it as (`texture_id.set(uri)`). A path, a data URI, or a "urn:" prefix
    // would all read as a null id and the face would lose its texture.
    {
        const auto textured = homeworldz::mesh::material_document(
            homeworldz::viewer::parse_uuid("abcdabcd-1111-4222-8333-444455556666"),
            {1.0f, 1.0f, 1.0f, 1.0f}, "BLEND", true);
        if (textured.find(R"("uri":"abcdabcd-1111-4222-8333-444455556666")") == std::string::npos)
            return 20;
        if (textured.find(R"("baseColorTexture":{"index":0})") == std::string::npos) return 21;
        if (textured.find(R"("doubleSided":true)") == std::string::npos) return 22;
        if (textured.find(R"("alphaMode":"BLEND")") == std::string::npos) return 23;
        // Stated rather than defaulted: an unstated metallicFactor is 1, which
        // draws the surface as metal.
        if (textured.find(R"("metallicFactor":0)") == std::string::npos) return 24;

        // No texture: no images or textures arrays at all, and the colour is the
        // whole surface.
        const auto bare = homeworldz::mesh::material_document(
            std::nullopt, {0.0f, 0.5f, 1.0f, 0.25f}, "OPAQUE", false);
        if (bare.find("\"images\"") != std::string::npos) return 25;
        if (bare.find("\"baseColorTexture\"") != std::string::npos) return 26;
        if (bare.find(R"("doubleSided":false)") == std::string::npos) return 27;
        // OPAQUE is glTF's default and is left unsaid rather than restated.
        if (bare.find("\"alphaMode\"") != std::string::npos) return 28;
        if (bare.find("[0,0.5,1,0.25]") == std::string::npos) return 29;
    }

    const auto failing_storage = [] -> std::unique_ptr<homeworldz::storage::RegionStorage> {
        throw std::runtime_error("storage deliberately unavailable in this test");
    };
    const auto failing_grid = [] -> std::unique_ptr<homeworldz::grid::Client> {
        throw std::runtime_error("grid deliberately unavailable in this test");
    };

    // Every submitted job comes back, exactly once, with the id it was given.
    {
        PublishQueue queue(failing_storage, failing_grid, "http://region.invalid");
        std::set<std::uint64_t> submitted;
        for (int at = 0; at < 8; ++at)
            submitted.insert(queue.submit(some_bytes(64), "Mesh", "creator"));
        if (submitted.size() != 8) return 1;  // ids must be distinct

        const auto results = drain(queue, 8);
        if (results.size() != 8) return 2;    // the worker never ran, or ran short
        std::set<std::uint64_t> reported;
        for (const auto& result : results) {
            // The factories threw, so nothing can have succeeded - and a
            // failure must still carry a reason, because that reason is what
            // the creator is shown.
            if (result.ok) return 3;
            if (result.error.empty()) return 4;
            if (!reported.insert(result.id).second) return 5;  // reported twice
        }
        if (reported != submitted) return 6;
        // Nothing left in flight once everything has been reported.
        if (queue.outstanding() != 0) return 7;
    }

    // take_completed hands each result over once and then forgets it, so the
    // loop cannot answer the same socket twice.
    {
        PublishQueue queue(failing_storage, failing_grid, "http://region.invalid");
        queue.submit(some_bytes(32), "Mesh", "creator");
        if (drain(queue, 1).size() != 1) return 8;
        if (!queue.take_completed().empty()) return 9;
    }

    // Destruction with work still queued must drain rather than abandon: an
    // upload accepted and dropped is one the creator was told nothing about.
    // It must also not hang, which is what the harness timing out would show.
    {
        PublishQueue queue(failing_storage, failing_grid, "http://region.invalid");
        for (int at = 0; at < 32; ++at) queue.submit(some_bytes(16), "Mesh", "creator");
    }

    // A queue that is never used must still start and stop cleanly. The worker
    // begins by opening its connections, so this is also the case where those
    // throw and nothing ever asks about it.
    {
        PublishQueue queue(failing_storage, failing_grid, "http://region.invalid");
        if (queue.outstanding() != 0) return 10;
    }

    // A source submission reports as a source, not as a publish. The loop picks
    // its status code off this - 202 for a stored source against 201 for a
    // published mesh - so a kind that came back wrong would answer the creator
    // with a receipt for an inventory item that does not exist.
    //
    // What this cannot reach is the follow-on: a *successful* store raises the
    // import job itself, and no store can succeed without a grid and a vault.
    // That chain is covered by the integration path, not here, and saying so
    // beats a test that looks like it covers it.
    {
        PublishQueue queue(failing_storage, failing_grid, "http://region.invalid");
        const auto id = queue.submit_source(some_bytes(128), "Body", "creator");
        const auto results = drain(queue, 1);
        if (results.size() != 1) return 11;
        if (results.front().id != id) return 12;
        if (results.front().kind != PublishQueue::Kind::StoreSource) return 13;
        if (results.front().ok) return 14;
        if (results.front().error.empty()) return 15;
    }

    // The name an import's parts folder takes. The creator recognises the file
    // they exported, so the name is theirs minus what they did not choose.
    {
        using homeworldz::mesh::source_folder_name;
        // The case Character Creator actually produces, capital extension and
        // a leading underscore the creator typed on purpose.
        if (source_folder_name("_HD Ariana.Fbx") != "_HD Ariana") return 16;
        if (source_folder_name("kevin.fbx") != "kevin") return 17;
        // A client that sends a path sends its own separators, either kind.
        if (source_folder_name("C:\\models\\CC4 Kevin.Fbx") != "CC4 Kevin") return 18;
        if (source_folder_name("models/Gibro.fbx") != "Gibro") return 19;
        // Dots inside the name belong to the name; only the last is an
        // extension. A name that is *only* an extension keeps it, because
        // ".fbx" is a stranger folder name than the alternative of nothing.
        if (source_folder_name("Aaron v1.2.fbx") != "Aaron v1.2") return 20;
        if (source_folder_name(".fbx") != ".fbx") return 21;
        // No name at all still has to produce a folder, since the parts have
        // to land somewhere the wearer can find them.
        if (source_folder_name("") != "Imported") return 22;
        if (source_folder_name("models/") != "Imported") return 23;
        // Inventory names are bounded, and an over-long one must be cut rather
        // than refused: the import has already stored its source by this point.
        if (source_folder_name(std::string(400, 'x') + ".fbx").size() != 255) return 24;
    }

    // Which imported meshes are the avatar and which are worn over it. Taken
    // from the corpus rather than invented: these are the names Character
    // Creator actually exported for Kevin, Gibro and Ariana.
    {
        using homeworldz::mesh::is_avatar_body_mesh;
        for (const auto* body : {"CC_Base_Body", "CC_Base_Eye", "CC_Base_Teeth",
                                 "CC_Base_Tongue", "CC_Base_TearLine", "CC_Base_EyeOcclusion",
                                 "CC_Game_Body", "Eyebrow", "Eyelash_Up", "Eyelash_Down"})
            if (!is_avatar_body_mesh(body)) return 25;
        // Clothing, and the accessories that come with it.
        for (const auto* outfit : {"Slim_Jeans", "Basic_T_shirts", "Sport_sneakers", "Boots",
                                   "Belt", "Apron", "Pants", "Glasses"})
            if (is_avatar_body_mesh(outfit)) return 26;
        // Hair and facial hair are outfit on purpose: they are worn, swapped
        // and removed the way clothing is, whatever they are made of.
        for (const auto* hair : {"Classic_short", "Mustache_Sparse", "Sideburns_Stubble",
                                 "Soul_Patch_Sparse", "Male_Brow_2"})
            if (is_avatar_body_mesh(hair)) return 27;
        // A name that merely contains a body prefix is not one: the test is
        // what the mesh starts with, or a jacket called "Eyebrow Bomber"
        // would file itself as a face.
        if (is_avatar_body_mesh("Retro CC_Base_Jacket")) return 28;
        if (is_avatar_body_mesh("")) return 29;
    }

    // A plain publish must still report as one, so the two are actually
    // distinguished rather than both defaulting to whatever the enum's first
    // value happens to be.
    {
        PublishQueue queue(failing_storage, failing_grid, "http://region.invalid");
        queue.submit(some_bytes(128), "Mesh", "creator");
        const auto results = drain(queue, 1);
        if (results.size() != 1) return 16;
        if (results.front().kind != PublishQueue::Kind::Publish) return 17;
    }

    return 0;
}
