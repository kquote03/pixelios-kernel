# 52: GPU G1: Mesa Panfrost on the host, the `sweet` pan_kmod backend, and the `.malijob` capture format

Date 2026-11-15, lane G (plan 2026-11-14 oneshot-2, step G1). Host only: no phone, no adb/fastboot.
Tags: **[obs]** a command run here, **[src]** read from the cited file, **[guess]** not verified.

## 1. Result

| Item | Value |
|---|---|
| Mesa | 26.2.5, `ddaf85e353442d9d7f192fdcf1823e652e3f2ee3` (doc 49), clone `$A/gpu/src/mesa`, plus `gpu/mesa-patches/0001-0004` |
| Build | `$A/gpu/build/mesa`, 1457/1457 targets, including `mesa_clc`, `vtn_bindgen2`, `panfrost_compile` (the host tools G3 needs) [obs] |
| `clear.malijob` | 2960 B, sha256 `c0582e31d76fb66c05cea99049ba58713831fddc4a708a9cae52e8fa5863c77c` [obs] |
| `tri.malijob` | 4736 B, sha256 `582442fa82ac1356c6a02bb3db2b3228cfa6e4740e504e014b9bee72e9c9abb1` [obs] |
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

Finalize: `sweet_malijob cap000.malijob --out clear.malijob --uniform 0xff996633` (expected crc32 0xff12cb5c, cross-checked with Python
`zlib.crc32` [obs]; checks at (0,0), (255,255), (128,128)); `sweet_malijob cap001.malijob --out tri.malijob --px 2,2,0xff996633 --px
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
| header | 0 | 64 | `magic[8]` = `PIXMJOB1`; u32 `version` = 1, `header_size` = 64, `gpu_id`, `n_chains` (1..8), `n_bos` (1..4096), `flags` (bit 0 = RT block present); u64 `file_size`; u32 `chain_off`, `rt_off`, `bo_off`, `data_off`; u32 reserved[2] |
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
