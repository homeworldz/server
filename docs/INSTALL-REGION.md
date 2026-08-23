# Install a Homeworldz Region

This guide is for a **region owner** connecting a region to an existing
Homeworldz grid. You do not need PostgreSQL, its credentials, or access to the
grid database. If you operate the central grid too, complete the
`INSTALL-GRID.md` included in the grid package first.

Development snapshots can now be assembled as release ZIPs. Public release
publication and an installer remain future work.
Archive names, service metadata, and the packaged `VERSION` file are derived
from the repository's root `VERSION` source when the package is built.

## Information to obtain from the grid operator

Before installation, ask for:

- The grid API URL reachable from the region host.
- The grid public URL advertised to viewers.
- The assigned region UUID and its unique startup access key.
- The approved region name, X/Y map coordinates, and assigned size (1x1,
  2x2, or 4x4), for reference.
- The transitional service token used by non-registration grid APIs.
- The public hostname or address viewers will use for this region.
- Approved HTTP/TCP and viewer/UDP ports.

Treat both secrets as passwords. The access key authorizes only the assigned
region's runtime lease. The transitional service token permits other internal
region API calls. Neither belongs in screenshots, public logs, or source
control.

## Expected region package

The `homeworldz-region-<version>-windows-x64.zip` or future
`homeworldz-region-<version>-linux-x64.tar.gz` will contain a prebuilt region
program and its static assets:

```text
homeworldz-region/
  INSTALL-REGION.md
  INSTALL-GRID.md         # reference for operators hosting both services
  VERSION
  homeworldz-region[.exe]
  *.dll                 # Windows runtime libraries, when required
  config/
    examples/
      region.ini
      region-personal.ini
      region-cloud.ini
  assets/
    region/
      terrain/plateau-square.raw
      ...
  docs/
    FEATURES.md
    ROADMAP.md
  deploy/
    linux/
      homeworldz-region@.service
      region.env.example
```

Extract the package into a dedicated directory. The packaged region will not
require Go, Node.js, Visual Studio, CMake, vcpkg, PostgreSQL, or a compiler.

## Prepare data and backups

Create a private, writable data directory outside replaceable program files
when possible. The current default is `var/region`. At first startup the region
creates:

```text
var/region/
  region.db
  assets/
  logs/
  scene/
```

`terrain.f32` holds the current ground. `revert.f32`, when present, holds the
**baseline** the terrain edit limits are measured from and the revert brush
returns to — written when an estate manager presses *Bake Terrain* on the
viewer's Region/Estate → Terrain tab. Absent, the baseline comes from the
packaged RAW, which means the edit limits are measured against the shipped shape
rather than against ground the region has been sculpted into. Back both up with
the rest of the data directory.

Every region process requires a different data directory. `region.db`, the
content-addressed `assets` tree, and `scene/snapshot.json` together constitute
the local persistent state; back them up as one consistent set while the
region is stopped.

## Configure the region

Copy the packaged example matching the grid deployment to the live, private
configuration file. Use `region-personal.ini` with `grid-personal.ini`, or
`region-cloud.ini` with `grid-cloud.ini`:

```cmd
copy config\examples\region-personal.ini config\region.ini
```

Edit `config/region.ini` with the values supplied by the grid operator. The
region executable reads this file directly. Set its viewer-reachable public
endpoint, grid URLs, and transitional service token. The grid supplies the
authoritative name and map coordinates after validating the command-line
region UUID and access key.

Runtime configuration is file-only. The public grid URL is used in
viewer-facing responses, and the public region endpoint must be reachable by
viewers. Service managers should pass a different file explicitly with
`--config`; they do not construct process environments from individual
settings.

The region UUID, access key, and transitional service token are currently all
required for a connected region.

## Install as an Ubuntu service

Use one instance of the packaged systemd template per region. Program files are
shared, while configuration, credentials, and persistent state remain separate:

```text
/opt/homeworldz/region/                 # extracted package
/etc/homeworldz/regions/sandbox.ini     # host-local region settings
/etc/homeworldz/regions/sandbox.env     # UUID and access key
/var/lib/homeworldz/regions/sandbox/    # SQLite, scene, terrain, and assets
```

Create the directories and install the unit:

```sh
sudo install -d -o root -g root -m 0755 /opt/homeworldz/region
sudo install -d -o homeworldz -g homeworldz -m 0700 /etc/homeworldz/regions
sudo install -d -o homeworldz -g homeworldz -m 0750 /var/lib/homeworldz/regions/sandbox
sudo install -o root -g root -m 0644 \
  /opt/homeworldz/region/deploy/linux/homeworldz-region@.service \
  /etc/systemd/system/homeworldz-region@.service
sudo systemctl daemon-reload
```

Copy `config/examples/region-cloud.ini` to
`/etc/homeworldz/regions/sandbox.ini`. Set `region.data_path` to
`/var/lib/homeworldz/regions/sandbox`, keep static asset paths relative to the
unit's `/opt/homeworldz/region` working directory, and set the public endpoint,
ports, bind addresses, grid URL, and transitional service token.

Copy `deploy/linux/region.env.example` to
`/etc/homeworldz/regions/sandbox.env`, replace both values with the UUID and
access key assigned by the grid operator, then protect both files:

```sh
sudo chown homeworldz:homeworldz /etc/homeworldz/regions/sandbox.ini \
  /etc/homeworldz/regions/sandbox.env
sudo chmod 0600 /etc/homeworldz/regions/sandbox.ini \
  /etc/homeworldz/regions/sandbox.env
sudo systemctl enable --now homeworldz-region@sandbox
```

Additional instances use another short instance name, distinct configuration,
credentials, data directory, TCP/UDP ports, and grid-provisioned identity.

## Start and stop

Start in a terminal for the first run so registration or storage errors remain
visible:

Windows:

```cmd
homeworldz-region.exe --config config\region.ini --region-name "REGION NAME" --access-key REGION-ACCESS-KEY
```

Linux:

```sh
./homeworldz-region --config config/region.ini --region-name "REGION NAME" --access-key REGION-ACCESS-KEY
```

Run it from the extracted package directory so relative asset and terrain paths
resolve. Stop it with Ctrl+C. A future installer is expected to add
Windows-service and systemd integration, but neither is implemented yet.

The startup log should report terrain loading, local storage initialization,
successful grid registration, and both listening ports. When a service token
is configured, the process exits if registration fails.

`--region-id REGION-UUID` may be used instead of `--region-name`; exactly one
identifier is required. Names are matched case-insensitively and the Grid
returns the canonical UUID, name, coordinates, assigned public endpoint and
viewer port, Grid display name, and public Grid URL. Operator-assigned endpoint
values override their transitional local INI counterparts before the viewer
socket opens. Host-local bind addresses and storage paths remain local.
The Grid assigns exactly one of three square extents: 1x1 (256 metres), 2x2
(512 metres), or 4x4 (1024 metres). The Region constructs terrain storage,
Jolt bounds, viewer terrain packets, object/avatar coordinate bounds, map
output, and teleport validation from that authenticated assignment. No local
size setting can disagree with the authoritative provisioned record.

A packaged 256x256 RAW default terrain is scaled over a larger new Region.
An exact-width RAW file may instead contain one unsigned height byte per metre
(512x512 or 1024x1024). Persistent `terrain.f32` contains one little-endian
float per metre and is accepted only when its dimensions match the current
assignment. Changing the provisioned size therefore requires a stopped Region,
a backup, and intentional terrain migration or regeneration.

Default endpoints:

- Region capabilities and operational HTTP: `127.0.0.1:42001/tcp`
- Viewer circuit: `127.0.0.1:42002/udp`
- Liveness: `http://127.0.0.1:42001/ping`
- Readiness: `http://127.0.0.1:42001/ready`
- Version: `http://127.0.0.1:42001/version`

Verify locally with `curl.exe --fail http://127.0.0.1:42001/ready` on Windows
or `curl --fail http://127.0.0.1:42001/ready` on Linux. Ask the grid operator to
confirm registration at the assigned coordinates before inviting viewers.

## Runtime settings

| INI setting | Purpose | Default |
| --- | --- | --- |
| `region.http_port` | Region HTTP port | `42001` |
| `region.viewer_port` | Viewer UDP port | `42002` |
| `region.bind_address` | Region HTTP IPv4 bind address | `127.0.0.1` |
| `region.viewer_bind_address` | Viewer UDP IPv4 bind address | `127.0.0.1` |
| `region.public_endpoint` | Viewer-reachable region HTTP URL | `http://localhost:42001` |
| `region.lease_seconds` | Registration lease (10–300 seconds) | `60` |
| `grid.url` | Grid API base URL | `http://localhost:42000` |
| `grid.public_url` | Viewer-facing grid base URL | Grid URL |
| `grid.service_token` | Transitional internal-API authentication secret | Required |
| `region.data_path` | Local scene and uploaded-asset state | `var/region` |
| `region.asset_path` | Static assets imported at startup | `assets/region` |
| `region.terrain_path` | Assigned-width byte RAW terrain; a packaged 256×256 default is scaled when needed | `assets/region/terrain/plateau-square.raw` |
| `region.walkable_slope_degrees` | Steepest ground an avatar can **walk up or along, and stand on** (10–89) — Jolt's `CharacterVirtual` max slope angle. **It bounds both, since 2026-08-08.** Ground steeper than this is Jolt's `OnSteepGround`: not supported, so gravity applies and the avatar descends under the existing fall path. Before that it bounded traversal only — `grounded` was `IsSupported()`, which is true for `OnSteepGround` as well as `OnGround`, so an avatar rested motionless on ground it could never walk, measured repeatably at 70°. Note the framing: the resting bound was not absent, it was infinite, and this moves it to the same angle rather than adding a limit where there was none. Published to session clients in the hello, with `slopeLimitGoverns` and `restingBoundedBySlope` beside it so the scope is stated rather than inferred | `65` (Halcyon's limit; the *walking* was confirmed in-world 2026-07-30) |
| `region.smooth_strength_percent` | How fast the smooth brush converges on the local average per application (1-100). A feel constant only: smoothing lerps toward a bounded target, so the rate decides how many applications level a feature, never whether it overshoots. The brush averages over a fixed 3x3 sample neighbourhood, so strength changes how fast it converges, not how far it reaches | `50` |
| `region.water_height` | **Starting** height in metres of the region's flat, region-wide water plane, in the same vertical datum as terrain heights (0–4096). Viewers read it from `RegionHandshake`, session clients from the hello's `water` block. Since 2026-08-04 an estate manager can change it per region from the viewer's Region/Estate → Terrain tab, and the change is persisted — so this setting is what a region starts at, not what it is. Clear the stored value by deleting the `region_settings` row | `20` |
| `region.terrain_raise_limit` / `region.terrain_lower_limit` | Not INI settings. How far an edit may move the ground from the region's original height, above and below, set from the viewer's Region/Estate → Terrain tab and persisted per region. The region announced 100 and −100 in `RegionInfo` and enforced neither until 2026-08-04, so both fields read as settings while being decoration. Measured against the region's baseline rather than the current height, so repeated edits cannot walk past them | `100` / `-100` |
| `region.welcome_message` | Optional region-specific line sent privately to each avatar as it enters, including on border crossings — set it only when a region has something worth repeating every entry ("You are in Sandbox, the build area"). The arrival greeting proper is `[grid] welcome_message` on the grid, delivered once per login. `{region}` and `{user}` resolve to the region name and display name | *(none — silent)* |

| `region.release_notes_url` | Where the viewer's ServerReleaseNotes capability redirects. Answered as a 302 rather than a body | `https://homeworldz.com/roadmaps/server` |
| `region.child_agents` | Set to `off` to stop this region announcing itself to neighbouring viewers and holding standing child circuits ([ADR 0038](adr/0038-cross-region-child-agents.md)). Any other value leaves it on. Until 2026-08-23 the code read this as a bare `child_agents`, which the parser can never produce — the switch was unreachable and writing it in an ini killed the region at startup | `on` |

Every setting in this table is accepted by the region config parser, and an
unrecognised key is a **fatal** startup error rather than a warning. Six of
the rows above were readable in code and refused by the parser until
2026-08-23, so following this guide produced a region that would not start.
There is now a test asserting each one parses.

`region.name`, `region.grid_x` and `region.grid_y` are accepted and
**ignored**: a region takes its name and coordinates from the grid's
registration reply. Existing configs set them, so they remain accepted rather
than becoming a startup failure.

Bind addresses must be IPv4 addresses. Invalid numeric values fall back to
their defaults.

## Firewall and remote viewers

The loopback defaults work only when the viewer is on the region host and avoid
firewall prompts. For remote viewers:

1. Set both bind addresses to the host's specific IPv4 address, or deliberately
   use `0.0.0.0` when all interfaces are intended.
2. Open the chosen region HTTP **TCP** port and viewer circuit **UDP** port.
3. Forward both ports through NAT when applicable.
4. Put the reachable host and HTTP port in the public region endpoint.
5. Restrict firewall rules as narrowly as the deployment permits.

Do not add PostgreSQL access or broad executable exceptions for a region host.
Voice is not initially provided by Homeworldz; a viewer may still prompt for
its own voice executable, but that is separate from the two region ports.

The range `42010`–`42099` is reserved for additional local regions and
development services. Each additional region needs a provisioned UUID and
access key plus unique HTTP and UDP ports, public endpoint, and data path.

## Upgrades and restoration

Before upgrading:

1. Notify the grid operator and log out viewers.
2. Stop the region cleanly.
3. Back up the entire configured data directory as one set.
4. Back up the region settings and service token securely.
5. Retain the old program package for rollback.

Extract the replacement package separately, restore the settings, point it at
the existing data directory, and start it. Verify `/version`, `/ready`, grid
registration, terrain, and an avatar login before deleting the old package.
Never overwrite the only copy of the data directory during an upgrade.

To restore, stop the process, move the damaged data directory aside, restore
the complete stopped-state backup, and restart. Coordinate any rollback that
changes identity or grid coordinates with the grid operator.

Building development snapshots from source is documented in the repository
README and is intentionally outside this region-owner installation guide.
