# A2: Trusty / GSA / AOC transport as used by the stock oriole kernel

Desk research only. No hardware touched. Date 2026-11-14. Extends `46-aoc-audio-oriole.md` (read that first for the "why").

Tags: **[SRC]** = read from source (file:line at the named commit). **[HW]** = observed in the stock recovery capture. **[DERIVED]** = computed here from [SRC] values. **[GUESS]** = guess, not shown by any source.

## Sources and licences

All GPL code was read for facts and cited. Only short struct and constant listings are quoted. Nothing was copied into the repo.

| Tag | Source | Commit | Licence |
|---|---|---|---|
| `tru` | https://android.googlesource.com/kernel/google-modules/trusty, branch `android-gs-raviole-6.1-android16`, shallow clone in `~/pixelios-artifacts/research-src/trusty` | `2494a3e105ef21fd204c44598e1a960f73400b90` ("Update kernel to ab/13436148", 2025-06-07) | drivers `GPL-2.0-only`; `include/linux/trusty/smcall.h`, `sm_err.h`, `arm_ffa.h` are `MIT` (SPDX lines) |
| `ipc` | https://android.googlesource.com/kernel/google-modules/aoc-ipc, same branch, clone in `~/pixelios-artifacts/research-src/aoc-ipc` | `1fe9b9cf3a2954a4a0c0f9868aa04c9d88c902b9` | `GPL-2.0 OR Apache-2.0` (SPDX in `aoc_ipc_core.h`) |
| `aoc` | `~/.hermes/cache/scratch/src-gm-aoc` (kernel/google-modules/aoc) | `ac69293` | GPL-2.0 (file headers); `aoc-interface-gs101.h` is Apache-2.0 (header text) |
| `gsa` | `~/.hermes/cache/scratch/src-gm-soc-gs`, `drivers/soc/google/gsa/` and `include/linux/gsa/` | `2040787` | `GPL-2.0-only` (SPDX) |
| `dts-gs101` / `dts-rav` | `src-dev-gs101` / `src-dev-raviole` | `a9b5edb` / `81506b9` | as in the repos |
| `kc` | https://android.googlesource.com/kernel/common branch `android14-6.1`: only `include/linux/remoteproc.h`, `include/uapi/linux/virtio_ring.h`, fetched as text into `research-src/kc/` (the stock kernel is 6.1) | branch head `c1e1dfc6e0511b5d1c47184a2c1a4ed29bbe2c06` at fetch time | BSD-3/GPL (kernel UAPI headers) |
| `ref` | `~/pixelios-artifacts/ref/oriole/20261114T072643Z/` (stock recovery capture: `dmesg.clean`, `iomem.txt`, `interrupts.txt`) | n/a | local capture |
| `pk` | pk-dev `tools/mkadt.py`, `device/google/oriole/board.json` | pk-dev HEAD `102e396` (both files unmodified in the worktree) | repo licence |
| `fw` | `/vendor/firmware/aoc.bin` read with `7z` from `~/pixelios-artifacts/stock/oriole/extract/vendor.img`: sha256 `9be5662290149cb014d89b630d2a19b36e98a6580c7898eb9b5874cc28985743`, 15,158,976 bytes. Header words only; the file is not kept in git | factory cp1a.260405.005 | Google firmware, not redistributed |

Note on the Trusty module: the stock `trusty-core.ko`, `trusty-ipc.ko`, `trusty-virtio.ko` are this tree [HW: `ref` dmesg:617, 754, 818]. The build used to produce the stock modules could be a different revision of the same branch; the numbers below match the stock log (API 5, qsz 32, notifyid 1/2), so they are consistent.

## What the stock phone actually did [HW]

From `ref` dmesg (recovery, kernel 6.1.162, stock Trusty):
- `dmesg:642` `trusty version: Project: slider, Build: 15644732, Built: 13:17:42 Jun 15 2026`.
- `dmesg:643` `selected api version: 5 (requested 5)`.
- `dmesg:844-845` `vring0: va(id) ...(ffffffc3) qsz 32 notifyid 1`, `vring1: ... (ffffffc4) qsz 32 notifyid 2`.
  - The "id" is the FF-A memory handle as printed by `%llx` (`tru:trusty-virtio.c:375-377`). It is a 32-bit-looking value here.
- `dmesg:858` `trusty_ipc virtio0: is online` (the secure side sent GO_ONLINE).
- `dmesg:21` and `iomem.txt:186-187`: AOC carveout `94000000-96ffffff` (49152 KiB, nomap) inside `93000000-973fffff : reserved`.
- `dmesg:175` `CPU: All CPU(s) started at EL2`; `dmesg:90` last `kvm-arm.mode=nvhe` (the earlier `mode=protected` on line 89 is overridden); `dmesg:261` `Hyp mode initialized successfully`.
- `dmesg:2068` `sjtag sjtag_gsa: [probe sts read]: SJTAG_GET_STATUS: Success - Cmd status: 0x00000010-0`. That is the GSA mailbox working at boot in stock.
- No "linked to ..." line from `gsa_link_s2mpu` (`gsa:gsa_mbox.c`, only compiled with `CONFIG_GSA_PKVM`): so `CONFIG_GSA_PKVM` is off in the stock modules [GUESS, from the missing log line].

## 1. Trusty SMC calling convention

### Conduit and register use

- **[SRC]** Plain `smc #0`, with x0..x7 in and x0..x7 out; x8 and lr are only saved around it (`tru:drivers/trusty/trusty-smc-arm64.S:18-35`, struct `smc_ret8` at `trusty-smc.h:10-19`).
- **[SRC]** `smc()` passes `r0 = function id`, `r1..r3 = arguments`, the rest zero; it uses `.r0` of the result (`tru:trusty.c:72-81`).
- Fastcall and std-call differ only in bit 31 of the function id (see below). Fastcalls return directly. Std-calls may return "interrupted" codes that the driver must handle.
- **[GUESS]** Our XNU at EL1 reaching BL31/Trusty with `smc #0` as the Linux host does. Stock Linux ran as an nVHE host at EL1 under KVM (`dmesg:90, 175`). Whether our shim runs XNU at EL1 or EL2, and whether anything traps SMC, is not known from sources. Settle with A3.

### Function id encoding [SRC `tru:include/linux/trusty/smcall.h:11-26`]

`id = fast<<31 | smc64<<30 | entity<<24 | function`. The entity field is 6 bits (`0x3F000000`). Fastcall: bit 31 set. SMC64: bit 30 set.

### The ids that matter [SRC `smcall.h`; values [DERIVED]]

Secure-monitor entity is 60 (`smcall.h:41`), Trusted-OS entity is 50 (`smcall.h:38`).

| Name | Kind | Id | Args | Return | Cite |
|---|---|---|---|---|---|
| `SMC_FC_API_VERSION` | fast | `0xBC00000B` | r1 = highest version the client supports (5) | version Trusty selects; `0xFFFFFFFF` (`SM_ERR_UNDEFINED_SMC`) means not implemented, treat as 0 | `smcall.h:143`, `tru:trusty.c:773-791` |
| `SMC_FC_GET_VERSION_STR` | fast | `0xBC00000A` | r1 = -1: returns the string length; r1 = i: returns char i | length (>0), then one char per call (<0 = error) | `smcall.h:119`, `tru:trusty.c:725-754` |
| `SMC_SC_RESTART_LAST` | std | `0x3C000000` | none | resumes the interrupted std-call | `smcall.h:44` |
| `SMC_SC_LOCKED_NOP` | std | `0x3C000001` | none | API < 2 only | `smcall.h:45` |
| `SMC_SC_RESTART_FIQ` | std | `0x3C000002` | none | after `SM_ERR_FIQ_INTERRUPTED`, API >= 1 | `smcall.h:57` |
| `SMC_SC_NOP` | std | `0x3C000003` | r1..r3 = optional kick args (see 3) | `SM_ERR_NOP_INTERRUPTED` (repeat) or `SM_ERR_NOP_DONE`, API >= 2 | `smcall.h:69` |
| `SMC_SC_SCHED_SHARE_REGISTER` / `UNREGISTER` | std | `0x3C000004` / `0x3C000005` | handle lo, handle hi, size | 0 or error; optional | `smcall.h:84, 100-101`, `tru:trusty-sched-share.c:180-200` |
| `SMC_FC_GET_NEXT_IRQ` | fast | `0xBC000003` | for the doorbell IRQ path | | `smcall.h:113` |
| `SMC_SC_VIRTIO_GET_DESCR` | std | `0x32000014` | handle lo, handle hi, size | descriptor size in bytes (< 0 error) | `smcall.h:146` |
| `SMC_SC_VIRTIO_START` / `STOP` | std | `0x32000015` / `0x32000016` | handle lo, handle hi, size | 0 | `smcall.h:147-148` |
| `SMC_SC_VDEV_RESET` | std | `0x32000017` | vdev notifyid | | `smcall.h:150` |
| `SMC_SC_VDEV_KICK_VQ` | std | `0x32000018` | vdev notifyid, vq notifyid | API < 3 only | `smcall.h:151` |
| `SMC_NC_VDEV_KICK_VQ` | std (sent as a NOP arg) | `0x32000019` | vdev notifyid, vq notifyid | API >= 3 | `smcall.h:152` |

Ids for the fast IRQ/FIQ/CPU-suspend calls (`0xBC000001/2`, `7/8/9`) exist (`smcall.h:106-118`) but are not needed for a polled bring-up. Compute check [DERIVED]: `0x80000000|(60<<24)|11 = 0xBC00000B`; `(50<<24)|20 = 0x32000014`.

### Error codes returned in r0 (`tru:include/linux/trusty/sm_err.h:8-26`) [SRC]

`UNDEFINED_SMC 0xFFFFFFFF`, `INVALID_PARAMETERS -2`, `INTERRUPTED -3`, `UNEXPECTED_RESTART -4`, `BUSY -5`, `INTERLEAVED_SMC -6`, `INTERNAL_FAILURE -7`, `NOT_SUPPORTED -8`, `NOT_ALLOWED -9`, `END_OF_INPUT -10`, `PANIC -11`, `FIQ_INTERRUPTED -12`, `CPU_IDLE -13`, `NOP_INTERRUPTED -14`, `NOP_DONE -15` (not an error).

### Std-call loop (NOP / interrupt restart) [SRC `tru:trusty.c:115-291`]

For a std-call other than NOP (`trusty_std_call32`, `trusty.c:242-291`):
1. Take the global `smc_lock` (one std-call at a time; NOPs do not take it). Local interrupts are masked around each SMC (`trusty.c:140-160, 176`; arm64 `local_daif_mask()`).
2. Issue the call. Inner loop (`trusty.c:115-138`): while the result is `FIQ_INTERRUPTED`, call `SMC_SC_RESTART_FIQ`; on `BUSY` retry the **original** call up to 5 times.
3. Outer helper (`trusty.c:160-240`): on `BUSY` sleep 1, 2, 4, ... ms (cap 1000) and retry the original call. `PANIC` marks Trusty dead.
4. `trusty_std_call32` then loops while the result is `INTERRUPTED (-3)` or `CPU_IDLE (-13)`: for `CPU_IDLE` it first waits (up to 10 s, then retries anyway; `trusty.c:222-232`) for another CPU's NOP to finish; then it calls `SMC_SC_RESTART_LAST` with no args (`trusty.c:271-278`).
5. On `INTERRUPTED` it also queues a NOP on this CPU "to eventually re-enter trusty" (`trusty.c:209-215`).

For NOP (the per-CPU `trusty-nop-N` thread, `trusty.c:925-958`): call `SMC_SC_NOP` with args (a queued kick, or zeros). Result `NOP_INTERRUPTED` means repeat; `NOP_DONE` means idle; anything else is logged and, if args[0] was non-zero, still repeated.

For a single-CPU polled design [GUESS]: pin the thread, mask interrupts around each `smc`, and bound the whole loop (the stock loop is unbounded; the busy-retry and `CPU_IDLE` waits are the only timeouts). The sched-share register call is explicitly optional in stock ("attempt to share; it is optional", `tru:trusty.c:1170-1174`); skipping it is expected to be harmless [GUESS].

### API version negotiation [SRC `tru:trusty.c:773-791`] and probe order (`trusty.c:1109-1113`)

1. `SMC_FC_GET_VERSION_STR(-1)` -> length, then once per char. Failure only logs.
2. `SMC_FC_API_VERSION(5)` -> `api_version`. If the answer is `0xFFFFFFFF` it becomes 0. If it is greater than 5 the driver errors out (Trusty must not return more than the client asked for).
3. Then `trusty_init_msg_buf` (FF-A, only if `api_version >= 5`, `trusty.c:612-613`).
- **[HW]** stock result: API 5, version string `Project: slider, Build: 15644732, Built: 13:17:42 Jun 15 2026` (`ref dmesg:642-643`). `TRUSTY_API_VERSION_*`: 1 restart-fiq, 2 SMP, 3 SMP-NOP, 4 PHYS_MEM_OBJ, 5 MEM_OBJ (`smcall.h:137-142`).
- What the API level changes [SRC]: >= 2 uses `SMC_SC_NOP`; >= 3 kicks use NOP args instead of `VDEV_KICK_VQ` (`tru:trusty-virtio.c:144-157`); >= 5 uses FF-A memory objects (section 2).

## 2. Shared memory registration at API 5

**Result: FF-A (Arm Firmware Framework for A-profile) v1.0 memory share, over SMC, to endpoint 0x8000.** It is not the older scheme (the older compat id at API < 5 is `pte | attr<<48`, `tru:trusty.c:352-355`, `trusty-mem.c`; not used on oriole). The "FF-A expected" guess in the plan is confirmed.

### Init, once, at probe [SRC `tru:trusty.c:607-715`]

| Step | Call | Check |
|---|---|---|
| 1 | `FFA_VERSION` (`0x84000063`), w1 = `0x00010000` | `ret >> 16 == 1` (`trusty.c:616-623`; `arm_ffa.h:22-29`) |
| 2 | `FFA_FEATURES` (`0x84000064`), w1 = `FFA_MEM_SHARE` (`0x84000073`) | r0 == `FFA_SUCCESS` (`0x84000061`) |
| 3 | `FFA_ID_GET` (`0x84000069`) | r0 == SUCCESS; **local id = r2** (`trusty.c:643-653`); remote id is hardcoded `0x8000` |
| 4 | allocate one page for TX and one for RX (page aligned, kmalloc'd) | PA must be page aligned (`trusty.c:663-681`) |
| 5 | `FFA_RXTX_MAP` 64-bit form `0xC4000066` (`SMC_FCZ_FFA_RXTX_MAP`, `arm_ffa.h:408-413`): x1 = tx PA, x2 = rx PA, x3 = `PAGE_SIZE/4096` | r0 == SUCCESS |

Ids [DERIVED from `arm_ffa.h`, entity 4, fastcall `0x84000000|fn`]: ERROR `0x84000060`, SUCCESS `0x84000061`, VERSION `0x63`, FEATURES `0x64`, RXTX_MAP `0x66` (SMC64 `0xC4000066`), RXTX_UNMAP `0x67`, ID_GET `0x69`, MEM_LEND `0x72`, MEM_SHARE `0x73`, MEM_RECLAIM `0x77`, MEM_FRAG_RX `0x7A`, MEM_FRAG_TX `0x7B` (`arm_ffa.h:315-595`). Error codes: NOT_SUPPORTED -1, INVALID_PARAMETERS -2, NO_MEMORY -3, DENIED -6 (`arm_ffa.h:281-284`).

### Share one contiguous buffer [SRC `tru:trusty.c:303-479`]

1. Fill the TX page (descriptor layout below), then `SMC_FC_FFA_MEM_SHARE` (`0x84000073`): **w1 = total descriptor length, w2 = fragment length (both equal for one fragment), w3..w7 = 0**. The "address/page_count" registers stay 0 because the descriptor is in the TX buffer.
2. Expect r0 = `FFA_SUCCESS`; the **memory handle = r2 | (r3 << 32)** (`trusty.c:434-438`). `FFA_MEM_FRAG_RX` (`0x8400007A`) is only returned when the descriptor needs more than one page; for our small shares it never occurs. Any other r0 is an error (`trusty.c:452-455`).
3. Reclaim: `SMC_FC_FFA_MEM_RECLAIM` (`0x84000077`), w1 = handle lo, w2 = handle hi, w3 = 0. Expect SUCCESS. `FFA_ERROR` with r2 == `DENIED` (-6) means Trusty still holds it (`trusty.c:529-545`).

### Descriptor layout in the TX page [SRC `tru:include/linux/trusty/arm_ffa.h`; offsets [DERIVED] with `STATIC_ASSERT` sizes]

`struct ffa_mtd` (32 bytes, `arm_ffa.h:231-242`), then `emad[1]` (16 bytes), then the composite descriptor:

| Offset | Size | Field | Value written by Linux (`trusty.c:365-401`) |
|---|---|---|---|
| 0 | 2 | `sender_id` | local id from `FFA_ID_GET` r2 |
| 2 | 1 | `memory_region_attributes` | from pgprot: normal WB cached = `(2<<4)|(3<<2)` = 0x2C, OR inner-shareable `3` = **0x2F** (`arm_ffa.h:113-121`, `trusty-mem.c:95-118`) |
| 3 | 1 | reserved | 0 |
| 4 | 4 | `flags` | 0 (share, not lend) |
| 8 | 8 | `handle` | 0 for MEM_SHARE |
| 16 | 8 | `tag` | 0 |
| 24 | 4 | reserved | 0 |
| 28 | 4 | `emad_count` | 1 |
| 32 | 2 | `emad[0].mapd.endpoint_id` | **0x8000** |
| 34 | 1 | permissions | `FFA_MEM_PERM_RW` = 2 for read-write, `FFA_MEM_PERM_RO` = 1 for read-only (`arm_ffa.h:136-137`) |
| 35 | 1 | mapd flags | 0 |
| 36 | 4 | `comp_mrd_offset` | **48** (= `offsetof(ffa_mtd, emad[1])`) |
| 40 | 8 | reserved | 0 |
| 48 | 4 | composite `total_page_count` | bytes / 4096 |
| 52 | 4 | `address_range_count` | `nents` (1) |
| 56 | 8 | reserved | 0 |
| 64 | 8 | constituent `address` | **physical address** (4 KiB aligned; Linux uses the dma address of the trusty device, which is the PA here) |
| 72 | 4 | constituent `page_count` | bytes / 4096 |
| 76 | 4 | reserved | 0 |

Total length for one range = 64 + 16 = **80 bytes** [DERIVED]. Linux fills it for 48-bit PAs (`trusty.c:392-395`, DMA mask `trusty.c:1100-1106`).

Which permission for what [SRC `tru:trusty-ipc.c:224-261`, `trusty-virtio.c:357-359, 630-632`]: RX buffers (Trusty writes) and vring pages and the descriptor page are shared read-write (`PAGE_KERNEL`); TX buffers (Trusty only reads) are shared read-only (`PAGE_KERNEL_RO`).

Not established [GUESS]: the cache-coherence requirement. Linux maps these pages WB inner-shareable and the secure side maps them with the matching attributes; XNU must use matching mappings (or clean/invalidate explicitly). Settled only by the A5 loopback.

## 3. Trusty virtio / TIPC transport

Virtio here is the **legacy split virtqueue** with a remoteproc-style resource table as the device descriptor, and the "DMA address" of every buffer being the **FF-A handle** (not an address).

### 3.1 Fetching the device descriptor [SRC `tru:trusty-virtio.c:162-176, 597-705`]

1. Allocate one zeroed page (`descr_buf_sz = PAGE_SIZE`), share it **read-write** (section 2) -> `descr_id` (handle).
2. `SMC_SC_VIRTIO_GET_DESCR` (`0x32000014`): w1 = `(u32)descr_id`, w2 = `descr_id >> 32`, w3 = 4096. Return value (>= 0) = descriptor size in bytes. Linux uses `trusty_std_call32`, so the full restart loop applies.
3. Parse the descriptor (below) and register the virtio devices. For each device Linux's virtio core calls the config ops, which are implemented by reading/writing the same descriptor page (status byte, features, vring `da`/`pa`).
4. After all devices probed: `SMC_SC_VIRTIO_START` (`0x32000015`) with the same (handle lo, hi, size).
- Notes: the call-notifier that polls the used rings after each Trusty return is registered **before** START (`trusty-virtio.c:656-664`). The descriptor page stays shared until `STOP`/teardown (`trusty-virtio.c:793-808`).

### 3.2 Descriptor layout [SRC `kc:remoteproc.h:73-78, 89-92, 258-264, 302-312`; parse code `trusty-virtio.c:513-596`]

```
struct resource_table { u32 ver (must be 1, RSC_DESCR_VER); u32 num; u32 reserved[2]; u32 offset[num]; }   // packed
at offset[i]: struct fw_rsc_hdr { u32 type /* RSC_VDEV = 3 */; u8 data[]; }
  struct fw_rsc_vdev { u32 id; u32 notifyid; u32 dfeatures; u32 gfeatures; u32 config_len;
                       u8 status; u8 num_of_vrings; u8 reserved[2]; struct fw_rsc_vdev_vring vring[]; }
  struct fw_rsc_vdev_vring { u32 da; u32 align; u32 num; u32 notifyid; u32 pa; }
  then config_len bytes of device config, directly after the last vring
```

All packed, little endian. Bounds checks the Linux parser does and we must also do: `descr_sz >= 16`, `ver == 1`, `descr_sz >= 16 + 4*num`, each `offset < descr_sz`, room for hdr, vdev, and `num_of_vrings*20 + config_len` (`trusty-virtio.c:523-596`).

- **Device type for TIPC: `id = 13`** (`VIRTIO_ID_TRUSTY_IPC`, `tru:trusty-ipc.c:2243`). Other `id`s may appear in the table [GUESS]; the TIPC driver binds only 13.
- **TIPC device config** (`struct tipc_dev_config`, packed, `trusty-ipc.c:54-58`): `u32 msg_buf_max_size; u32 msg_buf_alignment; char dev_name[32]`. Defaults when absent: both = `PAGE_SIZE` (`trusty-ipc.c:41-42, 2152-2160`).
- **Feature bits**: `dfeatures` offered, `gfeatures` written back by the driver (`trusty-virtio.c:221-250`). Linux adds `VIRTIO_F_ACCESS_PLATFORM` locally and strips it again before writing (`trusty-virtio.c:229-245`). Device status is a plain write into the `status` byte (`trusty-virtio.c:276-281`). The virtio core ends probe with DRIVER_OK; that is virtio core behaviour, not in the trees fetched [GUESS].
- **[HW]** stock found 2 vrings: `qsz 32`, notifyids 1 (rx) and 2 (tx). The vdev's own `notifyid` (the first arg of `VDEV_KICK_VQ`) is not printed in the log: **unknown**.

### 3.3 Vrings [SRC `tru:trusty-virtio.c:326-410`; `kc:virtio_ring.h:107-112, 199-214`]

For each vring `i` (rx = vring 0, tx = vring 1; the TIPC driver asks for `{rx, tx}` in that order, `trusty-ipc.c:2138-2180`):
1. `size = PAGE_ALIGN(vring_size(num, align))`, where `num` and `align` come from the descriptor entry. `vring_size = ((16*num + 2*(3+num) + align-1) & ~(align-1)) + 6 + 8*num` (`virtio_ring.h:209-214`).
2. Allocate zeroed, physically contiguous memory (`alloc_pages_exact`), share it **read-write**, get `shared_mem_id`.
3. **Write the handle into the descriptor**: `vr->da = (u32)handle`, `vr->pa = (u32)(handle >> 32)` (`trusty-virtio.c:367-373`). So `da` is the low and `pa` the high half of the 64-bit handle. The comment says `pa` is "previously unused 'reserved'".
4. Standard legacy split-ring layout inside that memory: descriptor table (`num` x {u64 addr; u32 len; u16 flags; u16 next}, 16 B each), then the avail ring {flags, idx, ring[num], used_event}, then padding to `align`, then the used ring {flags, idx, ring[num]{u32 id; u32 len}, avail_event} (`virtio_ring.h:107-112, 199-208`). Weak barriers (`vring_new_virtqueue(..., true, ...)`).
- **Descriptor `addr` is the FF-A handle of the buffer** (not an address): the driver's dma-map hook returns `buf->buf_id` (`trusty-virtio.c:707-717`). `len` is `buf_sz` for rx buffers (inbound, page size) and `mb->wpos` for tx (`trusty-ipc.c:434-441`).
- The rx ring is pre-filled with `vring_size` page-sized shared RW buffers before START; tx buffers are allocated on demand (RO share) up to the tx ring size (`trusty-ipc.c:224-261, 342-368, 2138-2210`).

### 3.4 Kick / notify and receive [SRC]

- **Kick (API >= 3, the stock case):** queue a NOP whose three args are `{SMC_NC_VDEV_KICK_VQ (0x32000019), vdev_notifyid, vq_notifyid}` and let the NOP loop issue `SMC_SC_NOP(0x3C000003, 0x32000019, vdev_notifyid, vq_notifyid)` (`trusty-virtio.c:144-157, 489-491`; `trusty.c:963-996`, `nop_work_func` `trusty.c:925-958`). For API < 3 it would be `SMC_SC_VDEV_KICK_VQ` with the same two ids.
- **Receive:** there is no completion interrupt needed for the used ring. After **every** Trusty std-call return the driver runs `vring_interrupt()` on all vqs (`trusty-virtio.c:81-107`, notifier `TRUSTY_CALL_RETURNED`). So a polling client just issues a `SMC_SC_NOP` and then reads both used rings. There is also a Trusty IRQ/IPI path (`irq` node `ipi-range = <8 15 8>`, `dts-gs101/gs101-trusty.dtsi:14-17`, stock IPI listed in `ref interrupts.txt` line 11 as `GICv3 13 Edge trusty`); a polled bring-up does not need it [GUESS].
- **Reset/teardown:** `SMC_SC_VDEV_RESET (0x32000017)` with the vdev notifyid (`trusty-virtio.c:210-219`), `SMC_SC_VIRTIO_STOP` (`0x32000016`), then reclaim every handle.

### 3.5 TIPC framing [SRC `tru:drivers/trusty/trusty-ipc.c`]

Every message in either ring is `struct tipc_msg_hdr` (packed, 16 bytes, `trusty-ipc.c:67-74`) followed by `len` payload bytes:

```
u32 src; u32 dst; u16 reserved; u16 shm_cnt; u16 len; u16 flags; u8 data[]
```

- Total message size must satisfy `rxlen >= 16`, `rxlen <= buf_sz`, and `rxlen - 16 == hdr.len` or the message is dropped (`trusty-ipc.c:2035-2062`).
- Control endpoint address: **`TIPC_CTRL_ADDR = 53`** (`:44`). "Any" = `0xFFFFFFFF` (`:45`). Local channel addresses start at **1024** (`TIPC_MIN_LOCAL_ADDR`, `:47`, allocated by an idr at `:462-470`).
- A control message is a normal message with `dst = 53` (client -> Trusty) or `dst = 53` (Trusty -> client) and payload `struct tipc_ctrl_msg { u32 type; u32 body_len; u8 body[]; }` (`:77-90`). `sizeof(ctrl) + body_len` must equal the payload length (`_handle_ctrl_msg`, `:1942-1950`).
- Control types (`:77-84`): `GO_ONLINE=1, GO_OFFLINE=2, CONN_REQ=3, CONN_RSP=4, DISC_REQ=5, RELEASE=6`.

**Connect** (`tipc_chan_connect`, `:809-858`): src = new local addr (>= 1024), dst = 53, payload = ctrl{type 3, body_len 256} + `char name[256]` (zero-padded, last byte forced NUL). Payload 264 B, message 280 B [DERIVED]. No connect before `GO_ONLINE`: `vds_queue_txbuf` fails with -ENODEV unless the device is online (`:434-452`; set by `_go_online` `:1796-1806`, which Trusty triggers with a `GO_ONLINE` ctrl message after START).

**Connect reply** (`_handle_conn_rsp`, `:1840-1870`): ctrl type 4, body is exactly 20 bytes: `u32 target (our local addr); u32 status (0 = ok); u32 remote; u32 max_msg_size; u32 max_msg_cnt` (`:96-102`). On status 0 the channel is CONNECTED and `remote` is the server's address; a non-zero status means refused.

**Data**: after connect, a message is hdr{src = local, dst = remote, len = payload bytes, flags = 0, shm_cnt = 0} + payload (`fill_msg_hdr` `:549-558`, `tipc_chan_queue_msg` `:774-801`). Replies come back with `dst = our local`.

**Disconnect** (`tipc_chan_shutdown`, `:882-925`): dst = 53, ctrl{type 5, body_len 4} + `u32 target = remote addr`. The server can also send `DISC_REQ` for our `target = local addr` (`_handle_disc_req`, `:1875-1900`). `RELEASE` (type 6, body = `u64 handle`) is the secure side asking us to reclaim a handle (`:1904-1934`); it only matters if `shm_cnt > 0` messages are used, which hwmgr does not need.

`max_msg_size` from the reply bounds our payload; a one-command exchange is 8 bytes out, 12 bytes in (section 4).

### 3.6 Bounds and waits actually used by the GSA client [SRC `gsa:gsa_tz.c:13-15, 57-100, 109-200`]

Connect wait 5 s (`TZ_CON_TIMEOUT 5000`), tx-buffer wait 10 s, reply wait 10 s. On a timeout it tears the channel down. On a queue error it reconnects once. These are reasonable bounds to copy (our plan wants <= 2 s per call and <= 10 s total).

## 4. hwmgr AOC service

Port name **`com.android.trusty.gsa.hwmgr.aoc`** (`gsa:hwmgr-ipc.h:10`; also `.tpu` and `.dsp`, the other two ports; the GSA driver sets up all three contexts at `gsa:gsa_core.c:851-853`).

Message layout [SRC `gsa:hwmgr-ipc.h:12-38`], all `u32`, little endian, **no extra framing beyond the TIPC header**:

| Enum | Value |
|---|---|
| `HWMGR_CMD_RESP` | `1U << 31` |
| `HWMGH_CMD_RESERVED` (sic) | 0 |
| `HWMGR_CMD_STATE_CMD` | 1 |
| `HWMGR_CMD_UNLOAD_IMG` | 2 |
| `HWMGR_CMD_GET_IMG_CONFIG` | 3 (defined, **no caller** in this tree) |

**State command** (`gsa:gsa_core.c:97-131`): request is 8 bytes `{u32 cmd = 1; u32 sub}`, response is 12 bytes `{u32 cmd = 0x80000001; u32 err; u32 state}`. The client accepts only if the received length is exactly 12 (`:114-116`), `rsp.cmd == (1 | RESP)` (`:122`), and `err == 0`; the return value is `state`.

The "commands" named in the task are the `sub` values (`gsa:include/linux/gsa/gsa_aoc.h:80-86`):

| `sub` | Name | Notes |
|---|---|---|
| 0 | `GSA_AOC_GET_STATE` | read-only query |
| 1 | **`GSA_AOC_START`** | takes AOC out of reset and starts the loaded image; the only start call the stock AOC driver uses (`aoc:aoc.c:646-651`) |
| 4 | `GSA_AOC_SHUTDOWN` | used by `aoc.c:1583` as a literal `4` |
| 5 | `GSA_AOC_RELEASE_RESET` | "take AOC A32 out of reset" |
| 6 | `GSA_AOC_RESET` | |

**State values** returned (`gsa_aoc.h:60-64`): `0 INACTIVE` (fw not loaded, AOC in reset), `1 LOADED` (image loaded, still in reset), `2 RUNNING`. After `LOAD_AOC_FW_IMG` the stock driver expects `GSA_AOC_STATE_LOADED` (`aoc.c:1587` checks `rc != GSA_AOC_STATE_LOADED`). **Reply codes:** `err != 0` is mapped to `-EIO` by Linux without distinguishing; no error table exists in the tree for hwmgr (unknown values).

**Unload** (`gsa_core.c:133-165`): request 4 bytes `{cmd = 2}`, response 8 bytes `{cmd = 0x80000002; err}`.

So `GET_STATE` and `START` are sub-commands of a single wire command (`STATE_CMD = 1`). The note `46` and plan wording "GET_STATE (0) and START (1)" is correct as sub-commands; the wire `cmd` word is 1 for both.

## 5. GSA mailbox

### Base, size, IRQ

- Node `gsa-ns` (`google,gs101-gsa-v1`): `reg = <0 0x17c90000 0x1000>` (`dts-gs101/gs101-gsa.dtsi:3-12`) [SRC]; SPI `IRQ_MAILBOX_GSA2NONTZ_GSA`, level high.
- **[HW]** `ref iomem.txt:58` `17c90000-17c90fff : 17c90000.gsa-ns`; `ref interrupts.txt:163` `GICv3 479 Level gsa-mb-irq` (so INTID 479; for the SPI number subtract 32 = SPI 447).
- DMA mask: the GSA driver sets a **36-bit** DMA mask (`gsa:gsa_core.c:826`), the header buffer PA must be < 64 GiB.

### Register map [SRC `gsa:gsa_mbox.c:22-57`]

| Offset | Name | Use by the stock driver |
|---|---|---|
| 0x0000 | `MBOX_MCUCTLR` | defined, not touched |
| 0x0020 | `INTGR0` | defined; not written by the AP driver |
| 0x0024 | `INTCR0` | clear: write 1 to bit 0 to clear the response interrupt (`:297`, `:331`) |
| 0x0028 | `INTMR0` | mask: init writes `0xFFFF` (all masked, `:235`); bit 0 cleared to unmask the response |
| 0x002C | `INTSR0` | raw status (defined, unused) |
| 0x0030 | `INTMSR0` | masked status; handler tests bit 0 (`:137-150`) |
| 0x0040 | `INTGR1` | **write bit 0 = request doorbell to GSA** (`:301`) |
| 0x0048/4C/50 | `INTMR1/INTSR1/INTMSR1` | defined, unused |
| 0x0080 + 4n | `SR0..SR15` | shared registers (`MBOX_SR_NUM = 16`, `:57`) |

### Command sequence [SRC `gsa:gsa_mbox.c:255-340`] (one request/response, `GSA_MB_CMD_*` = u32 id)

1. Write `SR0 = cmd`, `SR1 = argc`, `SR[2+i] = arg[i]` (`argc + 2 <= 16`).
2. `INTCR0 = 1`, then clear bit 0 of `INTMR0` (unmask), then `INTGR1 = 1`.
3. Wait for the response interrupt. **Stock waits with `wait_for_completion()` and no timeout** (`:304`); the plan must add a bound. **[GUESS]** that polling `INTSR0` bit 0 (raw status) would work with the line masked; the stock path uses `INTMSR0` (masked) in the IRQ handler.
4. Response: `SR0 = cmd | 0x80000000` (`GSA_MB_CMD_RSP`, `:342`), `SR1 = err`, `SR2 = rsp_argc`, `SR[3+i]` = response args; `rsp_argc + 3 <= 16`.
5. `INTCR0 = 1`, set `INTMR0` bit 0 again (`:331-334`).
6. Success is `err == 0`; the call returns `rsp_argc` (>= 0).

**Error codes in `SR1`** (`gsa:gsa_mbox.c:345-360`): 0 OK, 1 INVALID_ARGS, 2 AUTH_FAILED, 3 BUSY, 4 ALREADY_RUNNING, 5 OUT_OF_RESOURCES, 6 BAD_HANDLE, 128 GENERIC, 129 INTERNAL, 130 TIMED_OUT, 131 BAD_STATE. Linux maps AUTH_FAILED to `-EACCES`, BUSY to `-EBUSY`, ALREADY_RUNNING to `-EEXIST`.

### `GSA_MB_CMD_LOAD_AOC_FW_IMG` (= 90) [SRC]

- Command ids in `gsa:gsa_mbox.h:16-73`: `AUTH_IMG 1`, `LOAD_FW_IMG 2`, `GET_CHIP_ID 3`, `TEST_CMD_ECHO 34`, `LOAD_TPU_FW_IMG 50`, `LOAD_AOC_FW_IMG 90` (`:50`), `AOC_CMD 91`, `UNLOAD_AOC_FW_IMG 92` (`:52`), `SJTAG_GET_STATUS 100`, `WAKELOCK_ACQUIRE 150`, `WAKELOCK_RELEASE 151`, `LOAD_APP_PKG 180`, `LOAD_DSP_FW_IMG 240`.
- **Arguments (`argc = 4`, `gsa:gsa_mbox.h:90-96`, built at `gsa:gsa_core.c:82-95`)**: `SR2 = header DMA addr lo`, `SR3 = header DMA addr hi`, `SR4 = body PA lo`, `SR5 = body PA hi`.
  - `header` = the first 4096 bytes of `aoc.bin` (`AOC_AUTH_HEADER_SIZE`, `aoc:aoc_firmware.h:16`) in coherent memory the GSA can read (`aoc.c:441-470`).
  - `body` = the physical address `dram_resource.start + AOC_BINARY_DRAM_OFFSET` = **`0x94000000 + 0`** (`aoc.c:459-460`; offset is `0x98000000 - 0x98000000`, `aoc.h:318-320`). The body must already be copied there (the AOC driver does that **before** the mailbox call, `aoc.c:612-614`).
  - The call returns `rsp_argc` (0 expected); on non-zero `err` the stock driver gives up.
- `LOAD_AOC_FW_IMG` is in the "data transfer" list (`gsa:gsa_mbox.c:435-464`). With `CONFIG_GSA_PKVM` on, the driver first sends `WAKELOCK_ACQUIRE (150)` and resumes the GSA S2MPU, and releases after (`:466-560`). **Stock appears to have `CONFIG_GSA_PKVM` off** ([HW] no "linked to" log; section above), so those two extra commands are probably not needed on the stock kernel [GUESS].
- The header comment says GSA authenticates the header, **locks the memory region of the body** so the AP can no longer access it, authenticates the body, then configures the AOC for execution (`gsa:include/linux/gsa/gsa_aoc.h:17-35`) [SRC]. That implies the AP must not touch the carveout through any mapping after this point, except the IPC area afterward is accessed by stock (see section 6): **unknown which sub-ranges stay AP-accessible**.

### Version / echo query

- **No version query is exposed** in `gsa_mbox.h`. `GSA_MB_CMD_GET_CHIP_ID = 3` is only in the enum (no caller). `GSA_MB_TEST_CMD_ECHO = 34` is in the enum; the first-party tree has **no user** and no argument layout, so its argc/echo behaviour is unknown, and test commands may be compiled out of production GSA firmware [GUESS].
- **Best harmless first probe: `GSA_MB_CMD_SJTAG_GET_STATUS` (= 100)**, argc 0, expects 3 response args (`gsa:gsa_core.c:476-497`, `SJTAG_STATUS_RSP_ARGC = 3` at `gsa_mbox.h:181-187`). **[HW]** the stock sjtag probe issues exactly this at boot and logs success (`ref dmesg:2068`). This exercises the whole mailbox path with no side effect on AOC state.

## 6. AOC firmware load in the stock driver

### 6.1 Validity checks [SRC `aoc:aoc_firmware.c`]

Only the signed layout is accepted: `_aoc_fw_is_signed` requires an auth header v1 or v2 (`:163-182`), else "Loading unsigned aoc image is unsupported" (`aoc.c:600-604`).

**Auth header** (first 4096 bytes; `struct aoc_auth_header`, `aoc_firmware.c:24-51`): `u8 signature[512]; u8 key[512];` then (v1) `{ u32 magic; u32 generation; u32 rollback_info; u32 length; u8 flags[16]; u8 hash[32]; u8 chip_id[32]; u8 auth_config[256]; u8 image_config[256] }`. v1 = `generation == 1` and v2 = `generation == 2` with `hash[64]`; magic `0x00434F41` ("AOC\0", `:13`).

**[HW from `fw`, stock image]** the header at offset 0x400 is: magic `0x00434F41`, generation 1 (so **v1**), rollback_info 1, length `0x00E73EC0` (15,154,880 = file size - 4096, consistent), flags `[0]=0x400`. The body starts at file offset 0x1000.

**Superbin header** (at file offset 4096, `aoc_firmware.c:53-78`):
`magic 0xaabbccdd; release_type; reserved; image_size; bootloader_low; bootloader_high; bootloader_offset; bootloader_size; uuid_table_offset; uuid_table_size; version_prefix[8]; version_string[64]; section_table_offset; ...`

Checks in `_aoc_fw_is_valid` (`:116-154`), all must pass:
1. data present, header region inside the image minus the 4096 header (`region_is_in_firmware(start,len) = start+len < fw->size - 4096`, `:106-110`);
2. `magic == 0xaabbccdd`;
3. uuid table inside the image;
4. bootloader region inside the image.

`_aoc_fw_is_compatible` (`:243-260`): only for `release_type == 1`, the 16-byte UUID entries at `uuid_table_offset` must match the table built into `aoc-interface-gs101.h` (`AocInterfaceCheck`, `aoc-interface-gs101.h:2555-2840`). A mismatch is "firmware and drivers are incompatible" and the load stops (`aoc.c:595-598`). Since we use the **stock** image the check passes by construction; an XNU client does not need to repeat it [GUESS], but must use the same command ids (the ids in `aoc-interface-gs101.h` are the ones for this exact interface hash `369ba3d57af1d698f311a3a33f876930b6eb2b3b`, header comment lines 20-24).

**[HW from `fw`]** stock superbin header words: `magic=0xaabbccdd`, `release_type=1` (release), `image_size = 0x01400000`, bootloader fields `{low 0, high 0x98000000, offset 0x1000, size 0x7000}`, `uuid_table_offset 0x8000`, `uuid_table_size 0x2030`, version string `13820614-polygon` (prefix `version`).

**IPC offset** = `image_size` = **0x01400000** (`_aoc_fw_ipc_offset`, `aoc_firmware.c:268-272`), i.e. the shared control area is at carveout + 20 MiB = **`0x95400000`** [DERIVED from `fw` + `aoc.c:569`].

### 6.2 Carveout use [SRC]

- DT: `memory-region = <&aoc_reserve>` (`dts-gs101/gs101-aoc.dtsi:48`), **[HW]** `aoc@94000000`, `0x94000000..0x96FFFFFF`, 48 MiB, nomap (`ref dmesg:21`). Stock maps it write-combined (`aoc.c:2614`, `devm_ioremap_wc`).
- The AOC sees DRAM at **IOVA `0x98000000 + (pa - 0x94000000)`** (`aoc_dram_translate_to_aoc`, `aoc.c:187-198`; `AOC_BINARY_DRAM_BASE = 0x98000000`, `aoc.h:318`).
- Layout [DERIVED]: image body at +0; IPC/control area at +`image_size` (+20 MiB); three DMA-buf heaps carved from the **top** (`aoc.c:2338-2368`): sensor-direct 4 MiB at `0x96C00000`, playback 16 KiB at `0x96BFC000`, capture 64 KiB at `0x96BEC000` (sizes `aoc.h:42-44`). Their AOC-side addresses are `0x9AC00000`, `0x9ABFC000`, `0x9ABEC000`.
- Default before any load: `aoc_control = dram + 6 MiB` (trace32 case, `aoc.c:2693`); after a real load it is set to `dram + ipc_offset` (`aoc.c:610`).

### 6.3 Order of operations in `aoc_fw_callback` [SRC `aoc:aoc.c:472-674`]

1. `request_aoc_on(1)`: `iowrite32(1, aoc_req_virt)` where `aoc_req` = `0x175D0000` (`aoc_v1.c:87-91`, reg-name `aoc_req`, `dts-gs101/gs101-aoc.dtsi:31-36`; line `aoc.c:557`). The matching status read is at `aoc_req + 0x40` (`aoc_v1.c:93-106`; used only by the reset path, `aoc_v1.c:137-138`, and `aoc.c:1568-1569`; a 2 s poll with `msleep(100)`). **[HW]** `iomem.txt:54` `175d0000-175d0fff : 19000000.aoc aoc_req`.
2. `_aoc_fw_is_valid`, `_aoc_fw_is_compatible`, `_aoc_fw_is_signed` (`aoc.c:564, 595, 600`).
3. `aoc_control = dram + ipc_offset` (`:610`), copy the **body** (file offset 4096 to end) to carveout + 0 (`_aoc_fw_commit`, `aoc_firmware.c:286-294`; `aoc.c:612-614`).
4. `aoc_configure_iommu_fault_handler` then `gsa_load_aoc_fw_image(header_dma, 0x94000000)` (`:624-625`). The non-GSA branch that would program the SysMMU from the header and write a reset trampoline (`aoc_configure_iommu`, `write_reset_trampoline`, `:632`) is **not taken** on oriole (`gsa-enabled` is set in DT, `gs101-aoc.dtsi:45`).
5. Write the **parameter block** at the IPC address, `aoc_pass_fw_information` (`:635`, writer `:384-399`):
   ```
   u32 magic = 0x0a0cda7a (AOC_PARAMETER_MAGIC, aoc.h:322)
   u32 count
   u32 total_bytes = 12 + count*12
   then count x { u32 key; u32 size = 4; u32 value }     // 12 bytes each
   ```
   All writes are `writel_relaxed`. The block overlays the future control block address; the firmware reads it at boot [GUESS about the exact moment it is overwritten by `AOC_MAGIC`].
6. `aoc_state = FIRMWARE_LOADED`; the driver disables AP system sleep "for 2 sec for aoc boot" (`:638-642`).
7. `gsa_send_aoc_cmd(GSA_AOC_START)` -> hwmgr `STATE_CMD, sub 1` over TIPC (`:646-651`); on failure it frees the firmware and goes offline.
8. Arm a 5 s delayed `aoc_monitor_online` (`:660`) and `msleep(2000)` (`:663`).

### 6.4 The parameter block entries [SRC `aoc:aoc.h:325-347`, values `aoc.c:503-526, 551-555`]

| Key | Name | Value on oriole | Source |
|---|---|---|---|
| 0x1001 | `kAOCBoardID` | **0x20302** | DT `aoc-board-id` (`dts-rav/gs101-oriole-audio.dtsi:12-16`) |
| 0x1002 | `kAOCBoardRevision` | **0x10000** | DT `aoc-board-rev`. `aoc-board-cfg = "o6"` (string, only logged). Defaults are `0x20202`/`0x10000` (`aoc.c:56-57`) |
| 0x1003 | `kAOCSRAMRepaired` | 0 | `aoc_sram_was_repaired()` is hard-wired false (`aoc.c:367`) |
| 0x1005 | `kAOCCarveoutAddress` | `0x94000000` (**CPU PA as a u32**, not the AOC IOVA) | `aoc.c:507` |
| 0x1006 | `kAOCCarveoutSize` | `0x03000000` | `aoc.c:508` |
| 0x1007/0x1008 | sensor direct heap addr/size | `0x9AC00000` / `0x00400000` (AOC-view) | `aoc.c:509-510` |
| 0x100C/0x100D | playback heap addr/size | `0x9ABFC000` / `0x4000` | `aoc.c:511-512` |
| 0x100E/0x100F | capture heap addr/size | `0x9ABEC000` / `0x10000` | `aoc.c:513-514` |
| 0x1009 | `kAOCForceVNOM` | 0 | DT `force-vnom = <0>` (`gs101-aoc.dtsi:73`) and `force_voltage_nominal` defaults 0 |
| 0x100A / 0x100B | `DisableMM` / `EnableUART` | 0 / 0 | sysfs-set module flags, kzalloc default (`aoc.c:1990, 2041`) [GUESS that the default is 0 on the stock phone] |
| 0x1010 | `ForceSpeakerUltrasonic` | 0 | same (`aoc.c:2058`) |
| 0x1011 | `kAOCRandSeed` | random u32 | `get_random_u32()` (`aoc.c:485`) |
| 0x1012 | `kAOCChipRevision` | `(main<<4)\|sub`; **[HW]** stock reads CPU_REV **0x11** (`ref dmesg:534`) | `gs-chipid.c:348-355, 388-394` |
| 0x1013 | `kAOCChipType` | `chipid[0] & 0xFF` | `gs-chipid.c:330-337, 381-385`; value not in the stock log: **unknown** |
| 0x1014 | `kAOCGnssType` | 0 | DT property absent in the repos grep (`gs101-aoc.dtsi`, `gs101-oriole*.dtsi`) |
| 0x1015 | `kAOCVolteReleaseMif` | 0 | module flag default |
| 0x1016 | `kAOCChipProductId` | `chipid[0] & 0xFFFFF000` = **0x09845000** (GS101) | `gs-chipid.c:71, 75, 381` |
| 0x1017 | `kAOCWifiChip` | 0 | DT property absent |

So the block has 22 entries (the array in `aoc.c:503-526`), `total_bytes = 12 + 22*12 = 276`. `kAOCASVTableVersion` (0x1004) is defined but not sent [SRC]. Chip-id registers: base `0x10000000` (`dts-gs101/gs101.dtsi:76`), product/main-rev word at +0x0, sub-rev at +0x10 bits [19:16] (`gs-chipid.c:115-126`). Whether the AOC firmware needs `kAOCChipType` to be exactly right is unknown.

### 6.5 AOC SysMMU (0x1A090000) and S2MPU (0x1A0B0000) in the stock driver

- **SysMMU:** DT `iommus = <&sysmmu_asoc>` and an iommu group (`gs101-aoc.dtsi:47, 49`). `sysmmu_asoc` is a `samsung,sysmmu-v8` at `0x1A090000` (size 0x10000) with a **secure bank at `0x1A0A0000`** and a secure IRQ (`dts-gs101/gs101-sysmmu.dtsi:45-52`). **[HW]** `iomem.txt:61` the Samsung sysmmu driver claims `1a090000-1a09ffff`.
  - In the GSA path the AOC driver **does not map anything** into the SysMMU (the `iommu_map` loop, `aoc.c:1251-1289`, is only in the non-GSA branch, `aoc.c:632`). It only registers a fault handler that logs and returns `-EAGAIN` (`aoc.c:1208-1229`) and looks up `domain = iommu_get_domain_for_dev` (`aoc.c:2649`).
  - The image header carries an iommu table (`iommu_offset/size` in `image_config`, `aoc_firmware.c:71-87, 196-222`; entries decode as vaddr[31:12], paddr[43:20], size[63:44] pages via `IOMMU_*` macros `aoc_firmware.h:20-23`). In the GSA path it is the GSA that consumes it [GUESS]. **Open:** the SysMMU's reset state if nothing programs it.
- **SSMT** (`0x1A060000`, size 0x5000): the driver writes `SSMT_BYPASS_VALUE = 0x80000000` to `NS_READ_PID(n)` = `0x4000+4n` and the write twin for `n = 0..32`, skipping 31, during probe (`aoc_v1.c:22-24, 317-338`, called at `aoc.c:2656`). **[HW]** `iomem.txt:60` `1a060000-1a064fff`. That is the only AOC-memory-routing register write done by the AP.
- **S2MPU:** the DT lists `aoc_s2mpu` = `0x1A0B0000` as the third `reg` (`gs101-aoc.dtsi:33`) and `aoc.h:36-38` define `AOC_S2MPU_CTRL0`, `..._PROTECTION_ENABLE_PER_VID_CLR`, `..._VID_MASK_ALL`, but **no `.c` file references them or `"aoc_s2mpu"`** (grep over the whole `aoc` tree) and **[HW]** `ref iomem.txt` has no entry at `1a0b0000`. So the stock AP driver never maps or programs this S2MPU; the GSA locks the carveout instead (`gsa_aoc.h:28-35`). **[GUESS]** the GSA/secure side owns it.

### 6.6 "Ready" signal, service discovery and bounded waits

- **Ready signal [SRC `aoc.c:336-352`]:** the AOC firmware raises any of the 16 AOC mailbox channels (`mbox_aoc` at `0x176a0000`, `google,mailbox-whitechapel`, `gs101-aoc.dtsi:9-19`; **[HW]** `ref interrupts.txt:7` `GICv3 104 Level 176a0000.mbox`). The Rx callback, in state `FIRMWARE_LOADED`, checks `aoc_fw_ready()` = `aoc_control != NULL && aoc_control->magic == 0xA0C00A0C` (`aoc.c:201-203`; `aoc_ipc_core_internal.h:61`). If true it starts `aoc_did_become_online` (`aoc.c:1322-1395`). A polled client can simply poll `magic` at carveout + `image_size`.
- **Mailbox registers** [SRC `aoc:mailbox-wc.c:22-37, 88-110`]: same register layout as the GSA's (`INTGR0 0x20, INTCR0 0x24, INTMR0 0x28, INTSR0 0x2C, INTMSR0 0x30, INTGR1 0x40, INTCR1 0x44, INTMR1 0x48, INTSR1 0x4C, INTMSR1 0x50, SHARED 0x80+4i`). The AP **rings the AOC with `INTGR0 = 1<<i`** (`wc_mbox_int_generate`, `:91-98`) and **acks AOC->AP interrupts with `INTCR1 = 1<<i`** (`:100-107`), unmasking with `INTMR1` bit i (`:122-148`). `i` is the **service IRQ index** (below).
- **Control block** [SRC `ipc:aoc_ipc_core_internal.h:62-72`], packed, at `ipc_base = carveout + image_size`:
  `u32 magic (0xA0C00A0C); u32 fw_version; u32 hw_version; u32 status; u64 supported_features; u32 services; u32 service_size; u32 services_offset; u8 fw_version_name[48]; u8 hw_version_name[48]; u64 system_clock_offset` (offsets 0, 4, 8, 12, 16, 24, 28, 32, 36, 84, 132 [DERIVED]).
- **Service table:** `service i` at `ipc_base + services_offset + i*service_size` (`aoc_service_core.c:343-347`). Up to `AOC_MAX_ENDPOINTS = 200` (`aoc.h:300`), names unique, non-empty and < 32 bytes (`aoc.c:228-260`).
  `struct aoc_ipc_service_header` (`ipc:aoc_ipc_core_internal.h:93-98`): `char name[32]; u32 flags; struct aoc_ipc_memory_region regions[2];` with each region (28 bytes, packed) `{u32 offset; u32 size; u32 slots; u32 tx; u32 rx; u32 wp; u32 rp}` (`:76-84`).
  - `regions[0]` = `AOC_UP` (AOC -> AP), `regions[1]` = `AOC_DOWN` (AP -> AOC) (`aoc_ipc_core.h`, `aoc_direction`).
  - Flags (`:47-53`): `0x00010000` ring, `0x01000000` ring-push, `type = (flags & 0x70000)`: 0 queue, 1 ring, 2 buffer; `irq index = (flags & 0x00F80000) >> 19`.
  - `offset` is relative to `ipc_base` (`aoc_service_ring_base`, `aoc_ipc_core.c:700-712`).
- **Queue message** (non-ring): slot = `{ u16 length; u16 r1; u16 r2; u16 r3; payload }` (`aoc_ipc_message_header`, `ipc:aoc_ipc_core_internal.h:95-99`; write `aoc_ipc_core.c:654-690`), index = `tx % slots`, payload bytes at `offset + region.size*(tx % slots)`. Advance `tx` by 1.
- **Ring semantics** (`ipc:aoc_ipc_core.c:53-80, 185-215, 654-690`): `tx`/`rx` are free-running byte counters; `wp`/`rp` are positions modulo `size`. Writer: copy to `offset + wp` with wrap, `wmb`, then `tx += n; wp = (wp+n) % size`. Reader does the same on `rx`/`rp`. Bytes available to read = `tx - rx` (capped at `size`).
- **Bounded waits in stock (so ours can copy or tighten them):** AOC power request poll 2 s (`aoc_v1.c:93-106`); online monitor 5 s then the driver triggers an AOC reset (`aoc.c:660, 1305-1320`); fw-boot AP-sleep hold 2 s (`aoc.c:663`); audio control command reply 500 ms spin (`WAITING_TIME_MS`, `alsa:aoc_alsa.h:67`), after which stock calls `aoc_trigger_watchdog` (resets the AOC): **do not copy that reset**.

## 7. AOC audio control: one TDM0 playback path

Everything runs over two AOC services found by name: **`audio_output_control`** (commands, `CMD_OUTPUT_CHANNEL`, `alsa:aoc_alsa.h:41-42`) and **`audio_playback0`** (PCM, first of `audio_playback0..6`, `alsa:aoc_alsa_drv.c:34-45`). The stock card only probes after `audio_output_control` appears (`aoc_alsa_card.c`, see 46).

### 7.1 Command transport [SRC `alsa:aoc_alsa_hw.c:196-311`]

1. Drain any stale replies from the control service's `AOC_UP` side.
2. `aoc_service_write(cmd)` to `AOC_DOWN` (writes a slot/message as in 6.6, then rings the service's mailbox channel through `signal_aoc` = `mbox_send_message`, `aoc_service_core.c:560-563`).
3. Spin-read `AOC_UP` for a reply, up to 500 ms (`WAITING_TIME_MS`). A **4-byte** reply is an error code (`err == 4` branch, `:277-282`); anything else is the reply (the command echoed, with the `reply` field in the header carrying the result, `:285-290`). Stock sends one command at a time (a mutex).
4. Whether `audio_output_control` is a queue or a ring service is not stated in the driver; the framing above for queues is assumed [GUESS] (to be read from the live table in A4).

### 7.2 Command header and ids [SRC `aoc:aoc-interface-gs101.h`]

`struct CMD_HDR` (packed, 8 bytes, `:419-423`): `{ u8 type; u8 cntr; u16 len; u16 id; i16 reply }`, where `type` = `DATA_TYPE_CMD (0)` (enum `:322-328`), `cntr` = a running counter, `len` = **total command size in bytes** (`AocCmdHdrSet`, `:2499-2511`). Ids (`:53-225`): `CMD_SYS_BANNER 7`, `CMD_SYS_STATS 12`, `CMD_AUDIO_OUTPUT_SINK 200`, `SOURCE 201 (0xC9)`, `BIND 202`, `GAIN 204`, `EP_SETUP 206 (0xCE)`, `SET_PARAMETER 209 (0xD1)`, `GET_PARAMETER 211`, `GET_DSP_STATE 233 (0xE9)`, `CFG_SIGGEN 225`, `BIND2 271 (0x10F)`.

(Correction to note 46: `CMD_AUDIO_OUTPUT_EP_SETUP` is id **206**, not "2".)

### 7.3 Minimal sequence for one speaker playback, as the stock driver does it

All sizes include the 8-byte header [DERIVED from packed structs, `:870-899, 948-966`].

| # | When in stock | Command | Fields | Size | Cite |
|---|---|---|---|---|---|
| 0 | optional read-only sanity | `GET_DSP_STATE` (233) | `u8 mode` | 9 | `alsa:aoc_alsa_hw.c:996-1010` |
| 1 | DAPM mixer "TDM_0_RX Mixer" enabled (`aoc_path_put`, `aoc_alsa_path.c:1222-1262`) -> `aoc_audio_path_open(ep=0 (EP1), hw=TDM_0_RX)` | `BIND2` (271) | `src = ep_id_to_source(0) = 0`, `dst = SINK_SPEAKER = 0`, `bind = 1` | 11 | `aoc_alsa_hw.c:1322-1343, 1635-1660`; sink table `:33-48` (`PORT_TDM_0_RX -> SINK_SPEAKER`) |
| 2 | PCM `prepare` (`aoc_alsa_pcm.c:545-592`): reset the ring write pointer, then | `EP_SETUP` (206) | `d.address=0, d.watermark=48000, d.length=0, d.metadata{chan, bits, sr, format, offset=0}, d.channel=0 (ep idx), d.wraparound=1, mode=0 (PLAYBACK)` | **28** | `aoc_alsa_hw.c:1729-1810`; `PLAYBACK_WATERMARK_DEFAULT 48000` (`aoc_alsa.h:100`) |
| 3 | `trigger START` (`aoc_audio_start`, `aoc_alsa_hw.c:2685-2700`) | `SOURCE` (201) | `source = 0 (ep idx), on = 1` | 10 | `aoc_alsa_hw.c:1257-1287` |
| 4 | PCM data | write to `audio_playback0` ring (7.4) | | | |
| 5 | `trigger STOP` | `SOURCE` (201) with `on = 0`; then unbind `BIND2` with `bind = 0` | | | `aoc_alsa_hw.c:2711-2722, 1322` |

Enum values used [SRC `aoc-interface-gs101.h`]: `SampleRate` `SR_8KHZ 0, SR_16KHZ 1, SR_24KHZ 2, SR_32KHZ 3, SR_44K1HZ 4, SR_48KHZ 5, SR_96KHZ 6, SR_192KHZ 7` (`:736-744`); `Format` `FP 0, FIXED 1, ULAW 2` (`:749-753`); `BitsPerSample` `8 -> 0, 16 -> 1, 24 -> 2, 32 -> 3` (`:755-760`); `Channels` `MONO 1, STEREO 2` (`:762-768`); `EntrypointMode PLAYBACK = 0` (`:938-941`). The stock driver maps unknown rates to 48 kHz and unknown widths to 32 bit (`aoc_alsa_hw.c:1744-1769`).

Entry-point index is the PCM device index; `audio_playback0` is EP1 = index 0 (`google-aoc-enum.h:54-62`; `aoc_alsa_pcm.c:405`). Sink ids: `SINK_SPEAKER = 0` (`aoc-interface-gs101.h:2082-2089`).

Volume: stock sets a sink gain with `SET_PARAMETER` (209): `block = sink + 16`, `component = 0`, `key = source`, `val = volume` (`aoc_alsa_hw.c:329-358`, `AOC_AUDIO_SINK_BLOCK_ID_BASE = 16`, `aoc_alsa.h:95`). The **default AOC gain and the unit of `val` are unknown** from sources. `CMD_AUDIO_OUTPUT_CFG_SIGGEN` (225) exists in the interface (`:1169-1174`: `u8 wf_type; u16 wf_period; u32 wf_amplitude`) but the ALSA driver never sends it; its waveform enum and units are unknown, so it is not a usable safe test tone.

How TDM0 itself (bit clock, frame sync, slot setup) is brought up is **not visible in the AP driver**: the AP never writes any TDM register. The only AP-visible effect of the bind/source commands is the command exchange above, so TDM0 start is inside the AOC firmware [GUESS]. The DT format is `dsp_a` TDM with `cs35l41_l`/`cs35l41_r` as codecs (`dts-gs101/gs101-audio.dtsi:673-687`, per note 46).

### 7.4 The `audio_playback0` ring format [SRC]

- A **ring** service (`flags & 0x10000`), PCM bytes are written **raw, no per-message header**, into `AOC_DOWN` (the AP is the writer). The AP copies up to the free space; stock refuses a write larger than `aoc_ring_bytes_available_to_write(AOC_DOWN)` (`aoc_alsa_hw.c:3076-3112`). The data is interleaved PCM exactly in the format given by `EP_SETUP` (channels, bits, rate).
- Before each stream start the AP resets the write pointer (`aoc_ring_reset_write_pointer(AOC_DOWN)`, `aoc_alsa_pcm.c:560-565`; code `aoc_ipc_core.c:578-600`).
- After writing, ring the doorbell of the service's IRQ index (6.6). Playback position is read by the AP as the AOC's consumed bytes `aoc_ring_bytes_read(AOC_DOWN)` (`aoc_alsa_pcm.c:586`; `rx` counter, `aoc_ipc_core.c:248-259`).
- Ring size and `slots` come from the live service table; `slots` is 1 for rings and the stock driver treats `slots == 0` as "not usable" (`aoc_service_message_slots`, `aoc_ipc_core.c:308-317`; comment `aoc.h:297-299`).

## 8. Is the AOC carveout outside XNU-managed RAM?

**Yes: the carveout `0x94000000..0x96FFFFFF` lies entirely outside the RAM XNU is given.**

- `pk:device/google/oriole/board.json:6-15` lists the DRAM banks: `0x80000000` size `0x80000000` and `0x880000000` size `0x180000000`.
- `pk:board.json:16-19`: `xnu_ram = ["0x880000000", "0x100000000"]`, i.e. XNU RAM is `[0x880000000, 0x980000000)`. `board.json:97-98` notes that the window "must lie inside a hole-free bank with no reserved range inside".
- `pk:tools/mkadt.py:80-96` (`build`): for a non-`sweet` board `phys_base, mem_size = config['xnu_ram']`, and `chosen.dram-base`/`dram-size` are exactly those two values. XNU manages only that range.
- The carveout is in the low bank (`0x80000000..0xFFFFFFFF`), 38 GiB below the XNU window. **[HW]** stock: `ref dmesg:21`, `ref iomem.txt:186-187` (`93000000-973fffff : reserved` containing `94000000-96ffffff : 19000000.aoc`).
- The other secure-world carveouts used by this stack are also low-bank: GSA `0x90200000` (4 MiB, `dmesg:19`), TPU fw `0x93000000` (16 MiB, `dmesg:20`).
- Consequence: XNU has to map the carveout itself (write-combined/device) if it wants to write the image or the rings; the page tables for it are not part of XNU's managed pool. **No test was added** (as instructed). The check that would be added later is "for every `board.json` bank pair, `[0x94000000, 0x97000000)` does not intersect `[xnu_ram.base, xnu_ram.base + xnu_ram.size)`".

## 9. Unknown / how to settle

| # | Unknown | Why it matters | How to settle |
|---|---|---|---|
| U1 | Whether `smc #0` from our EL (EL1 vs EL2) reaches BL31/Trusty untrapped, and whether anything else (hypervisor) intercepts it | all of section 1 | A3: the two version fastcalls, expecting `api=5` and the stock version string |
| U2 | Exact stock Trusty build vs this source tree (the tree is the same branch, not proven to be the same build) | struct sizes, call semantics | A3 version string equals `Project: slider, Build: 15644732`; A5 loopback |
| U3 | The vdev's own `notifyid`, vring `align`, and the full descriptor contents (other devices, dfeatures, config bytes, `dev_name`) | kick args, vring sizing | A5: dump the descriptor page after `GET_DESCR`; dmesg only proves qsz 32 and vring notifyids 1, 2 |
| U4 | FF-A local endpoint id (`FFA_ID_GET` r2) and whether `FFA_FEATURES`/`RXTX_MAP` behave as in Linux on our boot path | share sender id; map failure | A5 first calls; compare to Linux's `trusty_init_msg_buf` order |
| U5 | Cache/shareability requirement for shared pages (WB inner-shareable, attr 0x2F) when mapped by XNU | silent data corruption on the rings | A5: write/read pattern through a TIPC echo-like exchange; or map non-cacheable and compare |
| U6 | Whether the optional sched-share registration is needed by Trusty for normal operation | none, if skippable | A5: skip it; if any call stalls, add it |
| U7 | Status byte / feature handshake the secure side requires before `VIRTIO_START` (DRIVER_OK) | `START` may fail | A5: replicate the status sequence Linux's virtio core writes (needs the virtio core source, not fetched) or read it back from the descriptor |
| U8 | hwmgr reply `err` values | cannot tell denied from unsupported | A5/A6: log raw `err`; no table in source |
| U9 | GSA mailbox: polled completion via `INTSR0` instead of IRQ; response timing; whether `WAKELOCK_*` is needed without `CONFIG_GSA_PKVM` | blocking forever vs bounded wait | A4: read-only `SJTAG_GET_STATUS` (cmd 100, expects 3 args, stock logs `Success - Cmd status 0x00000010`) |
| U10 | `GSA_MB_TEST_CMD_ECHO` (34) / `GET_CHIP_ID` (3) argument layout and presence in production firmware | a no-side-effect echo test | A4: only after reviewed; otherwise use cmd 100 |
| U11 | Which parts of the carveout the AP may still access after `LOAD_AOC_FW_IMG` (body locked, IPC area?) | writing the parameter block and reading rings | A4/A6: read-only access to the IPC area only after the load; keep a fault-safe read |
| U12 | State of the AOC SysMMU (`0x1A090000`) and S2MPU (`0x1A0B0000`) before/after the load | whether AOC DMA works without AP programming | A4: fault-safe status reads of both blocks (S2MPU is not claimed by the stock AP driver); compare before and after the load |
| U13 | `kAOCChipType` value and whether the AOC firmware validates it | param block | read `0x10000000` word 0 low byte in A4; the stock driver passes `chipid[0] & 0xFF` |
| U14 | Defaults of the sysfs-set flags in the param block (`DisableMM`, `EnableUART`, `ForceSpeakerUltrasonic`, `VolteReleaseMif`) on the stock phone | param block | assume 0; read from the stock phone only if a tester starts a session (not done here) |
| U15 | Whether the firmware reads the parameter block before overwriting that area with the control block (`AOC_MAGIC`), and the exact "ready" mailbox channel | polling `magic` | A6: poll `magic` at carveout + 0x01400000 with a bounded wait (<= 5 s like stock) |
| U16 | `audio_output_control` service type (queue vs ring), slot size, IRQ index, and the live service list | command framing | A6: parse the live service table (`ipc_base + services_offset`) after ready |
| U17 | AOC default sink gain, unit of `SET_PARAMETER val`, and the output level of a first playback | **speaker safety** (46 rule 3) | not derivable; keep the level low in the source data, verify in a reviewed attended run; treat as blocking |
| U18 | How TDM0 clocks come up and whether the amp must be configured (SPI) before the AOC `BIND2`/`SOURCE` | ordering relative to amp DSP/boost setup (46 rules 1-2) | A7+ step design; research the CS35L41 TDM/ASP register setup separately |
| U19 | `CONFIG_GSA_PKVM` really off in the stock modules | extra `WAKELOCK` commands | A4: if `LOAD` returns `BAD_STATE`/`BUSY`, test the `WAKELOCK_ACQUIRE` path; otherwise confirmed |
| U20 | Touch-bus interaction: the AOC shares a pin with the touch controller (`gti tbn ap2aoc/aoc2ap` gpios, `ref dmesg:688`) | baseline health after starting the AOC (plan rule A-6) | make AOC start the last action of a boot; check touch after |
| U21 | `Trusty -> AP` IRQ/IPI delivery is not needed for polling, but `GO_ONLINE` timing and whether Trusty drops events if never kicked | connect/online | A5: issue NOPs in a bounded loop until `GO_ONLINE` (<= 2 s) |
| U22 | The SMC64 vs SMC32 arg widths on the NOP kick and descriptor calls under XNU (Linux passes `u32` args via `trusty_std_call32`) | register truncation | follow Linux exactly (32-bit values, upper bits zero) |
