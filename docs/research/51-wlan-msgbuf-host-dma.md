# 51: BCM4389 msgbuf bring-up on oriole: host DMA and the first ioctl round trip (W7 design)

Date 2026-11-15. Author: the driver session (Fable), after the Wi-Fi lane handed this note over.
Tags: **[obs]** observed on hardware, **[src]** read from source, **[guess]**.

Sources (pinned under `~/pixelios-artifacts/research-src/`):
- `openbsd-src` 9d8cbe8: `sys/dev/pci/if_bwfm_pci.c`, `if_bwfm_pci.h` (ISC). Cited as `bwfm.c:N`, `bwfm.h:N`. This is the model for the code.
- `linux-v6.18` 7d0a66e: brcmfmac `pcie.c`, `msgbuf.c` (GPL, read and cited only). Cited as `brcmf pcie.c:N`.
- `dhd-bcm4389-bluejay-a13` 865527e: `dhd_pcie.c`, `include/bcmpcie.h` (GPL, read and cited only). Cited as `dhd_pcie.c:N`.
- Stock oriole device tree dump `~/pixelios-artifacts/ref/oriole/20261114T072643Z/dt/base/pcie@14520000` (**[obs]** on stock Android).

## 1. Where we start (level 7, patch 0150)

- **[obs]** (run `o3-w5-l7`): `fw ready shared=0x408cf4 ver=7 flags=0x70050107 console=0x426628 ring_info=0x426cdc`.
- Flags decoded (W6, `bcmpcie.h:54-120`): shared version 7; DMA_INDEX set; 2BYTE_INDICES clear; HOSTRDY_DB1 set; NO_OOB_DW set;
  INBAND_DS set; SHARED_DAR clear; USE_MAILBOX clear.
- **[obs]** (W4, run `o2-w4-mmu1`): EP Command/Status `0x100002` (memory space on, bus master off); inbound iATU regions 0–3
  disabled; `s2mpu_hsi2 CTRL0=0`, `FAULT_STATUS=0`; no SysMMU.
- After level 7: firmware running, outbound ATU region 2 enabled, cfg 0x84 = 0. Nothing has enabled bus mastering, so the
  chip cannot touch host RAM yet.

## 2. Host DMA on PCIe1

| Question | Answer | Tag |
|---|---|---|
| Is a bus address the CPU physical address? | Yes. The stock node has `ranges` only (`82000000 0 60000000 0 60000000 0 00ff0000`), no `dma-ranges`, `use-sysmmu = "false"`. Stock Linux therefore hands the chip plain physical addresses | [obs] DT; conclusion [src] |
| Are 64-bit addresses normal here? | Yes. Stock boots with `disable_dma32=on swiotlb=noforce` and RAM extends above 4 GiB; every msgbuf address field has a high word (`bwfm.h:224-227`). XNU RAM is 0x8_8000_0000–0x9_8000_0000, so the high word is 8 or 9 | [obs] cmdline; [src] |
| Does the RC need an inbound iATU region for device→host? | Not on stock: all four inbound regions read disabled in our W4 probe after ABL, and the stock DT gives no `dma-ranges` for the driver to program one from. With no inbound match a DesignWare RC forwards the request address unchanged | W4 [obs]; pass-through [guess], proven only by the first completion (§7 U1) |
| Does the HSI2 S2MPU block it? | `CTRL0=0` (enable bit clear). The four GPU S2MPUs read `CTRL0=0` in the same state and did not block GPU DMA (doc 28) | [obs] register; "does not block PCIe1" is [guess] (U2) |
| Is the RC cache-coherent? | No: `use-cache-coherency = "false"`. Every host buffer needs a clean before the chip reads it and an invalidate before the host reads what the chip wrote | [obs] DT |
| Interrupts | `use-msi = "false"` in the DT, and XNU has no interrupt controller path for it. Level 8 polls | [obs] DT |
| Who enables bus mastering on stock? | The driver, with `pci_set_master` (`brcmf pcie.c:1797`; DHD `dhd_pcie_linux.c:1281`): Command bit 2 | [src] |

**Host pool.** One wired, physically contiguous, zeroed, permanent allocation (`kmem_alloc_contig(... KMA_DATA | KMA_ZERO |
KMA_PERMANENT ...)` + `kvtophys`, the pattern of patch 0104 lines 204–211), 256 KiB, 64 KiB aligned. It is never freed,
so a late write by the chip can only land in memory nothing else owns. Every address given to the firmware is
`pool_pa + offset` with `offset + len <= pool_len`; the writer checks this itself (§5 rule R1). Cache maintenance uses
`flush_dcache64(pa, len, TRUE)` (clean + invalidate, as patch 0105 line 356) before every doorbell and before every
host read of chip-written bytes.

## 3. Protocol facts for shared version 7

Shared area (`pciedev_shared_t`, offsets from `shared`; `bwfm.h:111-148`):

| Offset | Field | Use at level 8 |
|---|---|---|
| 0x00 | flags (low byte = version) | read (already at level 7) |
| 0x22 | max_rxbufpost (u16) | read, logged |
| 0x24 | rx_dataoffset | read, logged |
| 0x28 / 0x2c | h2d / d2h mailbox data address | read, logged |
| 0x30 | ring_info address | read |
| 0x34 / 0x38 / 0x3c | D2H scratch len / addr low / addr high | **write** (len 8, `bwfm.c:49, 706-717`) |
| 0x40 / 0x44 / 0x48 | D2H ring-update len / addr low / addr high | **write** (len 1024, `bwfm.c:50, 719-730`) |
| 0x54 | host_cap | **write** |
| 0x70 | host_cap2 | **write** 0 |

- **host_cap** (`bwfm.c:542-558`, bits `bcmpcie.h:440-449`): `version | HOSTCAP_H2D_ENABLE_HOSTRDY (0x400, because the
  firmware set HOSTRDY_DB1) | HOSTCAP_DS_NO_OOB_DW (0x1000)`; DAR is not offered because the firmware did not set
  SHARED_DAR. Value for this firmware: **0x00001407**. host_cap2 := 0. Only for version ≥ 6.
- **ring_info** (`bwfm.h:168-185`, 58 bytes): `ringmem`, four TCM index-array pointers (`h2d_w_idx_ptr`, `h2d_r_idx_ptr`,
  `d2h_w_idx_ptr`, `d2h_r_idx_ptr`), four 64-bit host index-buffer addresses, then `max_flowrings`,
  `max_submissionrings`, `max_completionrings` (u16; this order for version ≥ 6, `bwfm.c:601-604`).
- **Ring indices stay in chip memory.** The firmware offers DMA_INDEX, but a host that does not supply index buffers
  leaves the four host addresses untouched and uses the TCM arrays, 4 bytes per index
  (`brcmf pcie.c:1254-1270` "Using TCM indices"; DHD falls back the same way, `dhd_pcie.c:13932-13962`). So the firmware
  gets no host address for indices, and the chip writes host RAM only for ring items and posted buffers. **[src]**
- **Index width.** bwfm, brcmfmac and DHD read and write a TCM index as 16 bits (`bwfm.c:1449-1510`,
  `dhd_pcie.c:6520-6528, 6688-6696`). Our bus accessor stores 32-bit words. The slot is 4 bytes wide (idx_offset 4), so
  the store writes the 16-bit index with a zero upper half, and only when the upper half read 0 before (rule R4).
  That the upper half is unused or zero is **[guess]** (U3); the old-value check keeps the store from changing it.
- **Ring memory table** at `ringmem`, 16 bytes per ring (`bwfm.h:149-153`, `bwfm.c:1388-1415`): +4 max_item (u16),
  +6 len_items (u16), +8 base low, +0xc base high. Order: H2D control submit, H2D rxpost submit, D2H control complete,
  D2H tx complete, D2H rx complete.

| Ring | Items × size (v7) | Bytes | Source |
|---|---|---|---|
| H2D control submit (idx 0) | 64 × 40 | 2560 | `bwfm.c:671` |
| H2D rxpost submit (idx 1) | 1024 × 32 | 32768 | `bwfm.c:676` |
| D2H control complete (idx 0) | 64 × 24 | 1536 | `bwfm.c:681` |
| D2H tx complete (idx 1) | 1024 × 24 | 24576 | `bwfm.c:685-686` |
| D2H rx complete (idx 2) | 1024 × 40 | 40960 | `bwfm.c:690-691` |

  All five are set up even though level 8 uses only the two control rings, because the firmware reads the whole table.
  Both indices of each ring are written 0 first (`bwfm.c:1398-1399`).
- **Doorbells** (PCIe core, BAR0 window 0x18001000; SHARED_DAR clear): H2D mailbox 0 at **0x140** := 1 after each
  submit (`bwfm.c:1437-1446`, `bwfm.h:52`); H2D mailbox 1 at **0x144** := 1 once as "host ready"
  (`bwfm.c:2448-2459`, `bwfm.h:53`), after the rings and buffers are in place (`bwfm.c:732-734`).
  No source reads these registers; the writer stores them without an old-value read or read-back (rule R5).
- **Interrupt mask.** bwfm unmasks MAILBOXMASK for its interrupt handler (`bwfm.c:2389-2402`). Level 8 does not write
  the mask or acknowledge MAILBOXINT: it polls the D2H control-complete write index in TCM. Whether the firmware keeps
  delivering completions with the mask closed is **[guess]** (U4).
- **Messages** (little endian; `bwfm.h:193-214, 216-291`):
  - Common header (8 B): msgtype u8, ifidx u8, flags u8, rsvd u8, request_id u32.
  - `IOCTLRESP_BUF_POST` 0x0B, 32 B: header, host_buf_len u16, 3 × u16 reserved, host_buf_addr low/high, 4 × u32 reserved.
    Posted before a request so the firmware has a buffer for the answer (`bwfm.c:1307-1344`).
  - `IOCTLPTR_REQ` 0x09, 40 B: header, cmd u32, trans_id u16, input_buf_len u16, output_buf_len u16, 3 × u16 reserved,
    req_buf_addr low/high, 2 × u32 reserved (`bwfm.c:2470-2535`).
  - `IOCTLPTR_REQ_ACK` 0x0A and `IOCTL_CMPLT` 0x0C, 24 B: header, status u16, flow_ring_id u16, resp_len u16,
    trans_id u16, cmd u32, reserved u32. The answer bytes are in the posted response buffer (`bwfm.c:2551-2570`).
- **First requests.** `WLC_GET_VAR` (cmd 262) with the NUL-terminated name in the request buffer: `ver` (firmware
  version text), then `cur_etheraddr` (6 bytes; only the last 2 are printed). Bounded wait 2 s per request
  (`MSGBUF_IOCTL_RESP_TIMEOUT` 2000 ms, `bwfm.h:188`).

## 4. Level 8 sequence (run F, rows W18–W25)

`sweetwlan=8` runs level 7 unchanged and then, only if W17 passed:

| Row | Action | Stores |
|---|---|---|
| W18 | Checks (reads only): version = 7, HOSTRDY_DB1 set, SHARED_DAR clear, 2BYTE_INDICES clear; ring_info inside RAM; `ringmem` and the four index pointers inside RAM, 4-aligned; max_submissionrings ≥ 2, max_completionrings ≥ 3; host pool present, 64 KiB aligned, inside XNU RAM | none |
| W19 | host_cap := 0x1407, host_cap2 := 0 | 2 TCM words |
| W20 | Per ring: both indices := 0; ringmem entry: word +4 := `nitem \| itemsz << 16`, +8 := pool address low, +0xc := high | 5 × 5 TCM words |
| W21 | Scratch and ring-update buffers: len, addr low, addr high each | 6 TCM words |
| W22 | Bus master on: cfg 0x04 := 0x0006 (Command = memory + master; Status half written as 0, its bits are write-1-to-clear), old low half must be 0x0002. Arms restore group BM | 1 cfg word |
| W23 | Host ready: BAR0 PCIe core 0x144 := 1 | 1 doorbell |
| W24 | Post one response buffer (control submit ring, item in host RAM), write index := 1, doorbell 0x140 := 1 | 1 TCM word, 1 doorbell |
| W25 | Request `ver`: item in host RAM, write index, doorbell; poll the D2H control-complete write index (≤ 200 × 10 ms); read the items, check type/trans_id/status, print the first console line of the answer; write the read index. Then a second response buffer and `cur_etheraddr` the same way | per request: 1–2 TCM index words, 1–2 doorbells |
| end | Always, pass or stop: **bus master off** (cfg 0x04 := 0x0002, read back) before any other restore | 1 cfg word |

Pass line: `SWEET WLAN: fw version <text> mac xx:xx:xx:xx:<b4>:<b5>`.
Left after the run: firmware running, bus master off, host pool still allocated, ATU region 2 enabled, cfg 0x84 = 0.
A power cycle resets the chip.

## 5. Rules the patch must enforce (for the review)

- **R1 Pool only.** A tuple whose value is a host address takes it from writer state (`pool_pa` + a fixed offset from a
  compile-time layout table), never from the caller; the writer refuses if `offset + len > pool_len` or the pool is unset.
  Message fields that carry addresses are filled by one helper with the same check.
- **R2 Bus master.** Set by exactly one tuple, only after W21, only from old Command 0x0002. Cleared on every exit path:
  normal end, every stop, and first in the restore order, before the CA7 reset restore. The glue also clears it if the
  writer returns with the BM group still armed.
- **R3 Bounded.** Every poll loop has a fixed count; no wait depends on the firmware.
- **R4 TCM stores.** Only at addresses derived from the ring_info read in W18 and bounds-checked against RAM; index
  stores only with a value < the ring's depth and an old upper half of 0.
- **R5 Doorbells.** Only 0x140 and 0x144 in the PCIe core window, only value 1, only after W22, write-only (no read).
- **R6 Firmware-written data is untrusted.** Completion items and lengths are bounds-checked (index < depth, resp_len ≤
  buffer length, msgtype in the expected set) before use; printed bytes are filtered to printable ASCII.
- **R7 No S2MPU/SysMMU/iATU-inbound write**, no interrupt mask write, no regulatory or NVRAM change.
- **R8 Levels ≤ 7 unchanged** (the level gate `run ≤ level − 2` puts run F at level 8).
- **R9 Stop on corruption signs.** A completion whose bytes lie outside the pool layout, an index ≥ depth, or a pool guard
  word (pattern between the layout regions) that changed: stop, bus master off, print `DMA suspect`. Plan stop H10 applies.

## 6. Pool layout (256 KiB, offsets from `pool_pa`)

| Offset | Size | Use |
|---|---|---|
| 0x00000 | 0x0A00 | H2D control submit ring |
| 0x01000 | 0x0600 | D2H control complete ring |
| 0x02000 | 0x0008 | D2H scratch |
| 0x02400 | 0x0400 | D2H ring-update buffer |
| 0x03000 | 0x0800 | ioctl request buffer |
| 0x04000 | 0x2000 | ioctl response buffer (8192, `MSGBUF_MAX_CTL_PKT_SIZE` `bwfm.h:191`) |
| 0x08000 | 0x8000 | H2D rxpost submit ring |
| 0x10000 | 0x6000 | D2H tx complete ring |
| 0x18000 | 0xA000 | D2H rx complete ring |
| gaps | | guard pattern 0x5A, checked at the end of the run (R9) |

## 7. Unknowns and how each is settled

| # | Unknown | Settled by |
|---|---|---|
| U1 | Device→host requests pass the RC untranslated with no inbound iATU region | The first `IOCTLPTR_REQ_ACK` appearing in the pool's control-complete ring. If the index advances but the ring bytes stay zero and guards are intact: "DMA blocked", Wi-Fi ladder ends (no secure-world call is tried) |
| U2 | HSI2 S2MPU with CTRL0 = 0 does not block | Same run; W4 probe line re-read after the run (`FAULT_STATUS`) |
| U3 | A 32-bit store of a 16-bit TCM index is accepted | Read-back, and the firmware consuming the item (read index advances) |
| U4 | Completions arrive with MAILBOXMASK untouched | Same run. If the firmware's read index advances (it took the request) but no completion comes in 2 s, the next iteration adds the mask write of `bwfm.c:2389-2402` as its own reviewed tuple |
| U5 | PCIe core rev 72 uses the classic 0x140/0x144 doorbells when DAR is not negotiated | bwfm rings by the DAR flag only (`bwfm.c:1437-1446`); confirmed if the read index advances |
| U6 | Whether scan needs a CLM download first | W8: DHD decides from the `country`/`clmver` iovars (`dhd_common.c:8104-8250`); query `clmver` at level 8 once the round trip works |
| U7 | host_cap of bwfm (3 bits) is enough for this DHD-oriented firmware | The firmware console after W19–W23 (read like level 7); a trap line stops the run |

## 8. Host test and audit plan

- `shim/wlan_wr_host_test.c` gets a fake chip that models the shared area, ring_info, TCM indices and a firmware that,
  on doorbell 0x140, consumes control-submit items and writes ACK + completion into the host pool.
- Tests first (red): run F happy path; each R1–R9 rule as a negative case (address outside the pool, bus master left on
  at each stop point, index ≥ depth from the fake firmware, oversized resp_len, wrong trans_id, timeout, guard damage,
  level 7 writes nothing new).
- `tools/test_wlan_wr_mutants.py`: add 0151 to both apply chains; one mutant per new guard.
