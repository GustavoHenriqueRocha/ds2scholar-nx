#!/usr/bin/env bash
# Linux twin of tools/win/build_switch.ps1: builds the Nintendo Switch NRO (out/nx/ds2.nro) with devkitA64 GCC + libnx and
# the static NVK from the Mesa Horizon SDK. The generated code must already exist (tools/codegen.sh).
#
#   tools/build_switch.sh [-j N] [--reconfigure] [--build-dir out/nx] [-D NAME=VALUE ...]
#
# NVK_SDK: the Mesa SDK folder with lib/libvulkan.a (nfsmw-nx's mesa/linux/build.sh makes
#   <mesa-switch>/mesa-linux-install/opt/devkitpro/portlibs/switch). DEVKITPRO: default /opt/devkitpro.
# GLSLANG: host glslangValidator (default: the system one; SPIR-V is portable).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
jobs=2  # notebook: 2 jobs (temperature); the author used 5 (8 ran cc1plus out of memory)
reconfigure=0
build_dir=out/nx
extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    -j) jobs="$2"; shift 2 ;;
    --reconfigure) reconfigure=1; shift ;;
    --build-dir) build_dir="$2"; shift 2 ;;
    -D) extra+=("-D$2"); shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
nvk="${NVK_SDK:-$HOME/projetos/externos/mesa-switch/mesa-linux-install/opt/devkitpro/portlibs/switch}"
[ -f "$nvk/lib/libvulkan.a" ] || { echo "NVK SDK not found: $nvk/lib/libvulkan.a (see the header)" >&2; exit 1; }
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
glslang="${GLSLANG:-$(command -v glslangValidator)}"
build="$root/$build_dir"
if [ "$reconfigure" = 1 ] || [ ! -f "$build/CMakeCache.txt" ]; then
  cmake -S "$root/app" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_TOOLCHAIN_FILE=$root/tools/switch/cmake/switch-devkitA64.cmake" \
    "-DREXGLUE_SWITCH_NVK_SDK=$nvk" "-DDS2_GLSLANG_VALIDATOR=$glslang" "-DDEVKITPRO=$DEVKITPRO" \
    "${extra[@]}"
fi
cmake --build "$build" -j "$jobs"
