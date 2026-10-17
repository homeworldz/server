#include "homeworldz/seating.h"

#include <cmath>
#include <iostream>

using homeworldz::region::choose_seat;
using homeworldz::region::link_order;
using homeworldz::region::resolve_link;
using homeworldz::region::seat_pose;
using homeworldz::region::seated_world;
using homeworldz::region::SeatCandidate;
using homeworldz::scene::Vector3;

namespace {

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
bool near(const Vector3& a, const Vector3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    // Seat choice (ADR 0040), on a sofa of root 10 and children 11 and 12.
    {
        const std::vector<SeatCandidate> sofa{{10, false, false}, {11, true, false}, {12, true, false}};
        check(choose_seat(12, sofa) == 12u, "the clicked prim's free sit target");
        check(choose_seat(10, sofa) == 11u, "clicking a prim without one takes the first free target");
        std::vector<SeatCandidate> one_left{{10, false, false}, {11, true, true}, {12, true, false}};
        check(choose_seat(11, one_left) == 12u, "a taken target passes to the next free one");
        std::vector<SeatCandidate> full{{10, true, true}, {11, true, true}};
        check(!choose_seat(10, full), "every target taken refuses, rather than stacking sitters");
        std::vector<SeatCandidate> plain{{20, false, false}};
        check(choose_seat(20, plain) == 20u, "a prim with no sit target gives a default seat");
        std::vector<SeatCandidate> plain_taken{{20, false, true}};
        check(!choose_seat(20, plain_taken), "a default seat holds one avatar too");
        check(!choose_seat(99, plain), "a prim outside the linkset seats no one");
    }

    // Link order and LSL link numbers.
    {
        const auto order = link_order(7, {12, 9, 7, 30});
        check(order == std::vector<std::uint64_t>{7, 9, 12, 30}, "root first, then children by id");
        check(resolve_link(1, 12, order) == std::vector<std::uint64_t>{7}, "LINK_ROOT");
        check(resolve_link(3, 7, order) == std::vector<std::uint64_t>{12}, "link 3");
        check(resolve_link(-4, 12, order) == std::vector<std::uint64_t>{12}, "LINK_THIS");
        check(resolve_link(-1, 12, order).size() == 4, "LINK_SET");
        check(resolve_link(-2, 12, order) == std::vector<std::uint64_t>{7, 9, 30}, "LINK_ALL_OTHERS");
        check(resolve_link(-3, 7, order) == std::vector<std::uint64_t>{9, 12, 30}, "LINK_ALL_CHILDREN");
        check(resolve_link(5, 7, order).empty(), "a link number past the end names nothing");
        check(resolve_link(0, 7, {7}) == std::vector<std::uint64_t>{7},
              "a single prim is link 0 as well as LINK_ROOT");
    }

    // The pose sent to the viewer, relative to the root prim.
    {
        // A sit target on the root, hip offset -0.1: the avatar is raised 0.1.
        const auto root_seat = seat_pose({}, {}, true, {0.0, 0.0, 0.5}, {}, -0.1);
        check(near(root_seat.position, {0.0, 0.0, 0.6}), "the hip offset raises a root seat");
        // No sit target: the viewer's default offset.
        const auto plain = seat_pose({}, {}, false, {}, {}, 0.0);
        check(near(plain.position, {0.34, 0.0, 0.55}), "the default seat is the viewer's own");
        // A child seat 1 m east of the root, turned 90 degrees about z, with a
        // sit target 0.5 m along its own x: that is 0.5 m north of the child.
        const double s = std::sqrt(0.5);
        const auto child = seat_pose({1.0, 0.0, 0.0}, {0.0, 0.0, s}, true, {0.5, 0.0, 0.0}, {}, 0.0);
        check(near(child.position, {1.0, 0.5, 0.0}), "a child seat is carried into the root's frame");
        check(near(child.rotation, {0.0, 0.0, s}), "and faces the way the child does");
    }

    // The seated world transform follows the root.
    {
        const double s = std::sqrt(0.5);
        const auto world = seated_world({100.0, 50.0, 20.0}, {0.0, 0.0, s}, {{1.0, 0.0, 0.5}, {}});
        check(near(world.position, {100.0, 51.0, 20.5}), "a seat moves and turns with its root");
        check(near(world.rotation, {0.0, 0.0, s}), "and the avatar turns with it");
    }

    // Standing clears the top of the seat.
    {
        homeworldz::scene::Entity stool;
        stool.position = {10.0, 10.0, 21.0};
        stool.scale = {0.5, 0.5, 1.0};
        const auto standing = homeworldz::region::stand_position({10.0, 10.0, 21.4}, stool, 1.8);
        check(near(standing.z, 21.5 + 0.9), "standing puts the avatar above the stool");
        check(near(standing.x, 10.0) && near(standing.y, 10.0), "where it was sitting");
    }

    if (failures != 0) return 1;
    std::cerr << "seating OK\n";
    return 0;
}
