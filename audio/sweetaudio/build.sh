#!/usr/bin/env bash
# SweetAudio (doc 54): host test, iOS arm64 cross build (nix clang 21 + theos ld64/ldid, as gpu/ios), overlay staging.
#   audio/sweetaudio/build.sh [MODE [tone]]   MODE = driver (default) | driver+va | restore | status; "tone" also runs
#                                             sweetaudio_tone 2 440 after the install step (needs the kernel /dev/sweetaudio)
# Output (not in git): $A/audio/out/{SweetAudio.driver/,sweetaudio_install,sweetaudio_tone}, overlay dir $A/audio/ov-sa-MODE[-tone]
# and its archive $A/audio/ov-sa-MODE[-tone].bin. AudioServerPlugIn.h / AudioHardwareBase.h are not in the iOS SDK: they are
# taken from the macOS 14 SDK on this host through a symlink include dir (no Apple header is copied into git).
set -euo pipefail
A=${A:-$HOME/pixelios-artifacts}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
MODE=${1:-driver}
TONE=${2:-}
case $MODE in driver|driver+va|restore|status) ;; *) echo "usage: $0 [driver|driver+va|restore|status [tone]]" >&2; exit 2;; esac
case $TONE in ''|tone|play) ;; *) echo "usage: $0 [driver|driver+va|restore|status [tone|play]]" >&2; exit 2;; esac
SDK=$A/localbuild/theos/sdks/iPhoneOS16.5.sdk
MACSDK=$A/localbuild/sdk/MacOSX14.0.sdk
CLANG=${CLANG:-/nix/store/jskv39ga5njhv0hwv5yxjq56dmc5jjm8-clang-21.1.8/bin/clang}
OUT=$A/audio/out
export PATH=$A/gpu/ios/bin:$PATH
LAUNCHD_RO=$A/gpu/g5/root-ro/launchd.plist
LAUNCHD_SHA=7aab4ecbdaec394205a955f0dce2abf9dd571614b3aee53eaa7439d455763ede   # the flashed root's own cache (G5)

# 1. host test (pure logic)
cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror -o "$OUT.host_test" "$HERE/sa_host_test.c" -lm 2>/dev/null \
  || { mkdir -p "$(dirname "$OUT")"; cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror -o "$OUT.host_test" "$HERE/sa_host_test.c" -lm; }
"$OUT.host_test"

# 2. cross build
mkdir -p "$OUT/inc/CoreAudio" "$OUT/SweetAudio.driver"
ln -sf "$MACSDK/System/Library/Frameworks/CoreAudio.framework/Headers/AudioServerPlugIn.h" "$OUT/inc/CoreAudio/AudioServerPlugIn.h"
ln -sf "$MACSDK/System/Library/Frameworks/CoreAudio.framework/Headers/AudioHardwareBase.h" "$OUT/inc/CoreAudio/AudioHardwareBase.h"
ln -sf "$MACSDK/System/Library/Frameworks/CoreAudio.framework/Headers/AudioHardware.h" "$OUT/inc/CoreAudio/AudioHardware.h"
ln -sf "$MACSDK/System/Library/Frameworks/CoreAudio.framework/Headers/AudioHardwareDeprecated.h" "$OUT/inc/CoreAudio/AudioHardwareDeprecated.h"
CF=(--target=arm64-apple-ios16.0 -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I"$OUT/inc" -I"$HERE")
"$CLANG" "${CF[@]}" -bundle -fvisibility=hidden -framework CoreFoundation -o "$OUT/SweetAudio.driver/SweetAudio" "$HERE/SweetAudio.c"
"$CLANG" "${CF[@]}" -o "$OUT/sweetaudio_tone" "$HERE/sweetaudio_tone.c"
"$CLANG" "${CF[@]}" -o "$OUT/sweetaudio_install" "$HERE/sweetaudio_install.c"
"$CLANG" "${CF[@]}" -Wno-deprecated-declarations -framework CoreFoundation -framework CoreAudio -framework AudioToolbox -o "$OUT/sweetaudio_play" "$HERE/sweetaudio_play.c"
for f in "$OUT/SweetAudio.driver/SweetAudio" "$OUT/sweetaudio_tone" "$OUT/sweetaudio_install" "$OUT/sweetaudio_play"; do ldid -S "$f"; done
python3 -I - "$OUT/SweetAudio.driver/Info.plist" <<'EOF'
import plistlib, sys
FACTORY = "7F3C2A61-5B0E-4E57-9C1D-0A5E1D6B9F42"           # ours (random); type = kAudioServerPlugInTypeUUID
pl = {"CFBundleDevelopmentRegion": "en", "CFBundleExecutable": "SweetAudio", "CFBundleIdentifier": "org.pixelios.audio.SweetAudio",
      "CFBundleInfoDictionaryVersion": "6.0", "CFBundleName": "SweetAudio", "CFBundlePackageType": "BNDL",
      "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1", "CFBundleSupportedPlatforms": ["iPhoneOS"],
      "MinimumOSVersion": "16.0", "UIDeviceFamily": [1], "UIRequiredDeviceCapabilities": ["arm64"],
      "CFPlugInFactories": {FACTORY: "SweetAudio_Create"},
      "CFPlugInTypes": {"443ABAB8-E7B3-491A-B985-BEB9187030DB": [FACTORY]},
      "SweetAudioTransportType": "usb ", "SweetAudioName": "PixOS Speaker"}
open(sys.argv[1], "wb").write(plistlib.dumps(pl, fmt=plistlib.FMT_XML))
EOF
# symbol + link checks (gpu/ios/check-sdk-symbols.py: every undefined symbol resolves against the SDK .tbd files)
llvm-nm -gU "$OUT/SweetAudio.driver/SweetAudio" | grep -q ' T _SweetAudio_Create$'
python3 -I "$REPO/gpu/ios/check-sdk-symbols.py" "$SDK" "$OUT/SweetAudio.driver/SweetAudio" "$OUT/sweetaudio_tone" "$OUT/sweetaudio_install" "$OUT/sweetaudio_play" | tail -1

# 3. overlay: /usr/local/{pixos,bin} + the launchd cache with the one job (paths inside the 0117 allowlist)
OV=$A/audio/ov-sa-$MODE${TONE:+-$TONE}
rm -rf "$OV"
mkdir -p "$OV/usr/local/pixos/SweetAudio.driver" "$OV/usr/local/bin" "$OV/System/Library/xpc"
install -m 0644 "$OUT/SweetAudio.driver/Info.plist" "$OV/usr/local/pixos/SweetAudio.driver/Info.plist"
install -m 0755 "$OUT/SweetAudio.driver/SweetAudio" "$OV/usr/local/pixos/SweetAudio.driver/SweetAudio"
install -m 0755 "$OUT/sweetaudio_install" "$OUT/sweetaudio_tone" "$OUT/sweetaudio_play" "$OV/usr/local/bin/"
printf '%s\n' "$MODE${TONE:+ $TONE}" > "$OV/usr/local/pixos/sweetaudio.mode"; chmod 0644 "$OV/usr/local/pixos/sweetaudio.mode"
python3 -I "$REPO/tools/mk_launchd_job.py" "$LAUNCHD_RO" "$OV/System/Library/xpc/launchd.plist" --sha256 "$LAUNCHD_SHA" --job sweetaudio
chmod 0644 "$OV/System/Library/xpc/launchd.plist"
python3 -I "$REPO/tools/mkdevoverlay.py" "$OV" "$OV.bin"
sha256sum "$OUT/SweetAudio.driver/SweetAudio" "$OUT/sweetaudio_install" "$OUT/sweetaudio_tone" "$OUT/sweetaudio_play" "$OV.bin"
