# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This repo holds the Homeworldz server software: the **grid** server (Go,
`grid/`), the **region** server (C++/Jolt, `region/`), the **Falcon** script VM
(C++, `falcon/`), database migrations (`db/migrations/`), and the architecture
documentation and ADRs (`docs/`).

`falcon/` is the script VM's source; `scripts/` is build and ops shell scripts.
They were `script/` and `scripts/` until the near-collision cost a reader once
too often. Inside `falcon/`, the CMake target is still `homeworldz-script` and
the headers are still `homeworldz/script/…`: the subsystem is the script VM, and
Falcon is what the language and its compiler are called.

## Sibling repos are independent

The other repos under `homeworldz/` — the public website (`../homeworldz.com`),
the first-party client (`../client`), and `../freebies` — are **separate,
independent projects**. They are related, but work in this repo must not modify
them without **specific confirmation** each time; the user runs parallel
sessions in each folder and cross-repo edits interfere with work in flight.
Reading them is expected and fine.

The one tool here that deliberately crosses that border is `node syncweb.mjs`
(see Documentation below). It writes only
`../homeworldz.com/content/roadmaps/SERVER.md` and leaves it unstaged; do not
stage, commit, or push in the website repo.

## Naming and terminology

**[docs/STYLE.md](docs/STYLE.md) is authoritative** — read it before writing
prose, log messages, CLI output, or user-visible strings.

The rules most easily got wrong:

- The name is **`Homeworldz`** in prose. Never `HomeWorldz`.
- Identifiers stay lowercase (`homeworldz::physics::World`, `homeworldz-region`,
  the `homeworldz` role) and environment variables upper snake (`HOMEWORLDZ_*`).
  That is language convention, not a stylization of the name.
- **"viewer"** means a third-party Second Life-lineage viewer (Firestorm and its
  peers), per [ADR 0016](docs/adr/0016-firestorm-compatibility-target.md). It
  never means the first-party client.
- The first-party client of [ADR 0030](docs/adr/0030-client-architecture.md) is
  just **Homeworldz**, or the **Homeworldz client** where the distinction from
  server software matters. **"frontend"** is reserved for that client's
  rendering layers.

## Build and test

Go (grid):

```
cd grid && go test ./...
```

C++ (region and script) needs the MSVC environment — `cmake` is not on `PATH`,
and building without `vcvars64.bat` fails to find the standard library:

```
cmd /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat\" >nul && cmake --build build\vcpkg --target <target>"
```

Test binaries land in `build/vcpkg/region/` and `build/vcpkg/falcon/` and are run
directly; exit code 0 is a pass. On the cloud box the region is built natively
with `scripts/build-region.sh`, never cross-compiled from Windows.

## Documentation

- ADRs in `docs/adr/` record decisions and are numbered sequentially; they state
  intent and are revised as evidence arrives rather than rewritten silently.
- `docs/ROADMAP.md` is vendored into the website repo. After editing it, run
  `node syncweb.mjs`, which copies it to
  `../homeworldz.com/content/roadmaps/SERVER.md`
  and leaves the change unstaged there for review.
- Markdown and plain text use native line endings (CRLF on Windows).
