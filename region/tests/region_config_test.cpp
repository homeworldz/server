#include "homeworldz/region_config.h"

#include <stdexcept>
#include <string_view>

int main() {
    constexpr std::string_view input = R"ini(
# packaged region configuration
[region]
name = Demo Region
grid_x = 1001
grid_y = 1002
public_endpoint = http://region.example:42001
http_port = 42001
viewer_port = 42002
bind_address = 0.0.0.0
viewer_bind_address = 0.0.0.0
data_path = D:\Homeworldz\region
asset_path = assets/region
terrain_path = assets/region/terrain/plateau-square.raw
lease_seconds = 60
session_port = 42061
session_public_url = wss://grid.example/session/welcome

[grid]
url = https://grid.example
public_url = https://grid.example
service_token = secret
)ini";
    const auto settings = homeworldz::config::parse_region_ini(input);
    if (settings.size() != 17 || settings.at("region.name") != "Demo Region" ||
        settings.at("region.grid_x") != "1001" || settings.at("region.grid_y") != "1002" ||
        settings.at("region.public_endpoint") != "http://region.example:42001" ||
        settings.at("region.http_port") != "42001" || settings.at("region.viewer_port") != "42002" ||
        settings.at("region.bind_address") != "0.0.0.0" ||
        settings.at("region.viewer_bind_address") != "0.0.0.0" ||
        settings.at("region.data_path") != "D:\\Homeworldz\\region" ||
        settings.at("region.asset_path") != "assets/region" ||
        settings.at("region.terrain_path") != "assets/region/terrain/plateau-square.raw" ||
        settings.at("region.lease_seconds") != "60" ||
        settings.at("region.session_port") != "42061" ||
        settings.at("region.session_public_url") != "wss://grid.example/session/welcome" ||
        settings.at("grid.url") != "https://grid.example" ||
        settings.at("grid.public_url") != "https://grid.example" ||
        settings.at("grid.service_token") != "secret")
        return 1;
    // Every setting docs/INSTALL-REGION.md documents, and the ADR 0038 kill
    // switch, must PARSE. Each of these was live in the code and fatal in an
    // ini until 2026-08-23, so an operator following the install guide got a
    // region that refused to start.
    constexpr std::string_view documented = R"ini(
[region]
welcome_message = You are in Sandbox, the build area
smooth_strength_percent = 50
walkable_slope_degrees = 65
water_height = 20
release_notes_url = https://homeworldz.com/roadmaps/server
child_agents = off
)ini";
    const auto extras = homeworldz::config::parse_region_ini(documented);
    if (extras.size() != 6 ||
        extras.at("region.welcome_message") != "You are in Sandbox, the build area" ||
        extras.at("region.smooth_strength_percent") != "50" ||
        extras.at("region.walkable_slope_degrees") != "65" ||
        extras.at("region.water_height") != "20" ||
        extras.at("region.release_notes_url") != "https://homeworldz.com/roadmaps/server" ||
        // Read as region.child_agents, not a bare child_agents: the settings
        // map is keyed section.key, so the bare form the code used could never
        // match anything the parser produced and the switch was unreachable.
        extras.at("region.child_agents") != "off")
        return 1;
    try {
        static_cast<void>(homeworldz::config::parse_region_ini("[region]\nunknown = value\n"));
        return 1;
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(homeworldz::config::parse_region_ini("name = missing-section\n"));
        return 1;
    } catch (const std::runtime_error&) {
    }
    return 0;
}
