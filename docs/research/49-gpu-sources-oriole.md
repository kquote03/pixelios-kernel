# 49: GPU userland sources for the oriole GLES path (plan 2026-11-14 oneshot-2, step G0)

Date 2026-11-14. Context: `libsweetgl` (Mesa Panfrost + EAGL shim, docs/research/18, 32, 33, 36) is lost (user decision D2). This note records what public sources were found and cloned, with licence and commit. Nothing here is in git: the clones live under `~/pixelios-artifacts/gpu/src/` (large disk, never committed).

| Source | URL | Ref / commit | Licence | Use | Tag |
|---|---|---|---|---|---|
| Mesa 26.2.5 | https://gitlab.freedesktop.org/mesa/mesa.git | tag `mesa-26.2.5` -> commit ddaf85e353442d9d7f192fdcf1823e652e3f2ee3 (2026-10-14), shallow, 501 MB | MIT (docs/license.rst; per-file headers) | Panfrost gallium driver, Valhall compiler, `pandecode`; may be borrowed with its notice (THIRD_PARTY.md, tools/license-map.txt) | observed (git clone) |
| Google GPU modules | https://android.googlesource.com/kernel/google-modules/gpu | branch `android-gs-raviole-6.1-android16` -> 696fc0246c17ba4270c11f88ab742ad4040938de (2025-06-07), 18 MB | GPL-2.0 WITH Linux-syscall-note (mali_kbase/mali_kbase.h) | register / job ABI reference only (cite file:line; never copied into APSL patches) | observed |
| ios-sailfish (EAGL shim `tools/sweetgl`) | https://github.com/healing-amber/ios-sailfish | repository exists but is EMPTY (GitHub API size 0, no default-branch content) | none | not usable | observed (gh api) |
| ios-sweet | https://github.com/dipa-studio/ios-sweet | main, MIT | MIT | searched the tree for sweetgl / eagl / mesa / panfrost: no match | observed (gh api tree) |
| ios-flame, hackios-xnu-userland | gh search repos | not found under these names | - | - | observed (gh search) |
| Asahi Linux (AGX) | https://asahilinux.org/docs/ (URL only, not cloned) | - | - | background only: Apple's Metal stack needs an AGX-style driver bundle against the private IOGPUFamily ABI; not a route for a Mali (plan 2026-11-14 section 12) | read from source (plan) / guess |

Consequence (plan rule for G0): the EAGL shim is rewritten from docs/research/18 section 1.2 and docs/research/33 sections 2 and 4 (its GL surface list); Mesa supplies the driver and compiler.

Next (G1): Mesa host build with a `sweet` pan_kmod backend (record transport), `pandecode` of a clear and a triangle job; this needs a nix shell with meson, ninja, LLVM, SPIRV-LLVM-Translator, libclc (Mesa 26.x Panfrost precompiles CLC shaders: check `src/panfrost/meson.build` for the mesa-clc requirement before building).
