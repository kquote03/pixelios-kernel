#!/usr/bin/env bash
# pixui_probe (doc 56 Step 8): iOS arm64 cross build as audio/sweetaudio/build.sh (nix clang 21 + theos SDK + ldid), ad-hoc signed
# with pixui_probe.entitlements. Output (not in git): $A/uifix/out/pixui_probe.
set -euo pipefail
A=${A:-$HOME/pixelios-artifacts}
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=$A/localbuild/theos/sdks/iPhoneOS16.5.sdk
CLANG=${CLANG:-/nix/store/jskv39ga5njhv0hwv5yxjq56dmc5jjm8-clang-21.1.8/bin/clang}
OUT=$A/uifix/out
export PATH=$A/gpu/ios/bin:$PATH
mkdir -p "$OUT"
"$CLANG" --target=arm64-apple-ios16.0 -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
	-framework CoreFoundation -framework AudioToolbox -lobjc -o "$OUT/pixui_probe" "$HERE/pixui_probe.c"
ldid -S"$HERE/pixui_probe.entitlements" "$OUT/pixui_probe"
ls -l "$OUT/pixui_probe"; sha256sum "$OUT/pixui_probe"
