<div style="text-align: left; width: 320px; margin: 0;">
  <img src="homeworldz.svg" alt="Homeworldz logo" width="320" style="display: block; margin: 0;">
</div>

Homeworldz is a clean-architecture virtual world server targeting practical
Firestorm compatibility. It is a new implementation informed by Halcyon,
OpenSimulator, and the Second Life viewer protocol without preserving their
internal service boundaries or storage formats.

The architecture uses a C++20 region server, a Go grid service, HTTP/JSON APIs
described by OpenAPI, Postgres for central state, and region-local scene and
asset storage.

The functional single-region world is largely in place: Firestorm login,
authoritative avatar movement, Jolt avatar and object physics, live terrain
editing, AIS v3 inventory, object rez/edit/permissions/persistence, and an
early Second Life LSL subset running on the purpose-built Falcon VM all work
against the supported viewer. Connected multi-region support, the interactive
physical world, and the Falcon scripting runtime are actively progressing. See
the [roadmap](docs/ROADMAP.md) for the current phase-by-phase status.

## Development

Central grid hosts require Go 1.21 or newer and a reachable PostgreSQL 16 or
newer installation. PostgreSQL 18.4 is recommended for new installations.
Region builds require a C++20 toolchain with CMake 3.24 or newer. PostgreSQL may
run locally or on another reachable host; Homeworldz does not require Docker.

Runtime configuration uses ordinary INI files in `config/`. Start from the
matching files in `config/examples/`: `grid.ini` controls grid server and client
settings, `db.ini` contains central database access, and `region.ini` controls
one region instance. Files directly under `config/` are ignored by Git because
they may contain credentials. INI files are the only source of configuration:
environment variables are deliberately not consulted, for the grid as well as
the region ([ADR 0005](docs/adr/0005-external-postgres.md)).

After installing PostgreSQL, grid operators bootstrap the application role,
database, all pending migrations, and ignored `config/db.ini`. The cross-platform Go
command securely prompts for the PostgreSQL administrator password and the
password to assign to the `homeworldz` application role:

```text
go run ./grid/cmd/bootstrap-grid
```

Use `-host`, `-port`, or `-admin-user` when PostgreSQL is remote or uses
non-default connection settings. The command uses the Go PostgreSQL driver and
does not require `psql` on `PATH`.

Region operators prepare region-local scene, asset, and log storage separately:

```powershell
.\scripts\bootstrap-region.ps1
```

Region operators do not need PostgreSQL credentials. The region will use its
local storage and authenticated grid APIs once those components are implemented.

Run grid tests from `grid/` with `go test ./...`. The grid service listens on
`127.0.0.1:42000` by default and reads `config/grid.ini` and `config/db.ini`.
The region HTTP and viewer UDP services also bind to `127.0.0.1` by default, on
ports `42001` and `42002` respectively. Loopback defaults avoid exposing local
development services or requiring Windows Firewall exceptions.

Firestorm discovers the local grid from `http://127.0.0.1:42000/` through the
standard `/get_grid_info` endpoint. Set `[server] public_url` in
`config/grid.ini` when the grid is exposed at a different address.

PostgreSQL lifecycle tests run when `HOMEWORLDZ_TEST_DATABASE_URL` is set and
otherwise skip cleanly. When the GitHub Actions workflow runs it supplies a
disposable PostgreSQL service and runs the region lease, identity/session, and
presence cleanup suites against it. Because the account's included Actions
minutes are limited and pushes are frequent, that workflow runs at most once
per day on a schedule (and on manual dispatch) rather than on every push, so
primary verification is local: run the Go and region suites on your workstation
and Linux build host and do not block on CI.

After pulling new database migrations into an already bootstrapped checkout,
apply them without re-entering the PostgreSQL administrator credentials:

```cmd
go run ./grid/cmd/bootstrap-grid -migrations-only
```

The shared Library identity is installed in a locked state. To assign local
interactive credentials to `Homeworldz Library`, run:

```cmd
go run ./grid/cmd/configure-library
```

Terrain heightmap images use the OpenSimulator/Halcyon convention and must be
lossless PNG files with dimensions matching the target region. Convert a 1x1
terrain image to the region service's current raw format with:

```cmd
go run ./grid/cmd/convert-terrain-image -input terrain.png -output terrain.raw
```

The importer flips image rows into terrain coordinates and maps HSL lightness
to the 0-to-128-metre range. JPEG terrain input is intentionally rejected
because compression artifacts become height spikes.

On Windows with Visual Studio, install the pinned vcpkg dependencies and build
the region service with:

```powershell
.\scripts\build-region.ps1 -Test
```

On Linux, install the compiler, CMake, Ninja, SQLite development files, and
the pinned Jolt 5.5 dependency. One reproducible Ubuntu setup uses the
repository's vcpkg baseline in classic mode so the optional PhysX evaluation
adapter is not pulled into the production build:

```sh
sudo apt-get install build-essential cmake ninja-build libsqlite3-dev pkg-config zip unzip curl
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
git -C "$HOME/vcpkg" checkout f87344cac03158cbf1467264565f1fd36b382a24
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
"$HOME/vcpkg/vcpkg" install --classic joltphysics:x64-linux
export VCPKG_ROOT="$HOME/vcpkg"
./scripts/build-region.sh --test
```

The script deliberately fails if Jolt is unavailable rather than producing a
region binary without production physics. Pass `--version VERSION` for a
release build whose embedded `/version` value must match its package name.

The grid provisions region identity and map placement in `config/regions.json`.
Start a region with its assigned credentials using `homeworldz-region --config
path/to/region.ini --region-id UUID --access-key KEY`. The INI holds host-local
endpoints, bind addresses, ports, data paths, the grid URL, and the transitional
internal service token. Individual environment overrides are intentionally
unsupported.

## Documentation

- [Feature differences](docs/FEATURES.md)
- [Install a packaged grid](docs/INSTALL-GRID.md)
- [Install a packaged region](docs/INSTALL-REGION.md)
- [Release packaging](docs/PACKAGING.md)
- [Project roadmap](docs/ROADMAP.md)
- [Implementation plan](docs/PLAN.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Asset storage and federation](docs/ASSETS.md)
- [Physics evaluation](docs/PHYSICS.md)
- [Scripting architecture](docs/SCRIPTING.md)
- [Falcon VM](docs/VM.md)
- [Firestorm compatibility](docs/FIRESTORM.md)

## License

Homeworldz is open source under the [MIT License](LICENSE).
