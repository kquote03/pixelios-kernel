# 57: Safari page area black on oriole (host research for plan Step 2)

Build 20H392 (iOS 16.7, WebKit 615.x). Tags: **[bin]** read from the dyld cache (`$A/gpu/dsc/cache`, symbols from its
`.symbols` subcache), **[log]** seen in run `20261119T072104Z-guest1`, **[src]** WebKit open source as remembered (not checked
here), **[guess]**. Helper scripts (names and addresses only, nothing Apple written out): `$A/safari/tools/dsc.py` (images,
exports, local symbols, string xrefs, callers, `llvm-mc` disassembly), `q.py` / `xr.py` / `fs.py` wrappers.

## 1. IOSurface selector 44 = SetOwnershipIdentity (harmless)

- [bin] `IOSurfaceClientSetOwnershipIdentity` loads selector 44 (`mov w1, #44`) and calls `IOConnectCallMethod` with 4 scalar
  inputs, no struct, no output: [surface id, task identity token, ledger tag (sign-extended int), options]. It is the only `#44`
  site in IOSurface.framework. Neighbours for the table: 43 Init/SetDetachModeCode, 50-52 data properties (site docs).
- [bin] WebKit callers: `ImageBufferShareableMappedIOSurfaceBackend::setOwnershipIdentity` (GPU process, called for each image
  buffer it allocates for a WebContent process); failure log string "IOSurface::setOwnershipIdentity: Failed to claim ownership".
- Effect: memory accounting only (charges the surface to the WebContent footprint). Our default branch already returns success
  with a zeroed reply, so WebKit sees success. **Ignoring it cannot black out tiles.** [bin+guess]
- Implementing it: a named `case 44` (4 scalars, id lookup, success, log once) only to remove the GAP line. Draft:
  `$A/safari/patch-drafts/surface-sel44-ownership.txt`. Low priority.
- [log] what it tells us: the GPU process allocated 10-99 image buffers for the page, i.e. **DOM rendering ran in the GPU process**.

## 2. How WebKit 20H392 draws a page here

- [bin] `WebPreferencesStore::defaults()` stores `true` for UseGPUProcessForDOMRenderingEnabled, UseGPUProcessForCanvasRendering
  Enabled, UseGPUProcessForMediaEnabled and AcceleratedDrawingEnabled. So on this build the page is drawn **in
  `com.apple.WebKit.GPU`**: WebContent records display lists (`RemoteDisplayListRecorder_*` messages) and the GPU process replays
  them with CoreGraphics into image buffers (`RemoteRenderingBackend::createImageBuffer`,
  `RemoteLayerWithRemoteRenderingBackingStoreCollection`). [log] matches: sel 44 from the GPU process, GPU process CPU time
  1222+1123 ticks, ~80 surfaces / ~350 MiB counted under `other` in `SWEET SURF: totals` after Safari started.
- Backing stores [bin]: `RemoteLayerBackingStore` buffers are `ImageBufferShareableMappedIOSurfaceBackend` (IOSurface, used when
  AcceleratedDrawing is on) or `ImageBufferShareableBitmapBackend` (shared memory, when off); the UI side takes either a
  MachSendRight or a ShareableBitmapHandle (`layerContentsBufferFromBackendHandle(variant<ShareableBitmapHandle, MachSendRight>)`).
- Drawing into an IOSurface is CoreGraphics on the CPU (CGIOSurfaceContext) [src+guess]; no Metal object is needed for DOM, only
  WebGL/WebGPU/"Use the Metal backend for ANGLE" use Metal [bin strings]. No "Metal missing" error is expected for a text page.
- Pixel formats [bin]: `WebCore::IOSurface` ctor picks one of 'BGRA', 'w30r' (RGB10), 'b3a8' (RGB10A8, two planes) and '422f'
  (video). Layers get the 10-bit formats when they want a deep-colour backing store (`PlatformCALayerRemote::wantsDeepColor
  BackingStore`); iPhone X is a P3 panel, so page tiles are likely 'w30r' (opaque) / 'b3a8' (with alpha) [src+guess].
- Path to the screen [src]: RemoteLayerTree: MobileSafari builds real CALayers from the WebContent transactions and sets each
  tile's `contents` to the IOSurface (or a CGImage made from the bitmap). CA in MobileSafari sends it to backboardd like any
  layer contents; backboardd looks the surface up by port (sel 34/35 are real IOKit object ports in 0046, so the
  GPU -> WebContent -> MobileSafari -> backboardd hand-over is fine) and samples it with `texImageIOSurface` in libsweetgl.
  No CALayerHost/remote context is involved for page tiles (only video uses one).
- **Gap in our sampler**: `eagl_mesa.c surface_pipe_format()` maps 'w30r' to B10G10R10A2_UNORM (alpha = the two pad bits, which a
  CPU writer leaves 0 [guess]; values are extended-range xr10, not remapped) and 'b3a8' to BGRA8 on plane 0 (ignores the 10-bit
  packing and the A8 plane). Both work for CA's own GL-rendered targets but are wrong for CoreGraphics-written tiles: transparent
  or near-transparent tiles over a black window would look exactly like a black page. [guess, the diagnostic boot decides]

## 3. Switches that force the CPU / bitmap path (verified)

| key (NSUserDefaults of the UI process = MobileSafari) | effect | evidence |
|---|---|---|
| `WebKit2GPUProcessForDOMRendering` = NO | DOM drawn in WebContent instead of the GPU process | [bin] `WebPageProxy::useGPUProcessForDOMRenderingEnabled`: `objectForKey:` this key, its `boolValue` wins, else the preference |
| `WebKitDebugAcceleratedDrawingEnabled` = NO | layer backing stores become shared-memory bitmaps (CGImage contents, CA's normal image path that icons already use) | [bin] debug pref read by `setDebugBoolValueIfInUserDefaults`; `-[WKPreferences init]` sets the global debug prefix "WebKitDebug" |

- Both are read on release builds, no AppleInternal check on these paths [bin]. Not useful: `WebKitExperimental%@` and
  `InternalDebug%@` are only read for network parameters [bin xrefs]; `WebKit2.` is the legacy WebPageGroup prefix;
  `UseGPUProcessForDOMRenderingEnabled` itself is only read with a preferences identifier (Safari's WKPreferences have none
  [guess]). MobileSafariUI does not reference `_setAcceleratedDrawingEnabled:` [bin strings]. No WebKit env vars [bin].
- QuartzCore env names exist (`CA_NO_ACCEL`, `CA_ACCEL_BACKING`, `CA_IOSURFACE_IMAGE`, `CA_ENABLE_OGL`) [bin] but they act on
  CABackingStore in the app, which WebKit tiles bypass; not needed.
- Delivery (overlay allowlist is fixed in the kernel: /usr/lib, /usr/local, /private/var/dev, a few exact files):
  - **Prepared, no kernel change**: `libwkcpu.dylib` registers both keys in MobileSafari's registration domain (nothing written to
    disk) from a constructor. Source `$A/safari/wkcpu/wkcpu.c`, build `$A/safari/wkcpu/build.sh` (sweetaudio toolchain),
    built `$A/safari/out/libwkcpu.dylib` (sha256 84927b78…), overlay tree `$A/safari/ov-wkcpu/usr/local/lib/libwkcpu.dylib`,
    packed `$A/safari/ov-wkcpu.bin` (mkdevoverlay accepted it). Boot-arg entry:
    `MobileSafari:DYLD_INSERT_LIBRARIES=/usr/local/lib/libwkcpu.dylib` (+ `,WKCPU=dom` or `,WKCPU=bitmap`; default both).
    Combine with libsalog as `DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib:/usr/local/lib/libwkcpu.dylib` (0110 splits
    the entry at the first ':' only; each assignment ≤ 127 bytes). Not run on the phone yet.
  - Fallback (needs a kernel allowlist patch): root-owned managed prefs `/private/var/Managed Preferences/mobile/
    com.apple.mobilesafari.plist` and `.GlobalPreferences.plist` (tree `$A/safari/ov-webkit-cpu/`, draft
    `$A/safari/patch-drafts/devoverlay-managed-prefs.txt`). `/Library/Managed Preferences/mobile` is a symlink in the root
    image; CoreFoundation has the managed path strings [bin]; that cfprefsd honours it without a profile is a guess.
    Safari's own plist lives in its data container (UUID unknown before first launch), so that path is not usable.

## 4. Is it backboardd not compositing another process?

Unlikely: page tiles are layers of MobileSafari's own context (RemoteLayerTree), the same kind of context as every other app,
and Settings etc. render. **Question for the user / diagnostic boot**: are Safari's address bar and toolbar visible while the page
area is black? Visible chrome = context composited, tile contents wrong (sections 2/3). Whole Safari window black = look at the
MobileSafari process instead.

## 5. Diagnostic boot recipe (phone agent)

1. Logs: `sweetnull=pixui_probe,backboardd,MobileSafari,com.apple.WebKit.GPU,com.apple.WebKit.WebContent` (84 of 127 bytes;
   0048 matches `proc_selfname`, the full XPC name, as the GAP line shows). `sweetenv=` backboardd's sweetgl entry +
   `MobileSafari:`, `com.apple.WebKit.GPU:` (and if room `com.apple.WebKit.WebContent:`)
   `DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib,SALOG_OSLOG=2` (never =1). All three: 406 of 512 bytes, but the guest1
   boot line is already ~1010 of 1023 (BOOT_LINE_LENGTH 1024): drop audio for this boot (mediaserverd entry, `sweetaoc`), the
   `sweethid*` test trio and the duplicate `sweetwlan=1`, and add `sweetfbhash=1`. Keep Wi-Fi (page load) and touch.
2. Grep: kernel `SWEET GAP: surface`, `SWEET SURF:` (note: 0261 per-name totals compare 17 bytes, so names over 16 chars,
   all WebKit services, never match and each create of theirs is counted under `other`; `CollectionsPoste` x5 slots is the same
   bug); `/var/tmp/sweetgl.log` lines `surface id … fmt … accepted|REJECTED (format)` and `upload surface id` for the Safari
   period: **the fourcc of the page tiles is the key fact** ('w30r'/'b3a8' -> cause A, 'BGRA' accepted -> look further);
   os_log (subsystem `com.apple.WebKit`, categories `RemoteLayerTree`, `Process`, `Layers`, `ViewState`) and the WebCore
   strings "IOSurface creation failed for size: (%d %d) and format: (%d)".
3. Then fix boots, cheapest first: (A) `libwkcpu` with `WKCPU=bitmap` (tiles become CGImage contents through the image path
   that already works; GPU process still draws) ; (B) `WKCPU=both` (no GPU process drawing at all); (C) if the tiles are
   'w30r'/'b3a8' and must stay IOSurfaces: libsweetgl `eagl_mesa.c` (userland repo) takes alpha = 1 for 'w30r', remaps xr10
   (v·1023 → (v−384)/510), and for 'b3a8' converts plane 0 as 10-bit + plane 1 as alpha on the copy path. Pass = page visible
   and `sweetfbhash` changes on load (plan Step 2.3).

Open: whether tiles are deep colour on this port (UIScreen gamut from MobileGestalt), whether Safari's chrome shows, whether
cfprefsd honours unmanaged-device managed prefs (only matters for the fallback).

## 6. Results on the phone (2026-11-19, Step 2 of plan woolly-floating-parrot; rows safari-* in device/google/oriole/docs/PORT.md)

Boots: 6 of 6 (budget). Kernel 1c843b0c, the Step 11 daily set (cpus 5 X1-first, MIF 1352, fb 60 Hz set), `sweetnull=backboardd,SpringBoard,MobileSafari`,
`sweetfbhash=1`, a 1-rep nav for the unlock and the labelled 0403 thumbnails. Boot line 1245-1330 B: the real limit is 2048 (0320), so the
1023 B concern of s.5 does not apply and audio stayed in.

- **Diagnosis came from this round's speed runs** (*observed*, no extra boot): iOS resumed Safari at the unlock in speed-base / speed-free (0403
  thumbnails: Safari's toolbar and address bar drawn, page area black: s.4 answered, the context is composited). Right after
  `exec com.apple.WebKit.GPU`, backboardd attaches **'w30r' 1125x1905 / 1536 / 1473 / 1194 / 1098 / 579 / 210** surfaces (page backing stores,
  double-buffered pairs) and 'b3a8' 1125x1311 / 183 / 159 (Safari's own bars, drawn fine as BGRA8). No import failure, no REJECTED/SURFFAIL;
  the only texImage refusal is one `plane 1` (b3a8 alpha plane) in free2/free3. Sel 44 from the GPU process as before (harmless, s.1).
- **A `WKCPU=bitmap`** (safari-bitmap, `wkcpu[273]: mode bitmap ... registered 1`): the page still arrives as 'w30r' 1125x1905 surfaces; page area
  black. The switch does not move the page off the 10-bit IOSurface path on this build. `WKCPU=both` not run (budget spent on C).
- **C libsweetgl W30R rule** (userland d0ff1a4, dylibs 6223d228 -> 7113f7c5 -> 2fc47f4b): CPU copy with the CIF10 remap and alpha 1 for
  non-full-screen, non-render-target 'w30r' attaches. *Observed*: 8-20 copies per Safari launch, 10-23 ms per 1125x1905 surface (CA attaches on
  content change, not per frame); no crash; unlock, home screen and bench steps unchanged (safari-w30r-lean, -full). First version re-pointed
  textures at the surface when CA reused the NAME for an FBO target 10 s later (would let CA render into Safari's surface); fixed (check the
  name still holds our RGBA8 copy; `reused ... forgotten` x9 in safari-w30r-full, only real 281x609 blur targets reverted).
- **Page visibility: NOT proven.** safari-w30r-full (Safari opened by the bench tap at 424,2266 = dock icon, guest join failed `set_ssid status=1`):
  the probe thumbnail 3 s after launch still shows a black page area; its page-area pixels are bit-identical to speed-free's (near-black dither),
  while the status band changed (white before, #22242a now). So either the tiles were still empty (no network, 3 s) or the page layer is not
  what covers that region. The copy-content thumbnails (`SWEETGL_W30RTHUMB=8`) that would decide it were lost: safari-w30r3-lean's USB log
  stopped 22 s into the boot (known intermittent silent stop; phone ran on and reset itself at +567 s).
- **Payload limit**: the 22 MB dylib in the run overlay (5.5 MB compressed) makes a full-config payload ~28.1 MB: back in fastboot at +7 s
  (safari-w30r, PORT G5 ramp: 24.3 MB passes, >= 32.7 MB fails). The dylib goes in by a lean boot (no AOC/amp/Wi-Fi firmware, ~22.9 MB) and stays
  on userdata; daily images rely on that copy. A later `--intent gpu` run restores the std extra 8efaad3e (no W30R rule): re-deliver 2fc47f4b
  (`$A/safari/sweetgl-out/libsweetgl.dylib`, overlay `$A/safari/ov-s4`, `LEAN=1 $A/safari/run-s.sh`) or revert by delivering 8efaad3e the same way.
- Next if the user still sees a black page: one boot with Wi-Fi joined and `SWEETGL_W30RTHUMB=8` (needs a dylib already on userdata, so the
  full config fits), then `SWEETGL_W30R=opaque` / `WKCPU=both` as A/B; also the b3a8 plane-1 alpha.
