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

    return 0;
}
