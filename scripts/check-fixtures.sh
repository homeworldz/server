#!/usr/bin/env bash
# Run the mesh gate against real bodies, not synthesised ones.
#
# The unit tests build their GLBs inline, which makes them regression tests: a
# synthetic fixture can only fail in the ways its author imagined, so it proves
# a known trap stays fixed and discovers nothing (client core, 2026-08-08).
#
# Every genuine surprise in the mesh work came from a real body instead. The
# geometric check's first version refused the Second Life reference body over two
# joints that are declared and move nothing — a shape no fixture written that
# afternoon contained, by the person who had just written that failure mode up.
# A MakeHuman export moves 124 joints where the prediction was "far fewer than
# 110". Neither is in the suite, because the bodies are not ours to commit.
#
# So this is the discovery instrument, kept runnable rather than remembered. It
# is deliberately **loud when it cannot run**: a fixture check that quietly
# passes because it found no fixtures is the exact failure this repository spent
# 2026-08-08 learning about, and it would be worse here than anywhere.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fixtures=${HOMEWORLDZ_FIXTURES:-$root/../fixtures}
diag=${HOMEWORLDZ_MESH_DIAG:-$root/build/vcpkg/region/homeworldz-mesh-diag}
[[ -x "$diag" ]] || diag="$root/build/linux-release/region/homeworldz-mesh-diag"

if [[ ! -x "$diag" ]]; then
  echo "no homeworldz-mesh-diag; build it or set HOMEWORLDZ_MESH_DIAG" >&2
  exit 2
fi
if [[ ! -d "$fixtures" ]]; then
  echo "no fixture directory at $fixtures; set HOMEWORLDZ_FIXTURES" >&2
  echo "refusing to report success having checked nothing" >&2
  exit 2
fi

# file:expected-substring. The expectation is the *reason*, not just the verdict,
# because "refused" is satisfied by being refused for the wrong thing.
expectations=(
  "SLReference.glb:accepted"
  "makehuman-female.glb:not a joint of the bento-avatar skeleton"
  "makehuman-male.glb:not a joint of the bento-avatar skeleton"
)

checked=0
failed=0
for entry in "${expectations[@]}"; do
  name=${entry%%:*}
  expected=${entry#*:}
  path="$fixtures/$name"
  if [[ ! -f "$path" ]]; then
    echo "MISSING  $name (not checked)" >&2
    failed=$((failed + 1))
    continue
  fi
  # mesh-diag exits non-zero when anything was refused, which is an expected
  # outcome here, so the reason text decides rather than the status.
  output=$("$diag" "$path" 2>&1 || true)
  if grep -qF -- "$expected" <<<"$output"; then
    echo "ok       $name"
  else
    echo "FAILED   $name: expected \"$expected\"" >&2
    sed 's/^/           /' <<<"$output" >&2
    failed=$((failed + 1))
  fi
  checked=$((checked + 1))
done

if ((checked == 0)); then
  echo "checked nothing; that is a failure, not a pass" >&2
  exit 2
fi
echo "$checked fixture(s) checked, $failed failed"
((failed == 0))
