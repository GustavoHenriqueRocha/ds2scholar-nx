#!/usr/bin/env bash
# Linux twin of tools/codegen.ps1: translates the game (default.xex) to C++ with the host code generator.
# Build the generator first: (cd sdk && cmake --preset linux-amd64 && cmake --build --preset linux-amd64-release --target rexglue)
#
#   tools/codegen.sh [classic|optimised]      (default: optimised, as codegen.ps1)
#
# Input:  assets/game_root/Default.xex, app/ds2_manifest.toml.  Output: app/generated/, out/codegen.log.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
flavor="${1:-optimised}"
rexglue="${REXGLUE:-$root/sdk/out/linux-amd64/Release/rexglue}"
[ -x "$rexglue" ] || { echo "rexglue not found: $rexglue (build it first, see the header)" >&2; exit 1; }
[ -f "$root/assets/game_root/Default.xex" ] || { echo "assets/game_root/Default.xex not found" >&2; exit 1; }
mkdir -p "$root/out"

flavor_toml="$root/app/codegen_flavor.toml"
case "$flavor" in
  optimised)
    { printf '# optimised codegen: written by tools/codegen.sh optimised from perf_overrides.toml.\n'
      cat "$root/app/perf_overrides.toml"; } | tr -d '\r' > "$flavor_toml" ;;
  classic)
    printf '# classic codegen: no extra options (tools/codegen.sh classic writes this file). Included by ds2_manifest.toml.\n' \
      > "$flavor_toml" ;;
  *) echo "flavor: classic or optimised" >&2; exit 2 ;;
esac

log="$root/out/codegen.log"
echo "== codegen ($flavor) with $rexglue"
(cd "$root/app" &&
  REX_MAX_JUMP_TABLE_ENTRIES=1024 REXGLUE_DUMP_IMAGE="$root/out/default_image.bin" \
  "$rexglue" codegen ds2_manifest.toml > "$log" 2>&1) || { tail -30 "$log"; echo "codegen failed, see out/codegen.log" >&2; exit 1; }
tail -30 "$log"

# The flavour's post-codegen steps, the same script the build appends to its own codegen command.
REXSDK_DIR="$root/sdk" python3 "$root/tools/post_codegen.py"
echo "done ($flavor codegen): $root/app/generated"
