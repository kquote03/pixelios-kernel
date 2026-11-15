#!/usr/bin/env bash
# G3: wrappers for the theos cctools (ld64-609, ldid, libtool, lipo, otool, nm) so they run outside the FHS env:
# they need libtinfo/libuuid/libstdc++/zlib, taken from nixpkgs. Output: $A/gpu/ios/bin/<tool>. clang finds "ld" there via PATH.
set -euo pipefail
A=${A:-$HOME/pixelios-artifacts}
T=$A/localbuild/theos/toolchain/linux/iphone/bin
mkdir -p "$A/gpu/ios/bin"
L=""
for a in ncurses libuuid.lib stdenv.cc.cc.lib zlib; do L="$L:$(nix-build --no-out-link '<nixpkgs>' -A $a | tail -1)/lib"; done
for tool in ld ldid libtool lipo otool nm; do
	printf '#!/bin/sh\nLD_LIBRARY_PATH=%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} exec %s/%s "$@"\n' "${L#:}" "$T" "$tool" > "$A/gpu/ios/bin/$tool"
	chmod +x "$A/gpu/ios/bin/$tool"
done
"$A/gpu/ios/bin/ld" -v 2>&1 | head -1
