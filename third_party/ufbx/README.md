# ufbx (vendored)

The FBX reader of [ADR 0035](../../docs/adr/0035-server-side-source-format-import.md).

| | |
| --- | --- |
| Upstream | https://github.com/ufbx/ufbx |
| Version | `v0.23.0` |
| Commit | `fcc5d6ba444cfd3eb80677dba5e37e493941abe5` |
| Vendored | 2026-08-10 |
| Licence | MIT or Unlicense, at our option — see [LICENSE](LICENSE) |

Only `ufbx.h`, `ufbx.c` and `LICENSE` are taken. The upstream repository also
carries a test suite, bindings and a large corpus of FBX fixtures; none of it is
needed to build, and vendoring it would put tens of megabytes of other people's
sample files in this history.

## Why vendored rather than a vcpkg port

The region is configured with `-DVCPKG_MANIFEST_MODE=OFF` on the cloud box
(`scripts/build-region.sh`), so every vcpkg dependency is a package that has to
be installed there by hand before a build can succeed. ufbx is two files with no
dependencies of its own, and ADR 0035 chose it partly because it "adds no
package". Vendoring keeps that promise: the source tree that builds on Windows
is the source tree that builds on the cloud box.

## Local changes

None. The files are upstream's bytes, unmodified, so that the next update is a
re-download rather than a merge. Configuration that upstream expects to be done
by editing the source is done through compile definitions in
[CMakeLists.txt](CMakeLists.txt) instead.
