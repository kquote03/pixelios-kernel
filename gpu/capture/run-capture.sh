#!/usr/bin/env bash
# G1 (docs/research/52): build the host Mesa with gpu/mesa-patches, record clear + triangle, finalize, hash.
# Usage (host, NixOS): gpu/capture/run-capture.sh   (Mesa 26.2.5 clone at $A/gpu/src/mesa with the patches applied)
set -euo pipefail
A=${A:-$HOME/pixelios-artifacts}
G=$(cd "$(dirname "$0")" && pwd)
B=$A/gpu/build/mesa
cd "$A/gpu"
[ -d "$B" ] || nix-shell nix/shell.nix --run "bash $G/../nix/configure-host.sh"
flock "$A/scratch/heavy.lock" nix-shell nix/shell.nix --run "ninja -C $B -j6"
S=$A/gpu/src/mesa
nix-shell nix/shell.nix --run "cc -O1 -g -Wall -I$S/include $G/sweetgl_capture.c -L$B/src/egl -L$B/src/mesa/glapi/es2api -lEGL -lGLESv2 -Wl,-rpath,$B/src/egl:$B/src/mesa/glapi/es2api -o bin/sweetgl_capture"
mkdir -p capture/raw && rm -f capture/raw/*.malijob
nix-shell nix/shell.nix --run "LD_PRELOAD=$B/src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so PAN_GPU_ID=9202 PAN_SWEET_RECORD=$A/gpu/capture/raw PAN_MESA_DEBUG=linear,nocrc ./bin/sweetgl_capture"
T=$B/src/panfrost/tools/sweet_malijob
$T capture/raw/cap000.malijob --out capture/clear.malijob --uniform 0xff996633 2>/dev/null
$T capture/raw/cap001.malijob --out capture/tri.malijob --px 2,2,0xff996633 --px 253,253,0xff996633 --px 128,128,0xff00ff00 2>/dev/null
for f in clear tri; do $T capture/$f.malijob > capture/$f.summary.txt 2> capture/$f.pandecode.txt; done
sha256sum capture/*.malijob
