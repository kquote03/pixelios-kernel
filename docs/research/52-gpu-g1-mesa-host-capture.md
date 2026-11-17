# 52: GPU G1: Mesa Panfrost on the host, the `sweet` pan_kmod backend, and the `.malijob` capture format

Date 2026-11-15, lane G (plan 2026-11-14 oneshot-2, step G1). Host only: no phone, no adb/fastboot.
Tags: **[obs]** a command run here, **[src]** read from the cited file, **[guess]** not verified.

## 1. Result

| Item | Value |
|---|---|
| Mesa | 26.2.5, `ddaf85e353442d9d7f192fdcf1823e652e3f2ee3` (doc 49), clone `$A/gpu/src/mesa`, plus `gpu/mesa-patches/0001-0004` |
| Build | `$A/gpu/build/mesa`, 1457/1457 targets, including `mesa_clc`, `vtn_bindgen2`, `panfrost_compile` (the host tools G3 needs) [obs] |
| `clear.malijob` | 2960 B, sha256 `d4fb280db52c8185239dc0b2d544f1f9857cdfe6826cff16c0a936bfe6a83e90` [obs] |
| `tri.malijob` | 4736 B, sha256 `e195bd8e3f0680c5a77ca69dec9167028cb8b09677ae407543b1c5a6c958ff14` [obs] |
| Location | `$A/gpu/capture/` (not in git), with `*.pandecode.txt` and `*.summary.txt` next to them |
| G1 pass | PASS: pandecode decodes the files themselves (not just Mesa's live state) without an assertion; clear = one Fragment job whose RT0 `Clear: Color 0: 0xff996633`; triangle = `Malloc vertex` job (Valhall vertex+tiler job with Tiler Job Payload, Tiler Heap, Tiler Context) then a Fragment job [obs] |

Reproduce: `gpu/capture/run-capture.sh` (configure via `gpu/nix/configure-host.sh`, build, record, finalize, hash).

## 2. Build (G1 blocker and fix)

The earlier attempt failed with `ERROR: C++ shared or static library 'clangBasic' not found`. Cause [src `meson.build:2103` + obs]:
Mesa searches the clang libraries only in LLVM's libdir (`llvm-config --libdir` = `llvm-21.1.8-lib/lib`), while nix ships them in the
separate output `clang-21.1.8-lib/lib`. nixpkgs' own mesa carries the same fix (`pkgs/development/libraries/mesa/opencl.patch`, option
`clang-libdir`) [src]. Our `0002-meson-clang-libdir-option.patch` adds that option (default: old behaviour). No fallback to an older Mesa
was needed.

```
nix-shell $A/gpu/nix/shell.nix --run 'bash gpu/nix/configure-host.sh'     # meson setup build/mesa src/mesa -Dclang-libdir=$CLANG_LIBDIR
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= -Degl=enabled -Dgbm=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=enabled
  -Dopengl=true -Dglvnd=disabled -Dllvm=enabled -Dshared-llvm=enabled -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled
  -Dinstall-precomp-compiler=true -Dtools=drm-shim,panfrost -Dbuild-tests=false ... (full line in gpu/nix/configure-host.sh)
flock $A/scratch/heavy.lock nix-shell $A/gpu/nix/shell.nix --run 'ninja -C $A/gpu/build/mesa -j6'
```
The shell (`gpu/nix/shell.nix`, copy of `$A/gpu/nix/shell.nix`) uses `llvmPackages_21` (21.1.8, the set nixpkgs' mesa uses [obs
`nix-instantiate --eval`]), `spirv-llvm-translator` 21.1.0, `spirv-tools`, libclc, and exports `CLANG_LIBDIR`.

## 3. Mesa changes (`gpu/mesa-patches/`, apply in order on a pristine 26.2.5 [obs `git apply --check`])

| Patch | What |
|---|---|
| 0001 | G78 model row `PAN_PROD_ID(9, 2, 2)`, name "G78". GPU_ID `0x92020010` [obs, SWEET GPU probe] decodes to arch 9.2, product major 2 (`model/pan_model.h` macros, doc 32 s.3) [src]. Aniso, tilebuffer sizes 16384/8192 and rates are **copied from the G57 row [guess]**; Mesa main could not be fetched (gitlab answers bots with an Anubis page [obs]), so no upstream G78 row was compared |
| 0002 | meson option `clang-libdir` (s.2) |
| 0003 | the `sweet` pan_kmod backend, record transport, and four small hooks in gallium (s.4) |
| 0004 | `src/panfrost/tools/sweet_malijob`: validates a capture, runs `pandecode_jc` over the BOs stored in the file, finds the render target by unpacking the v9 FRAGMENT_JOB -> FRAMEBUFFER_POINTER -> FRAMEBUFFER -> RENDER_TARGET descriptors, and writes the RT block |

## 4. The `sweet` backend

`src/panfrost/lib/kmod/sweet_kmod.{c,h}`: a `pan_kmod_ops` backend over a transport interface (`struct sweet_transport`:
query, bo_new, bo_close, bo_label, submit, wait). Selected in `pan_kmod_dev_create()` **before** `drmGetVersion()` when a transport is
registered (`sweet_kmod_set_transport`, iOS) or `PAN_SWEET_RECORD=<dir>` is set (host). Behaviour, each tied to the SweetMali driver
(patches 0104/0105, read from the built tree `$A/localbuild/work/main/xnu`):
- one VM, AUTO_VA, whole-BO maps only (like `panfrost_kmod.c`); the GPU VA comes from the transport (kernel-chosen, `sweet_mali_map`);
- `supported_bo_flags` = EXECUTABLE | NO_MMAP: no alloc-on-fault (SweetMali has no fault-driven heap growth), no WB mmap, not
  IO-coherent (COHERENCY_ENABLE = 31, `sweet_mali.c` `COHERENCY_NONE`);
- the tiler heap is therefore allocated eagerly and small: `PAN_SWEET_TILER_HEAP_MB` (default 16, 1..128) instead of the 128 MiB growable
  reservation (`gallium/drivers/panfrost/pan_device.c`). pandecode shows `Chunk size: 2 MiB, Size: 0x1000000` [obs];
- CPU mappings are owned by the transport: new optional paths in `pan_kmod_bo_mmap()` / new `pan_kmod_bo_munmap()` (used by `pan_bo.c`),
  so nothing calls `mmap()` on a DRM node (doc 32 s.5 B);
- submit: `pan_jm.c` calls `sweet_kmod_submit(jc, requirements)` instead of `DRM_IOCTL_PANFROST_SUBMIT`. The BO list and syncobjs are not
  passed: one address space, one in-order queue (`sweet_mali_submit` waits for the previous job) [src]. Syncobjs still go through libdrm:
  on the host the drm-shim stubs them; on iOS the fake libdrm of G3 must implement them (doc 32 s.2.5);
- BO wait = wait on the last submitted seqno, absolute `os_time_get_nano()` deadline converted to a relative bound <= 60 s (selector 4
  clamps to 60 s too);
- dma-buf import: not implemented (returns NULL; IOSurface import is G6 work).

**Record transport** (`sweet_record.c`, built only for Linux hosts): VAs from a copy of the `mali_va_alloc` rule (4 KiB bitmap over
`0x02000000..0x100000000`, BOs rounded to 16 KiB, align 4 pages, first fit from a rolling hint); handles = lowest free slot + 1 of 4096
(`SWEET_MALI_MAX_BOS`), BO size <= 256 MiB (`SWEET_MALI_MAX_BO_BYTES`). `query` returns:

| Field | Value | Tag |
|---|---|---|
| gpu_id, shader_present, l2_features, mem_features, mmu_features, as_present, js_present | 0x92020010, 0x779fff, 0x8130206, 0x301, 0x2830, 0xff, 0x7 | [obs] SWEET GPU probe (patch 0102) on oriole, PORT.md 2026-11-14 |
| tiler_features | 0x809 (bin 512 B, max levels 8 -> `Hierarchy Mask: 255`) | [src] kbase software model row "tBEx", `google-gpu 696fc02 mali_kbase/backend/gpu/mali_kbase_model_dummy.c:308-322`; **not read on hardware [guess for the real chip]** |
| thread_max_threads/workgroup/barrier, thread_features | 0x180 x3, 0x6000 \| 4<<16 | same model row [src], hardware [guess] |
| texture_features, afbc_features | 0 | no compressed formats claimed; AFBC unused (captures force linear) |

The model row's `mmu_features` 0x2830 equals the observed register, which supports (does not prove) the row. On the phone the iokit
transport reads the real registers (`sweet_mali_query`), so only the **recorded** chains depend on these guesses. Risk for G2: if the real
TILER_FEATURES has fewer than 8 levels, the recorded hierarchy mask 255 may be invalid for the chip; a fault then prints JS_STATUS and the
next step is to read TILER_FEATURES (offset 0x0C, already in `sweet_mali_query`) and re-record.

## 5. Capture program and procedure

`gpu/capture/sweetgl_capture.c`: EGL surfaceless + GLES2, FBO with a 256x256 `GL_RGBA8_OES` renderbuffer.
Frame 0: `glClearColor(0.2, 0.4, 0.6, 1.0)` (exact in UNORM8: 0x33, 0x66, 0x99, 0xff), `glClear`, `glFinish`.
Frame 1: same clear, one triangle NDC (-0.5,-0.5) (0.5,-0.5) (0,0.5), constant colour (0,1,0,1), `glFinish`.
Environment: `LD_PRELOAD=libpanfrost_noop_drm_shim.so PAN_GPU_ID=9202 PAN_SWEET_RECORD=<dir> PAN_MESA_DEBUG=linear,nocrc`. The drm-shim only
gives EGL a render node to open the panfrost driver with; every BO and submit goes through the sweet backend [obs: `GL_RENDERER=Mali-G78
MC20 (Panfrost)`, log `sweet-record: submit jc=0x3054180 req=0x1`, then `jc=0x3054300 req=0` + `jc=0x3054680 req=0x1`].
`linear` forces a linear RT (the replay CRCs it), `nocrc` turns off transaction elimination (no CRC buffers).

Finalize: `sweet_malijob cap000.malijob --out clear.malijob --name clear --uniform 0xff996633` (expected crc32 0xff12cb5c, cross-checked with Python
`zlib.crc32` [obs]; checks at (0,0), (255,255), (128,128)); `sweet_malijob cap001.malijob --out tri.malijob --name tri --px 2,2,0xff996633 --px
253,253,0xff996633 --px 128,128,0xff00ff00` (no expected CRC: the replay reports it; rasterisation is not predicted on the host).
Both: RT0 base 0x3014000, row stride 1024, 256x256, block format Linear, R8G8B8A8 [obs].

Observed quirks:
- `pandecode` prints `Format (v7): XXX: INVALID RGBA` three times for buffer descriptors of the triangle; Mesa's own live trace
  (`PAN_MESA_DEBUG=trace`) prints the same three lines [obs], so this is pandecode's display of a v7 field on v9, not missing data.
- Frame 0 is byte-identical across runs; frame 1 differs in 6 bytes of the batch pool at +0x1cc and +0x1dc [obs `cmp`]: they are the
  unused fourth lanes after two vec3 viewport sysvals (128.0, 128.0, 0.5) that Mesa uploads into vec4 slots without clearing. The
  shaders do not read them [guess, from the layout]. The sha256 above pins the copy that was recorded.
- `MESA: error: export failed` after each frame: fence export through drm-shim's stub syncobjs; no effect on the capture.

## 6. `.malijob` format, version 1 (`src/panfrost/lib/kmod/sweet_malijob.h` in patch 0003)

Little-endian, offsets from the start of the file. The kernel replay (G2) parses the same layout; any change bumps `version`.

| Part | Offset | Size | Fields |
|---|---|---|---|
| header | 0 | 64 | `magic[8]` = `PIXMJOB1`; u32 `version` = 1, `header_size` = 64, `gpu_id`, `n_chains` (1..8), `n_bos` (1..4096), `flags` (bit 0 = RT block present); u64 `file_size`; u32 `chain_off`, `rt_off`, `bo_off`, `data_off`; char `name[8]` at +56 (`[a-z0-9_]`, NUL-padded, set by `sweet_malijob --name`; the replay prints it) |
| chains | `chain_off` (64) | 16 x 8 slots | u64 `jc` (GPU VA of the first job descriptor); u32 `req` (bit 0 = fragment chain, JS0; else JS1); u32 reserved. Only `n_chains` are used, submitted in order |
| rt | `rt_off` (192) | 64 | u64 `va`; u32 `width`, `height`, `stride`, `format` (1 = R8G8B8A8_UNORM, pixel as LE u32 = R \| G<<8 \| B<<16 \| A<<24); u32 `expect_crc`, `crc_valid`; 3 x {u16 `x`, u16 `y`, u32 `rgba`}; u32 reserved[2]. The crc is crc32 (IEEE/zlib) over `height` rows of `width*4` bytes |
| bos | `bo_off` (256) | 64 x `n_bos` | u64 `va`, `size` (multiple of 16 KiB), `data_off`, `data_len` (<= size; bytes after it are zero); u32 `flags` (bit 0 exec, bit 1 never CPU-mapped, bit 2 tiler heap); u32 reserved; char `label[24]` (NUL-terminated, Mesa's BO label) |
| data | `data_off` | sum of `data_len` | BO contents, trailing zeros trimmed, lengths multiples of 16 |

Rules the recorder guarantees and every reader must check (the host tool and the G2 kernel parser do): `file_size` equals the real length;
every table and data range lies inside the file; each BO is 16 KiB aligned and sized, inside `0x02000000..0x100000000`, and BOs do not
overlap; each chain's `jc` and the RT (`va + stride*height`, `stride >= width*4`) lie inside a BO; the three pixels lie inside the RT.

Contents of the two files [obs, `*.summary.txt`]:

| BO (va, size, data) | clear | tri |
|---|---|---|
| Tiler heap 0x02000000, 16 MiB, no data (flags 0x2) | yes | yes |
| Sample positions 0x03000000, 16 KiB, 0x590 | yes | yes |
| Preload shaders 0x03004000 (exec) / Descriptors 0x03008000 / Shaders 0x0300c000 (exec) / Printf 0x03010000, 16 KiB each | yes | yes |
| Render target 0x03014000, 256 KiB, no data | yes | yes |
| Batch pool 0x03054000, 64 KiB | 0x1b0 | 0x6b0 |
| Index buffer 0x03064000, 1 MiB, 0x20 | - | yes |

Wired memory the replay needs: 17.4 MiB (clear) / 18.4 MiB (tri), dominated by the heap.

## 7. Open items

1. TILER_FEATURES and THREAD_* of the real chip (s.4 table); `sweet_mali_query` already reads them, a read-only print would settle them.
2. G78 tilebuffer sizes (0001, [guess]).
3. Syncobj emulation and `bo_import` for the iOS side (G3/G6).

## 8. G2: kernel replay (patch 0155, `sweetmali=0x100`; built, unrun, review pending)

- Container: `tools/pack_gpujob.py OUT clear.malijob tri.malijob` = the WLANFWB1 layout of `tools/pack_wlanfw.py` (now a generic
  `pack_container`/`parse_container`, WLANFW output byte-identical [obs]) with magic `GPUJOBS1`, tag `GPUJ` (repeatable, <= 4, length + crc32).
  `mkpayload.py --gpujob` puts it in `/chosen/memory-map GPUJOB <pa, size>`; `tools/xrun --gpujob` / `build-xnu-image.sh --gpujob` (needs
  `--gpu-handover`). `$A/gpu/capture/gpujob-clear-tri.bin`: 7744 B, sha256 `f1f568d7a0fb2a9eddc3e3b1fc9074e46533a9c63098a8c39a01c23d12266fba` [obs].
- Parser `bsd/dev/sweet_mali_replay.h` (pure, the rules of s.6 plus kernel limits: <= 64 BOs, <= 32 MiB per BO, <= 64 MiB per job, RT <= 4096^2,
  job descriptors 128-byte aligned (`v9.xml:1425`) with >= 64 bytes inside a BO, GPU_ID must equal the chip's). Host test
  `shim/mali_replay_host_test.c` (plain + ASan/UBSan, exact-size heap copies, 200k random corruptions), `tools/test_mali_replay_mutants.py`
  (48 guards removed one by one, all killed [obs]).
- Kernel (`sweet_mali.c`): `sweetmali=0x100` without bits 1|2 = bring-up as `sweetmali=1` but no SweetMali service (its `start()` needs bit 1|2),
  so AS0 holds only the replay's BOs. Per job: `kmem_alloc` wired zeroed memory per BO, copy `data_len` bytes, clean+invalidate, `sm_map_at`
  (= `sweet_mali_map` with `mali_va_reserve` at the recorded VA; refused when taken or outside the window), chains through
  `sweet_mali_submit`/`sweet_mali_wait` (2 s each; a timeout waits 4 s more for the core's 3 s hard stop + reset), invalidate, crc32 of the RT
  rows and the three pixels. Output `SWEET MALI: replay <name>=PASS|FAIL crc=0x...`. A failure stops the replay; BOs are freed only after a
  clean unmap with no job running, else leaked. Read-only extra: `replay regs ...` prints TILER_FEATURES / THREAD_* / TEXTURE_FEATURES (s.4 open item 1).

## 9. G3 state at hand-over (2026-11-15, lane G; the rest of G3 is owned by another agent from here)

What exists [obs unless tagged]:

| Item | Where | Note |
|---|---|---|
| Host tools for the cross stage | `$A/gpu/install-host/bin/{mesa_clc,vtn_bindgen2,panfrost_compile}` | `ninja -C $A/gpu/build/mesa install` (host build of s.2, patches 0001-0004); run inside `gpu/nix/shell.nix` (they link nix LLVM) |
| theos cctools wrappers | `$A/gpu/ios/bin/{ld,ldid,libtool,lipo,otool,nm}` | made by `gpu/ios/mk-wrappers.sh`; `ld -v` = ld64-609 |
| Cross toolchain check | - | nix clang 21.1.8 `--target=arm64-apple-ios16.0 -isysroot iPhoneOS16.5.sdk` + wrapped ld64: a hello world is `Mach-O 64-bit arm64 executable`, `otool -L` = `/usr/lib/libSystem.B.dylib` only |
| meson cross / native files | `gpu/ios/ios-arm64.cross`, `gpu/ios/host-tools.native` (copies of `$A/gpu/ios/`) | `-fuse-ld` must NOT be in the compiler args (meson's argument probes then fail with -Wunused-command-line-argument) and meson ignores it for the Apple-linker probe: ld64 is found as `ld` through PATH instead |
| Configure script | `gpu/ios/configure-ios.sh` -> `$A/gpu/build/mesa-ios` | doc 32 s.4.2 option set + `-Ddarwin-sweet=true`; configure succeeds, detects `ld64 609` for host links |
| Mesa patch 0005 | `gpu/mesa-patches/0005-panfrost-sweet-darwin-ios-build.patch` (also `$A/gpu/patches/`) | `include/darwin-sweet/xf86drm.h` (declarations only), meson option `darwin-sweet` (dep_libdrm = that header), kmod on Darwin = pan_kmod + panthor_kmod + sweet_kmod + `sweet_iokit.c` (no panfrost_kmod: eventfd/timerfd), `sweet_kmod_wait_idle()` for the syncobj stand-in, iokit transport auto-selected on `__APPLE__` |
| iokit transport | `src/panfrost/lib/kmod/sweet_iokit.c` (in 0005) | selectors 0-4 of patch 0105; BO CPU map = `IOConnectMapMemory64(type = handle, kIOMapWriteCombineCache)`; unrun |

Not done (owner: the G3 agent): the gallium frontend/target `src/gallium/targets/sweetgl` (no GLX/EGL on iOS: es2api is not built, so the dylib must
carry the GLES entry points itself; `panfrost_create_screen(fd, config, NULL)` needs a `driOptionCache` built from `driinfo_panfrost.h` because
xmlconfig is disabled), the libdrm/syncobj implementation (`sweet_drm.c`, syncobj waits -> `sweet_kmod_wait_idle`), the EAGL/IOSurface shim,
`libsweetgl.dylib`, `sweetgl_selftest`, ldid signing. Result of the first cross `ninja -k 0`: see the row added below when it finishes.

Rebuild from scratch:
```
cd $A/gpu/src/mesa && git checkout -- . && for p in <repo>/gpu/mesa-patches/000[1-5]*.patch; do git apply $p; done
<repo>/gpu/ios/mk-wrappers.sh
nix-shell $A/gpu/nix/shell.nix --run 'bash <repo>/gpu/nix/configure-host.sh' && flock $A/scratch/heavy.lock nix-shell $A/gpu/nix/shell.nix --run 'ninja -C $A/gpu/build/mesa install'
nix-shell $A/gpu/nix/shell.nix --run 'bash <repo>/gpu/ios/configure-ios.sh'
flock $A/scratch/heavy.lock nix-shell $A/gpu/nix/shell.nix --run 'export PATH=$A/gpu/ios/bin:$PATH; ninja -C $A/gpu/build/mesa-ios -k 0'
```
[guess] 0005 changes the kmod meson for Linux too (panfrost_kmod.c moved into the non-Darwin branch); the host build was not rebuilt with 0005 yet.

First cross build result [obs 2026-11-16]: `ninja -C $A/gpu/build/mesa-ios -k 0` = 278/278, 0 FAILED. The default targets are 30 static libraries
(`libglapi.a`, `libpan-arch-v*.a`, `libpan-format-v*.a`, `libpanfrost*.a`, `libpanfrost_perf.a`, `libpanfrostwinsys.a`); objects are
`Mach-O 64-bit arm64 object`, LC_BUILD_VERSION platform ios, minos 16.0, sdk 16.4. Libraries that are not built by default (libmesa,
libgallium, libpankmod_lib, ...) are only built when a target links them; `ninja ... src/panfrost/lib/kmod/libpankmod_lib.a` built cleanly
(no warnings) and exports `sweet_kmod_*` and `sweet_iokit_transport_create`. No dylib or executable exists yet (the sweetgl target is the G3 agent's).

## 10. G2 on hardware (2026-11-15, run gpu-o5-g2-replay1, review 0155 SAFE)

[obs] Kernel `xnu-local-lane-g-63384af` (patches to 0155), `--gpu-handover --gpujob gpujob-clear-tri.bin --args sweetmali=0x100`:
`replay clear=PASS crc=0xff12cb5c (expected)`, `replay tri=PASS crc=0x7a47b674 px=0xff996633,0xff996633,0xff00ff00`, `replay done 2/2 PASS`.
The first Mesa Panfrost job chains (fragment clear; malloc-vertex/tiler + fragment triangle) ran on the Mali-G78, recorded on the host with
the record transport's guessed properties. Registers read in that run (settles s.4 open item 1):

| Register | Record transport used | Hardware [obs] |
|---|---|---|
| TILER_FEATURES | 0x809 (kbase model) | 0x809 |
| THREAD_MAX_THREADS / WORKGROUP / BARRIER | 0x180 x3 (kbase model) | 0x400 / 0x200 / 0x200 |
| THREAD_FEATURES | 0x46000 | 0x48000 |
| TEXTURE_FEATURES0..3 | 0 | 0xf7fe007e, 0xc3fff7ff, 0xbfe1ff9f, 0x10c6 |
| AFBC_FEATURES | 0 | 0 |

Review follow-ups (review 0155): F2 BO labels printable ASCII only (kernel parser + `pack_gpujob.check_malijob`, host test + mutant);
F3 `sm_replay` refuses unless `PAGE_SIZE == SMR_BO_PAGE`; F4 `sweet_malijob --crc` and `tri.malijob` now carries the observed crc
(`crc_valid = 1`, `expect_crc = 0x7a47b674`; only bytes 216-220 changed): `tri.malijob` sha256 `009c4e89334659c76ca411c7c2eb741250cc798ac902e55fe1d345eb94f67fe6`,
`gpujob-clear-tri-v2.bin` sha256 `b15fb13b23ce5a657e5422523b48bf965eb1f7034e89d9ba27ca0c2ecc325e87` (7744 B). F1 (submit failure other than EINVAL
leaks) is in patch 0157, where the second submitter (IRQ path) appears. F5 (print JS_HEAD/TAIL) not done (optional, diagnostic).

## 11. G3 result (2026-11-16, sub-lane lane-g3; host only, nothing ran on the phone)

Tags as above: **[obs]** a command run here, **[src]** read from the cited file, **[guess]** not verified. Nothing in this section is a
device result: `sweetgl_selftest` has **not** been run (no phone work in this step), so "PASS" below means the host-checkable criteria only.

### 11.1 Outputs (`$A/gpu/out/`, not in git)

| File | Type [obs `file`] | Size | sha256 (after `ldid -S`) |
|---|---|---|---|
| `libsweetgl.dylib` | Mach-O 64-bit arm64 dynamically linked shared library, TWOLEVEL, NOUNDEFS | 20920496 B | `f2b31a68a0021178d48e0debfc0704d026e5affb272fa194117e8409ba54d452` |
| `sweetgl_selftest` | Mach-O 64-bit arm64 executable, TWOLEVEL, PIE | 68544 B | `67733d40c5fee7ed531861fc85880f5f1a930da0180d803261446475e8e3caab` |

Both: `LC_BUILD_VERSION platform ios, minos 16.0, sdk 16.5` [obs `llvm-objdump --macho --private-headers`]; ad-hoc signed with theos `ldid -S`
(selftest CodeDirectory v=20400, sha256 hashes [obs `ldid -h`]). Install name of the dylib: `/usr/lib/libsweetgl.dylib`.
`$A/gpu/out/check-sdk-symbols.txt` holds the full check output.

### 11.2 Pass criteria (host) [obs]

| Criterion | Result |
|---|---|
| `file` = Mach-O 64-bit arm64, both | PASS (11.1) |
| linked libraries only from iPhoneOS16.5.sdk | PASS. dylib: `IOKit.framework/Versions/A/IOKit`, `IOSurface.framework/IOSurface`, `/usr/lib/libobjc.A.dylib`, `/usr/lib/libSystem.B.dylib`, `/usr/lib/libc++.1.dylib`; selftest: `IOSurface`, `CoreFoundation`, `libSystem.B` (it `dlopen`s libsweetgl, so it does not link it) |
| every undefined symbol resolves against the SDK `.tbd` files | PASS: `gpu/ios/check-sdk-symbols.py` -> dylib 219 undefined / 0 unresolved, selftest 29 / 0, `RESULT PASS`. The script reads the arm64-ios blocks of each tbd v4 document, follows re-exported libraries (libSystem -> libsystem_*), skips the image's own `LC_ID_DYLIB`, requires a two-level namespace, and has a negative control (`_eventfd` must not resolve in libSystem) |
| exports | 14 `sweetgl_*` + 1300 `gl[A-Z]*` (libglapi_bridge public entries) [obs `llvm-nm -gU`]; nothing else |

Limitation [guess]: tbd files carry no per-symbol availability, so a symbol introduced after iOS 16.0 would still "resolve"; the binaries
target minos 16.0 against the 16.5 SDK, and the phone runs 16.7, so this only matters for 16.0-16.4 devices.

### 11.3 Build commands

```
# Mesa clone: pristine 26.2.5 + gpu/mesa-patches/0001..0006 in order (all six apply with git apply --check on HEAD [obs])
nix-shell $A/gpu/nix/shell.nix --run 'bash <repo>/gpu/ios/configure-ios.sh'      # unchanged (lane G); meson picks up targets/sweetgl
flock $A/scratch/heavy.lock nix-shell $A/gpu/nix/shell.nix --run 'export PATH=$A/gpu/ios/bin:$PATH; ninja -C $A/gpu/build/mesa-ios -j6 \
   src/gallium/targets/sweetgl/libsweetgl.dylib src/gallium/targets/sweetgl/sweetgl_selftest'
cp $A/gpu/build/mesa-ios/src/gallium/targets/sweetgl/{libsweetgl.dylib,sweetgl_selftest} $A/gpu/out/
PATH=$A/gpu/ios/bin:$PATH ldid -S $A/gpu/out/libsweetgl.dylib; ldid -S $A/gpu/out/sweetgl_selftest
nix-shell $A/gpu/nix/shell.nix --run 'python3 <repo>/gpu/ios/check-sdk-symbols.py $A/localbuild/theos/sdks/iPhoneOS16.5.sdk $A/gpu/out/libsweetgl.dylib $A/gpu/out/sweetgl_selftest'
```
Step 1 of G3 (the Mesa libraries) needed **no further Darwin patch**: lane G's first `ninja -k 0` (278 targets, `ninja-ios.log`) and the
remaining 898 targets (libmesa, libgallium, glsl, nir, glapi, glapi_bridge, panfrost compiler/lib/decode/winsys, `ninja-ios2.log`) built
with 0 FAILED [obs]. Link: no unresolved symbol; ld64 warnings only for static initializers (glsl builtins, the shim's constructor).

### 11.4 Patch 0006 (`gpu/mesa-patches/0006-panfrost-sweet-libsweetgl-target-iosurface-import.patch`, also `$A/gpu/patches/`)

sha256 `7f30d2fd08f655b0873de9ed0efe67624086ca9abac5c5af880654536703dee6`, 2590 lines, on top of 0001-0005 (exported through a temporary
index; the clone's HEAD stays pristine 26.2.5).

| Part | What |
|---|---|
| `sweet_kmod.h` / `sweet_iokit.c` | transport gains optional `import_surface` (selector 5: in IOSurface id; out handle, GPU VA, length, bytesPerRow, width, height, pixelFormat [src patch 0105 `importSurface`]) and `bo_cache_sync` (selector 6) |
| `sweet_kmod.c` | `sweet_kmod_import_surface()` maps a surface and parks `{handle, va, size, cpu}` (64 slots); `bo_import` takes the parked entry (CPU access = the caller's own surface mapping, else `NO_MMAP`); `sweet_kmod_import_drop()`, `sweet_kmod_import_size()`, `sweet_kmod_bo_cache_sync()` |
| `pan_kmod.c` | `pan_kmod_bo_import`: on the sweet backend the size comes from the parked import instead of `lseek()` on a dma-buf (1 hunk) |
| `src/gallium/meson.build` | `subdir('targets/sweetgl')` when `-Ddarwin-sweet=true`, Darwin, panfrost |
| `src/gallium/targets/sweetgl/` | the new target, below |

### 11.5 The pieces (all in `src/gallium/targets/sweetgl/`, MIT)

* **(b) frontend `sweetgl_frontend.c`**, modelled on `frontends/hgl/hgl.c` [src]: a `pipe_frontend_screen` over `panfrost_drm_screen_create(open("/dev/null"))`
  (the sweet backend's iokit transport is selected by `sweet_kmod_get_transport()` before any DRM call), a hand-built `driOptionCache` from
  `driinfo_gallium.h` + `driinfo_panfrost.h` (xmlconfig is disabled, doc 32 s.7 item 8), `st_api_create_context(API_OPENGLES2, 2.0..3.1)` with an
  empty visual (surfaceless; `st_api_make_current(st, NULL, NULL)`), `get_egl_image` returning IOSurface resources. API in `sweetgl.h`.
  IOSurface path: selector 5 -> placeholder "prime fd" registered in `sweet_drm.c` -> `resource_from_handle(WINSYS_HANDLE_TYPE_FD, stride = bytesPerRow,
  DRM_FORMAT_MOD_LINEAR)` -> `pan_kmod_bo_import` -> parked handle. Formats: `'BGRA'` -> B8G8R8A8_UNORM, `'RGBA'` -> R8G8B8A8_UNORM.
* **(a) libdrm stand-in `sweet_drm.c`**: every `xf86drm.h` function of 0005. Syncobjs are a 1024-entry userland table; **every wait is a wait for the
  whole SweetMali queue** (`sweet_kmod_wait_idle`, absolute `os_time_get_nano` deadline). Reason [src `pan_jm.c` sweet branch of 0003]: the sweet submit
  path does not pass `out_sync`, and `ctx->syncobj` is created signalled and reused for every batch, so a per-syncobj "signalled" flag would let a
  fence return before its work ran. Over-waits, never under-waits. Sync-file fds are real fds (`/dev/null`) because Mesa `close()`s them. `drmIoctl`,
  prime export, caps: ENOSYS/EINVAL. `drmGetVersion` returns name `"sweet"` (never reached: the backend is chosen before it).
* **(c) EAGL shim `sweetgl_eagl.c`** (C on the Objective-C runtime, no Foundation): replaces, on `EAGLContext` only if the class itself defines them
  (`class_copyMethodList`, so an inherited NSObject method is never swizzled), `initWithAPI:properties:` (API 2/3 -> our context, ES 3.1 for API 3;
  failure falls through to Apple's init so CA keeps its CPU renderer), `dealloc`, `API` (3), `setParameter:to:` (YES), `getMacroContextPrivate`
  (`{rctx, disp[1036]}`), both `texImageIOSurface:` forms, `attachImage:toCoreSurface:invertedRender:`, `+setCurrentContext:`, `+currentContext`
  (doc 18 s.1.2, doc 33 s.2.1/s.4 P0). IOSurface images in a 32-entry LRU keyed by (id, base address, w, h, bytesPerRow); sampled surfaces that cannot be
  imported fall back to a CPU copy (`glTexImage2D`, BGRA/RGBA only); render targets have no fallback. Installed by a constructor only in `backboardd`
  (or `SWEETGL_EAGL=1`), via `_dyld_register_func_for_add_image` so it waits until OpenGLES has registered the class.
  GLI wrappers: `gen_gli_wrappers.py` generates one typed wrapper per `dispatch.h` typedef (1639) from Mesa's own generated header, so the arm64
  `(rctx, args...)` shift is done by the compiler; `sweetgl_gli.c` adds the doc 33 s.2.2 specials (ARB shader objects -> ES, `glFlush` = `glFinish`,
  `glFinishObjectAPPLE`, `glTestObjectAPPLE`, `glFramebufferParameterivAPPLE` no-op, `glGetString(0x8a1e)` -> NULL without error, client state no-op)
  and a lookup that also strips ARB/EXT/OES/APPLE/KHR. Unmapped slots go to 1036 per-slot traps (log once, return 0).
* **The two doc 36 crash causes, designed around:** (1) every GLI wrapper and special calls `sweetgl_gli_ensure()`, which makes rctx current (taking it
  over from the creating thread if needed) and then checks **Mesa's own TLS** (`_mesa_glapi_get_context()`); without a context the call is skipped and
  logged, never run on NULL. `sweetgl_context_create` does not leave the context bound, `sweetgl_get_current` cross-checks `st_api_get_current()` and the
  owner thread. (2) an IOSurface whose width/height/bytesPerRow/base read as zero (the sel-6 zero-reply client) is refused in the shim before any use;
  surfaces the kernel reports with zero or inconsistent geometry are refused in the frontend too.
* **(d) `libsweetgl.dylib`**: meson `shared_library` linking libmesa, libgallium, libglapi, `link_whole` libglapi_bridge, `driver_panfrost`;
  `-exported_symbols_list sweetgl.exports`, `-dead_strip`, frameworks IOKit, IOSurface, CoreFoundation, `-lobjc`.
* **(e) `sweetgl_selftest`**: `dlopen`s libsweetgl (argument, `SWEETGL_DYLIB`, next to the executable, `/usr/lib`), context ES 3.1/3.0/2.0, 256x256
  `GL_RGBA8` renderbuffer FBO; clear (0.2, 0.4, 0.6, 1) -> all 65536 pixels must read `33 66 99 ff`; triangle (-0.5,-0.5)(0.5,-0.5)(0,0.5) green ->
  (128,128) `00 ff 00 ff`, (2,2) and (253,253) the clear colour (same scene as the G1 capture, s.5); last line
  `sweetgl selftest: clear=PASS|FAIL|SKIP tri=...`, exit 0 only for two PASS. `--iosurface` repeats both into a 256x256 BGRA IOSurface (256 KiB =
  16 pages, selector-5 rule) and reads the surface memory after `sweetgl_image_sync_for_cpu` (glFinish + selector 6).
* **(f)** both signed with `ldid -S` (11.1).

### 11.6 Open issues

1. **Not run.** `sweetgl_selftest` on the phone is the next step (needs SweetMali service with patches 0104/0105 and a phone slot). First
   expected trouble points [guess]: `IOServiceOpen` permission for a root CLI; the one-client rule of 0105 (backboardd must not hold it).
2. **GLI slot map missing (the EAGL shim does not install without it).** Apple's slot -> name table (sailfish `ios-dispatch.txt`, reverse engineered
   from GLEngine `_gliInitDispatchTable`) is not on this host (`~/pixelios-artifacts/refs/ios-sailfish` and the dsc extraction `work/dsc` are absent
   [obs]) and must not be invented (AGENTS.md rule 7). Only 6 slots are cited in docs 33/36 (98, 104, 117, 779, 781, 1024). The shim loads the map at
   run time from `SWEETGL_GLI_MAP` or `/usr/share/sweetgl/gli-slots.txt` (`<slot> <glName>` lines); with no map `sweetgl_eagl_install` returns
   `-ENOTSUP` and leaves Apple's EAGLContext alone. Re-deriving the map from the 20H392 cache (doc 33 method) is a separate RE task.
3. **Declined in this step** (one line each): `GL_TEXTURE_RECTANGLE`/`sampler2DRect` emulation and GLSL rewriting (doc 33 s.4 P1 item 7: needs CA's shader
   texts and a device trace to check); the shared-cache binary patch of CA's four texture functions (P1 item 8: needs the cache and refuse-on-mismatch
   addresses, not on this host).
4. Syncobj emulation over-waits (any fence = whole queue) [src reasoning, 11.5 a]; correct but serialises CPU and GPU per fence.
5. Context take-over between threads (`sweetgl_make_current_steal`) assumes the previous thread no longer issues GL calls; the old thread's Mesa TLS
   still names the context [guess: CA creates on one thread and renders on its render thread only, doc 36 s.2].
6. IOSurface import requires the kernel's rules (16 KiB-paged length, `IOBufferMemoryDescriptor` surfaces) and Panfrost's linear-import pitch rules
   (`pan_layout` strict import); a refusal falls back to a copy for sampled surfaces only. Real CA pitches are untested [guess].
7. Cache maintenance: imported surfaces are cleaned+invalidated by the kernel at import and by `sweetgl_image_sync_for_cpu`; CA's own CPU writes to a
   surface after import are not flushed before GPU sampling [guess: needs a selector-6 call per CA update, or uncached surfaces].
8. The host (Linux) build was not rebuilt with 0005+0006 (`pan_kmod.c` now calls `sweet_kmod_import_size`, defined in `sweet_kmod.c`, which the host
   build also compiles [src]; not built [guess that it links]).
9. `check-sdk-symbols.py` cannot see API availability (11.2 limitation).

### 11.7 GLI slot map (2026-11-16, host only; closes 11.6 item 2 in part)

Output `$A/gpu/gli-slots.txt` (not in git; sha256 a27da00dc330aabeeab600757b9d1c7f5bfd5af7c7c1450ca547430c7dac4a37), 393 `<slot> <glName>` lines,
slots 0..1032; tools `gpu/tools/hfs_extract.py` + `gpu/tools/gli_slots.py` (stdlib Python, run with `python3 -I`).
* Cache: `$A/ios-root-oriole-v2.img` (HFSX; SystemVersion 20H392), `/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/`
  `dyld_shared_cache_arm64{,.01-.42,.symbols}`, all hard links into `[HFS+ Private Data]`; 7-Zip 17 gives "Data Error" on them, `hfs_extract.py`
  resolves the links and extents overflow. Extracted to `$A/gpu/dsc/cache/` [obs]. The IPSW dmgs were not needed.
* GLEngine's local symbols are `<redacted>` in this cache (1876 locals, no names) [obs], so `_gliInitDispatchTable` cannot be found by name.
* Method used instead: every exported `_gl*` of `OpenGLES.framework/OpenGLES` is a thunk `mrs TPIDRRO_EL0; ldr ctx,[tls,#240]; ldr fn,[ctx,#off];`
  `ldr x0,[ctx,#32]; br fn` [obs-RE]. So ctx = {+32 rctx, +40 disp[0]} and slot = (off-40)/8; 440 thunks give 393 slots [obs-RE]. The name is
  Apple's export name; where several names share a slot the core name is listed first and the others as a `# alias` comment (ignored by the
  shim parser, which reads `%u %127s`) [src 0006 sweetgl_gli_fill].
* Check against docs 33/36: 98 glGenTextures, 104 glGetIntegerv, 117 glGetString, 779 glFenceSync, 781 glDeleteSync match [obs-RE];
  **1024 mismatch**: Apple's export at 1024 is `glFramebufferParameteriAPPLE` (1025 `glGetFramebufferParameteriAPPLE`); doc 33 and the shim
  special say `glFramebufferParameterivAPPLE`. Not adjusted. Whether CA passes a value or a pointer must be re-read at 0x18840771c [guess: value].
* Cross-check with GLEngine [obs-RE, partial]: one function at 0x1f6e9ae20 stores 411 GLEngine text pointers with `ldr x9,[x10,#off]; cmp; csel
  x9,x11,x10,eq; adrp/add; str [x9,#off]`, offsets on the same ctx layout (slots 0..1028). 155 thunk slots (incl. 104, 117) are not stored there and
  171 stored slots have no exported thunk; no pointer template table exists in GLEngine `__DATA_CONST` [obs]. So the init is split or partly
  conditional; the full 1036-slot coverage is still unknown.
* Not in the map [obs]: desktop/ARB names the shim special-cases and CA uses (`glAttachObjectARB`, `glGetInfoLogARB`, `glGetHandleARB`,
  `glBindFramebufferEXT`, `glGetIntegerIndexedv`, ...): OpenGLES does not export them, so their slots are not named by this method.

## 12. G6-a follow-up: IOSurface render targets and the lost context (2026-11-17, host only; nothing ran on the phone)

Input: run `20261117T114625Z-g6-a` (`logs/runs/*-g6-a/merged.log`, every line twice: two log sinks) [obs]. The shim installed in backboardd (364
GLI slots), created an ES 3.1 context on `Mali-G78 MC20 (Panfrost)`, then refused every CA IOSurface and skipped ~9000 GL calls; no crash.

### 12.1 Causes [obs log + src]
* **-22**: not the kernel. Selector 5 mapped the surfaces (the frontend's `surface N refused: fmt 0x77333072 1125x2436 bpr 4608 len 11239424`
  line comes after the import); `format_from_iosurface()` in `sweetgl_frontend.c` knew only `'BGRA'`/`'RGBA'` and returned `-EINVAL` for
  `'w30r'` (48 refusals, bpr **4608**), `0x0` (51; 600x600, 1105x1160, 375x812, 1380x1812, ...), `'b3a8'` (4; 1125x2436 bpr 4544, len 13893632,
  more than bpr*h: a second plane or metadata [guess]) and `'LA08'` (2). `attachImage` had no fallback, so CA's display FBOs had no storage.
* **The skipped calls**: not caused by the refusal. `bind_locked()` (frontend) on a take-back: thread A's Mesa TLS still names the context, so the
  early `return 0` ran without setting `owner = A`/`bound`; `sweetgl_get_current()` then returned NULL on A for ever, so `sweetgl_gli_ensure()`
  logged `make_current 0` and skipped every call. Order in the log: `taken over` (l.1558) -> first refusal (l.1563) -> skips.

### 12.2 Changes (Mesa patch 0007 `gpu/mesa-patches/0007-sweetgl-w30r-attach-copy-fallback-context-steal.patch`, on 0006)
* `bind_locked`: the take-back path re-marks owner/bound; the `taken over` line is logged 8 times at most.
* `'w30r'` = `kCVPixelFormatType_30RGBLEPackedWideGamut`, "little-endian RGB101010, 2 MSB are zero, wide-gamut (384-895)" [src SDK 16.5
  `CoreVideo.framework/Headers/CVPixelBuffer.h:87`]; bit order read like the neighbouring `'l10r'` ARGB2101010LE (`:88`): R 29:20, G 19:10, B 9:0
  [src, by analogy; Metal twin `MTLPixelFormatBGR10_XR`, `MTLPixelFormat.h:78`]. Memory layout = `PIPE_FORMAT_B10G10R10X2_UNORM` (`u_format.yaml`
  UN10,UN10,UN10,X2 from the LSB, swizzle Z,Y,X,1), renderable on Panfrost incl. v9 (`pan_format.c` `FMT(B10G10R10X2_UNORM, RGB10_A2_UNORM, BGR1,
  L, VTR_IB)`, `_R` = render target; blend `BFMT(B10G10R10X2_UNORM, R10G10B10A2)`) [src Mesa 26.2.5]. **Not imported zero-copy** all the same: the
  codes are extended range (384 = 0.0, 895 = 1.0), a UNORM target writes 0..1023, so blacks/whites would land outside the display range.
* Copy fallback (`sweetgl_eagl.c`, conversion in `sweetgl_convert.h`): `attachImage` of a non-importable `'w30r'`/`'BGRA'`/`'RGBA'` surface gives
  CA's texture GPU storage (`GL_RGB10_A2` for w30r, `GL_RGBA8` else), remembered per (context, texture) in 16 slots (a newer texture for the same
  surface replaces the entry; `glDeleteTextures` and context dealloc drop it). The FBO bound when CA calls `glBindFramebuffer`/`glFlush`/
  `glFinishObjectAPPLE`/`glFenceSync` marks its colour texture dirty; on Flush/FinishObjectAPPLE/FenceSync each dirty one is read back
  (`glReadPixels` RGBA/`UNSIGNED_INT_2_10_10_10_REV`, allowed for RGB10_A2 by ES 3.0, Mesa `readpix.c:953`), `IOSurfaceLock(0)`, converted row by
  row into the surface's bytes per row (c -> 384 + round(c*511/1023), padding untouched), `IOSurfaceUnlock`. GL state touched (read FBO, pack
  buffer, pack alignment/row length, texture binding) is restored. Sampled w30r surfaces (texImageIOSurface fallback) convert the other way.
* `0x0`, `'b3a8'`, `'LA08'`: no import (the kernel is no longer asked for formats the frontend cannot use), one log line per distinct
  (format, w, h, bpr) with plane count, bytes/element, element size and alloc size; `attachImage` returns NO, logged once per shape; the context
  stays current, so only that FBO is incomplete and other draws run.
* Host test `sweetgl_convert_host_test.c` (cc -fsanitize=address,undefined): mapping for w30r/BGRA/RGBA and none for 0/b3a8/LA08, XR end points,
  monotonic, round trip within 1 code, channel placement, the g6-a shape 1125x2436 bpr 4608 (padding untouched, last pixel offset), short-bpr
  refusal: **24/24 PASS** [obs].

### 12.3 Build [obs]
Same commands as 11.3 (ninja -j4 under `heavy.lock`, 0 compiler warnings in the target, ld static-initializer warnings as before), `ldid -S`,
`check-sdk-symbols.py` RESULT PASS. `$A/gpu/out/libsweetgl.dylib` 20938912 B, sha256 `11f6b32ce0a2bd86a43d0c14e8a650cd06b643c0cb832fab88c8e67e1944c976`
(previous build kept in `$A/gpu/out/g6a-prev/`). Overlay `$A/gpu/g5/ov-g6a` (`/usr/local/lib/libsweetgl.dylib` 0755); archive with
`--zlib-above 1048576` 4725360 B, sha256 `ca2e3a5536fb1cd60ed4b7456b624bab5db5af6a18fa2a86e2d9b84a08869bbb` (re-pack identical).

### 12.4 Open [guess]
1. Cost: one 1125x2436 readback + CPU conversion (~11 MB each way) per dirty display surface per flush; a GPU pass into a zero-copy
   `B10G10R10X2_UNORM` import with the XR mapping in the shader would remove it.
2. Dirty tracking assumes CA leaves or flushes the FBO it drew to; a surface drawn and handed on without a bind/flush/fence is not copied.
3. w30r bit order is from the header comment by analogy with 'l10r' (no device check yet); fmt 0x0 surfaces are unidentified until the new log line.
4. The take-back still lets two threads alternate one context (as before); safe only if CA serialises them.
