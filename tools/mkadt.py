#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the sweet live FDT. Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--highmem]
"""
import argparse, hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools import adt, boards  # noqa: E402

BOARDS = boards.BOARDS

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1080 * 4
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def fake_mac(kind):
    """Locally administered unicast MAC (02:xx...) derived from the synthetic ECID; kind 0/1/2 = wifi0/bluetooth0/ethernet0.
    Placeholder until the radio's own address is read from its firmware (docs/research/26 rows 2 and 7)."""
    e = struct.pack('<Q', SWEET_ECID)
    return bytes([0x02, e[0], e[1], e[2], e[3], (e[4] + kind) & 0xff])


# xnu patch 0148 (bsd/dev/sweet_hsi2c_fg.h, boot-arg sweetbatt=1): the READ-only fuel-gauge reader over HSI2C12. The controller constants come
# from soc.json "hsi2c" / "peri" (cites: soc.json notes.hsi2c / notes.peri), the gauge and the observed bootloader handoff words from board.json
# "fg" (notes.fg). The device allowlist is pinned HERE and in the patch header: exactly one 7-bit address (0x36, the MAX77759 fuel gauge) and
# exactly these seven registers in this order; anything else in board.json is refused, so a typo can never address the charger (0x69), the PMIC
# (0x66), the TCPC (0x25), the OVP (0x35) or the direct charger (0x57).
FG_ADDR7 = 0x36
FG_REGS = (("REPSOC", 0x06), ("TEMP", 0x08), ("VCELL", 0x09), ("CURRENT", 0x0A), ("AVGCURRENT", 0x0B), ("FULLCAPREP", 0x35),
           ("DEVNAME", 0x21))	# BMS max_m5_reg.h:209,219,224,229,234,894,548
_FG_FORBIDDEN = (0x25, 0x35, 0x57, 0x66, 0x69)	# the other devices on hsi2c_12 (board.json notes.fg)
# unit formulas of the Google maxfg driver (bms 183509a): VCell uV = val * 78125 / 1000 (maxfg_common.h:192-196) -> 78125 nV per LSB; RepSOC
# 1/256 % (:210-214); Temp 1/256 degC (:198-202); current uA = s16 * 156250 / rsense, rsense in 10 uOhm (max1720x_battery.c:473-477)
_FG_VCELL_NV_PER_LSB = 78125
_FG_LSB_PER_PCT = 256
_FG_LSB_PER_C = 256
_FG_CURR_NV_UOHM = 156250
_HSI2C_REGS = ("ctl", "fifo_ctl", "trailing_ctl", "int_enable", "int_status", "err_status", "fifo_status", "tx_data", "rx_data", "conf", "auto_conf",
               "timeout", "trans_status", "addr", "usi_con")
_HSI2C_BITS = ("ctl_master", "ctl_rxchon", "ctl_txchon", "fifo_rxfifo_en", "fifo_txfifo_en", "int_tx_underrun", "int_tx_overrun", "int_rx_underrun",
               "int_rx_overrun", "int_trans_done", "int_trans_abort", "int_no_dev_ack", "int_no_dev", "int_timeout", "fifo_tx_full", "fifo_tx_empty",
               "fifo_rx_full", "fifo_rx_empty", "conf_hs_mode", "conf_10bit_addr", "conf_auto_mode", "auto_read_write", "auto_stop_after_trans",
               "auto_master_run", "timeout_en", "trans_slave_busy", "trans_master_busy", "usi_con_reset")
_HSI2C_FIELDS = ("fifo_rx_trig_shift", "fifo_tx_trig_shift", "fifo_rx_lvl_shift", "fifo_lvl_mask", "auto_len_mask", "addr_mas_shift", "addr_field_mask",
                 "trans_st_mask", "st_idle", "st_init", "st_no_ack", "st_lose", "trailing_count", "tx_fifo_depth", "pad_func")
_FG_HANDOFF = ("ctl", "fifo_ctl", "trailing_ctl", "int_enable", "conf", "auto_conf", "timeout", "addr", "usi_con")


def _n(v):
    return int(v, 0) if isinstance(v, str) else v


def sweet_batt_props(config):
    """The properties of /sweet-batt as [(name, bytes)] (without the phandle), or None for a board without an fg block.
    Raises ValueError on anything outside the allowlist or inconsistent between the tables."""
    fg = config.get('fg')
    if not fg:
        return None
    hs, peri = config['hsi2c'], config['peri']
    area = [a for a in peri['areas'] if a['name'] == fg.get('bus')]
    if fg.get('bus') != 'i2c12' or len(area) != 1 or area[0]['kind'] != 'i2c':
        raise ValueError(f"fg.bus {fg.get('bus')!r}: only 'i2c12' (USI12 / HSI2C) is supported")
    area = area[0]
    addr = _n(fg['addr7'])
    if addr in _FG_FORBIDDEN or addr != FG_ADDR7:
        raise ValueError(f"fg.addr7 {addr!r}: the allowlist is exactly {{{FG_ADDR7:#x}}}")
    regs = tuple((n, _n(o)) for n, o in fg['regs'])
    if regs != FG_REGS:
        raise ValueError(f"fg.regs: the allowlist is exactly {FG_REGS}")
    rs = fg['rsense_10uohm']
    if not isinstance(rs, int) or not 1 <= rs <= 10000 or (_FG_CURR_NV_UOHM * 1000) % rs:
        raise ValueError(f"fg.rsense_10uohm {rs!r}: want 1..10000 dividing {_FG_CURR_NV_UOHM * 1000} (an exact nA per LSB)")
    # one register map: hsi2c.regs_off must equal peri.regs.i2c (and the two FIFO data registers peri.fifo_offs.i2c)
    ro = hs['regs_off']
    if tuple(ro) != _HSI2C_REGS:
        raise ValueError(f"hsi2c.regs_off: want exactly {_HSI2C_REGS} in this order")
    peri_i2c = {n: int(o, 0) for n, o in peri['regs']['i2c']}
    fifo = [int(o, 0) for o in peri['fifo_offs']['i2c']]
    offs = {}
    for k in _HSI2C_REGS:
        off = int(ro[k], 0)
        if off & 3 or off >= 0x100:
            raise ValueError(f"hsi2c.regs_off.{k}: {off:#x} not an aligned offset below 0x100")
        if k in ("tx_data", "rx_data"):
            if off != fifo[("tx_data", "rx_data").index(k)]:
                raise ValueError(f"hsi2c.regs_off.{k}: {off:#x} differs from peri.fifo_offs.i2c")
        elif peri_i2c.get("USI_CON" if k == "usi_con" else "HSI2C_" + k.upper()) != off:
            raise ValueError(f"hsi2c.regs_off.{k}: {off:#x} differs from peri.regs.i2c")
        offs[k] = off
    if set(hs['bits']) != set(_HSI2C_BITS) or set(hs['fields']) != set(_HSI2C_FIELDS):
        raise ValueError("hsi2c.bits / hsi2c.fields: want exactly the names the patch header reads")
    for k, b in hs['bits'].items():
        if not isinstance(b, int) or not 0 <= b < 32:
            raise ValueError(f"hsi2c.bits.{k}: want a bit number 0..31")
    flds = {k: _n(v) for k, v in hs['fields'].items()}
    for k, v in flds.items():
        if not isinstance(v, int) or not 0 <= v <= 0xffffffff:
            raise ValueError(f"hsi2c.fields.{k}: want a 32-bit number")
    # the two I2C pads: one bank (soc.json <bank>_bank), CON / DAT offsets of peri.pad_off
    pins = {p[0].rsplit('_', 1)[-1]: p for p in peri['pins'] if p[3] == fg['bus']}
    if set(pins) != {"SCL", "SDA"} or pins["SCL"][1] != pins["SDA"][1]:
        raise ValueError("peri.pins: the fuel-gauge bus needs exactly an SCL and an SDA pin on one bank")
    bank = _n(config[pins["SCL"][1] + '_bank'])
    con, dat = _n(peri['pad_off']['con']), _n(peri['pad_off']['dat'])
    gate = {k: _n(area[k]) for k in ('gate_ipclk_off', 'gate_pclk_off', 'qch_off')}
    hand = fg.get('handoff', {})
    if set(hand) != set(_FG_HANDOFF):
        raise ValueError(f"fg.handoff: want exactly {_FG_HANDOFF}")
    hand = {k: _n(v) for k, v in hand.items()}
    mas = (flds['addr_field_mask'] << flds['addr_mas_shift'])
    if hand['addr'] & ~mas or hand['auto_conf'] & (1 << hs['bits']['auto_master_run']):
        raise ValueError("fg.handoff: ADDR outside the master field, or AUTO_CONF with MASTER_RUN")
    u = lambda *v: u32(*v)
    props = [('compatible', cstr('google,gs101-hsi2c-fg')),
             ('reg', u64(_n(area['base_pa']), max(offs.values()) + 4)),
             ('pad-bank', u64(bank, max(con, dat) + 4)),
             ('usi-mode-reg', u64(_n(area['mode_pa']), 4)),
             ('cmu-reg', u64(_n(area['cmu_pa']), max(gate.values()) + 4)),
             ('usi-mode-val', u(_n(area['mode_val']))), ('usi-mode-mask', u(_n(peri['mode_mask']))),
             ('cmu-gate-ipclk-off', u(gate['gate_ipclk_off'])), ('cmu-gate-pclk-off', u(gate['gate_pclk_off'])), ('cmu-qch-off', u(gate['qch_off'])),
             ('cmu-cg-val-bit', u(peri['gate_cg_val_bit'])), ('cmu-qch-enable-bit', u(peri['qch_enable_bit'])),
             ('cmu-qch-req-bit', u(peri['qch_clock_req_bit'])),
             ('pad-con-off', u(con)), ('pad-dat-off', u(dat)), ('pin-scl', u(pins["SCL"][2])), ('pin-sda', u(pins["SDA"][2]))]
    props += [('off-' + k.replace('_', '-'), u(offs[k])) for k in _HSI2C_REGS]
    props += [('bit-' + k.replace('_', '-'), u(hs['bits'][k])) for k in _HSI2C_BITS]
    props += [('fld-' + k.replace('_', '-'), u(flds[k])) for k in _HSI2C_FIELDS]
    props += [('fg-addr7', u(addr)), ('fg-regs', u(*[o for _name, o in regs])), ('fg-vcell-nv-per-lsb', u(_FG_VCELL_NV_PER_LSB)),
              ('fg-repsoc-lsb-per-pct', u(_FG_LSB_PER_PCT)), ('fg-temp-lsb-per-c', u(_FG_LSB_PER_C)),
              ('fg-curr-na-per-lsb', u(_FG_CURR_NV_UOHM * 1000 // rs))]
    props += [('handoff-' + k.replace('_', '-'), u(hand[k])) for k in _FG_HANDOFF]
    for k, _v in props:
        if len(k) > 31:
            raise ValueError(f"ADT property name {k!r} longer than 31 characters")
    return props


def sweet_batt_c(board):
    """The /sweet-batt properties of BOARD as a C table (shim/build.sh writes it for shim/hsi2c_fg_xnu_host_test.c, so the host test loads the
    patch header's config from exactly the bytes this tool puts in the ADT)."""
    props = sweet_batt_props(BOARDS[board])
    if not props:
        raise ValueError(f"{board}: no fg block, no /sweet-batt node")
    rows = []
    for k, v in props:
        rows.append('\t{ "%s", %d, { %s } },' % (k, len(v), ", ".join("0x%02x" % b for b in v)))
    return ("/* Generated by tools/mkadt.py sweet_batt_c(%r); do not edit. */\n" % board +
            "static const struct { const char *name; unsigned len; unsigned char b[64]; } sfg_adt[] = {\n" + "\n".join(rows) + "\n};\n")


def build(seed=None, qemu=False, highmem=False, board='sweet', no_ufs=False, wlan_node=False):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (the QEMU virt smoke runs were retired, T2)."""
    config = BOARDS[board]
    if board == 'sweet':
        phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
        gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    else:
        phys_base, mem_size = config['xnu_ram']
        gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (config['gicd_base'], config['gicr_base'], config['timer_freq'])
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
        # MobileGestalt WifiAddress/BluetoothAddress read these; without them each call waits 5 s for an
        # IONetworkController (docs/research/26 row 2, per ios-flame notes: installd took 5 s per bundle).
        ('mac-address-wifi0', fake_mac(0)),
        ('mac-address-bluetooth0', fake_mac(1)),
        ('mac-address-ethernet0', fake_mac(2)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SWEET SMP: all eight cores in physical-core order (0-5 Cortex-A55,
    # 6-7 Cortex-A76), one homogeneous cluster. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else len(config['cpu_mpidrs'])):
        # 'qcom,kryo470' is the IOKit personality key SweetCPU matches (KernelConfigTables), not a
        # hardware description: every board uses it so the kernel needs no per-board CPU match.
        compatible = 'qcom,kryo470'
        mpidr = core << 8 if board == 'sweet' else config['cpu_mpidrs'][core]
        # One homogeneous cluster, as sweet's working 8-core ADT (MAX_CPU_CLUSTERS is 1 in the
        # SWEET machine config; a real 4+2+2 topology would panic xnu).
        cluster = 0
        cluster_core = core
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr(compatible) + cstr('ARM,v8')),
            ('reg', u32(mpidr)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(cluster)),
            ('cluster-type', u32(cluster)),
            ('die-cluster-id', u32(cluster)),
            ('cluster-core-id', u32(cluster_core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2304000000 if core >= 6 else 1804800000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else config['vtimer_intid'])),
        ('reg', u64(gicd - SOC_BASE, config['gicd_size'], gicr - SOC_BASE, config['gicr_size'])),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])] + ([
        # Makes launchd start wifid (com.apple.wifid is limited to the 'wlan' device tree node, per ios-flame notes,
        # docs/research/26 row 1). Off by default: no IO80211 stand-in exists yet, so wifid would only fail and burn CPU.
        node('wlan', [ph(), ('device_type', cstr('wlan')), ('compatible', cstr('sweet,wlan'))])] if wlan_node else []))

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(config['fb_base'], config['fb_size'])),
        ('width', u32(config['fb_width'])),
        ('height', u32(config['fb_height'])),
        ('stride', u32(config['fb_stride'])),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc' if board == 'sweet' else 'google,gs101-ufs')),
        ('reg', u64(config['ufs_base'], config['ufs_size'])),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    # About > Model Name reads /product product-description (MG ArtworkTraits, docs about-identity report)
    if product_props:
        product_props = [(k, v) for k, v in product_props if k != 'product-description'] + [('product-description', cstr('Pixel 6a' if board == 'sweet' else boards.device_name(board)))]
    product = [node('product', [ph()] + product_props)] if product_props else []

    # bsd/dev/sweet_usb.c (patch 0082): the gs101 DWC3 and the DRAM range ABL keeps its event
    # buffer in. Only boards that name a USB core (soc.json usb_base) get it; sweet keeps its
    # hard-coded Qualcomm addresses and an unchanged tree.
    usb = node('sweet-usb', [
        ph(),
        ('compatible', cstr('google,gs101-dwusb3')),
        ('reg', u64(config['usb_base'], config['usb_size'])),
        ('dma-pool-range', u64(*config['usb_dma_range'])),
    ]) if 'usb_base' in config and not qemu else None

    # bsd/dev/sweet_restart.c (patch 0083): reset through the EL3 PMU SMC (docs/research/10). Only boards that
    # name a PMU (soc.json pmu_base) get the node; sweet keeps its tree unchanged.
    reset = node('sweet-reset', [
        ph(),
        ('compatible', cstr('google,gs101-pmu-reset')),
        ('reg', u64(config['pmu_base'], 0x10000)),
        ('mode-offset', u32(config['pmu_mode_off'])),
        ('swreset-offset', u32(config['pmu_swreset_off'])),
    ]) if 'pmu_base' in config and not qemu else None

    # bsd/dev/sweet_keys_gs101.c (patch 0087): the three buttons as GPIO DAT words. 'keys' = (sweet_hid_key index,
    # physical address of the bank's DAT register, pin) per key; board.json key_pins names bank, bank offset, pin,
    # soc.json gpio_alive_dat_off the DAT offset. Only boards that define key_pins get the node.
    keys = node('sweet-keys', [
        ph(),
        ('compatible', cstr('google,gs101-gpio-keys')),
        ('keys', u32(*[v for k, base, off, pin in config['key_pins']
                       for v in (k, base + off + config['gpio_alive_dat_off'], pin)])),
        ('active-low', u32(config['key_active_low'])),
    ]) if 'key_pins' in config and not qemu else None
    # bsd/dev/sweet_touch_gs101.c (patch 0088): the FTM5 touchscreen. Only boards whose board.json names a touch IC
    # (touch_chip_id) get the node; sweet keeps an unchanged tree. Pins are (bank register base, pin) u64 pairs; the
    # bank bases and the USI/SPI engine come from soc.json (sources cited there).
    def touch_node():
        usi = config['touch_usi']
        pin = lambda key: u64(config[config[key][0] + '_bank'], config[key][1])
        return node('sweet-touch', [
            ph(),
            ('compatible', cstr('google,gs101-ftm5-spi')),
            ('reg', u64(config['usi%d_spi_base' % usi], config['usi%d_spi_size' % usi])),
            ('usi-regs-offset', u32(config['usi_regs_offset'])),
            ('usi-mode-reg', u64(config['usi%d_mode_reg' % usi])),
            ('usi-mode-spi', u32(config['usi_mode_spi'])),
            ('usi-mode-mask', u32(config['usi_mode_mask'])),
            ('spi-pin-func', u32(config['touch_spi_pin_func'])),
            ('max-hz', u32(config['touch_max_hz'])),
            ('chip-id', u32(*config['touch_chip_id'])),
            ('max-coords', u32(*config['touch_max_coords'])),
            ('screen', u32(config['fb_width'], config['fb_height'])),
            ('pin-clk', pin('touch_pin_clk')),
            ('pin-mosi', pin('touch_pin_mosi')),
            ('pin-miso', pin('touch_pin_miso')),
            ('pin-cs', pin('touch_pin_cs')),
            ('pin-irq', pin('touch_pin_irq')),
            ('pin-reset', pin('touch_pin_reset')),
            ('pin-mux', pin('touch_pin_mux')),
        ] + cmu_props())
    # CMU_PERIC1 clock chain of USI11 (docs/research/19, soc.json cmu_peric1): offsets and bit numbers, read by the
    # driver's clock dump and its sweettouchclk sequence.
    def cmu_props():
        c = config.get('cmu_peric1')
        if not c:
            return []
        n = lambda v: int(v, 0) if isinstance(v, str) else v
        o, b = c['offsets'], c['bits']
        names = [('cmu-option', 'option'), ('cmu-mux-usi', 'mux_usi'), ('cmu-mux-bus', 'mux_bus'),
                 ('cmu-mux-bus-auto', 'mux_bus_auto'), ('cmu-mux-usi-auto', 'mux_usi_auto'), ('cmu-div', 'div'),
                 ('cmu-gate-ipclk', 'gate_ipclk'), ('cmu-gate-pclk', 'gate_pclk'), ('cmu-gate-rstn', 'gate_rstn'),
                 ('cmu-qch', 'qch')]
        bits = [('cmu-mux-sel-bit', 'mux_sel'), ('cmu-mux-busy-bit', 'mux_busy'), ('cmu-gate-manual-bit', 'gate_manual'),
                ('cmu-gate-val-bit', 'gate_val'), ('cmu-gate-auto-bit', 'gate_auto'),
                ('cmu-qch-enable-bit', 'qch_enable'), ('cmu-qch-req-bit', 'qch_req')]
        t = c.get('top')    # CMU_TOP PERIC1_IP chain (docs/research/19 section 1, read from source)
        top = [('cmutop-reg', u64(n(t['base']), n(t['size']))), ('cmutop-mux', u32(n(t['mux']))),
               ('cmutop-div', u32(n(t['div']))), ('cmutop-gate', u32(n(t['gate']))),
               ('cmutop-gate-bit', u32(n(t['gate_bit'])))] if t else []
        return ([('cmu-reg', u64(n(c['base']), n(c['size'])))] + [(p, u32(n(o[k]))) for p, k in names] +
                [(p, u32(n(b[k]))) for p, k in bits] + top)
    touch = touch_node() if 'touch_chip_id' in config and not qemu else None

    # bsd/dev/sweet_wlan.c (patches 0124, 0129): the BCM4389 behind the PCIe1 link the shim left up (--wlan-hold). All addresses come from soc.json
    # wlan (sources cited there): reg = BAR0 ATU window (mem, 64 KiB), cfg = endpoint config space, atu = DBI ATU block, elbi = PCIe1 ELBI (link state), pmu-iso = PHY isolation word.
    # Only boards whose soc.json has a wlan block with mem/cfg get the node; the driver additionally refuses addresses other than the pinned ones.
    w = config.get('wlan')
    wlan = node('sweet-wlan', [
        ph(),
        ('compatible', cstr('google,gs101-bcm4389-pcie')),
        ('reg', u64(int(w['mem'], 0), 0x10000)),
        ('cfg', u64(int(w['cfg'], 0), 0x1000)),
        ('atu', u64(int(w['atu'], 0), 0x1000)),
        ('elbi', u64(int(w['elbi'], 0), 0x2000)),
        ('pmu-iso', u64(int(w['pmu_pcie1_iso'], 0), 4)),
    ] + (
        # xnu patch 0129 (W-D3 step 2, docs/research/43): tcm = BAR2 (the chip's 4 MiB backplane window, CPU range), atu-r2-off = outbound iATU region 2
        # inside the atu block. The driver pins both, like the five above, and refuses any other value.
        [('tcm', u64(int(w['tcm'], 0), int(w['tcm_bytes'], 0))), ('atu-r2-off', u32(int(w['atu_r2_off'], 0)))]
        if all(k in w for k in ('tcm', 'tcm_bytes', 'atu_r2_off')) else [])
    ) if w and all(k in w for k in ('mem', 'cfg', 'atu', 'elbi', 'pmu_pcie1_iso')) and not qemu else None

    # bsd/dev/sweet_hsi2c.c (patch 0148): the fuel gauge on HSI2C12 (sweet_batt_props above). Only boards whose board.json has an fg block.
    bp = sweet_batt_props(config) if not qemu else None
    batt = node('sweet-batt', [ph()] + bp) if bp else None

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr('qcom,sm7150') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        # About > Serial Number / Model Number: IOPlatformExpert serial-number -> IOPlatformSerialNumber,
        # MG ModelNumber = root model-number (region-info stays absent so the row is exactly this text). Fake serial, never a real one.
        ('serial-number', cstr('PixOS-Project/pixelios')),
        ('model-number', cstr('bluejay' if board == 'sweet' else board)),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if (qemu or no_ufs) else [ufs]) + ([usb] if usb else []) + ([reset] if reset else []) + ([keys] if keys else []) + ([touch] if touch else []) + ([wlan] if wlan else []) + ([batt] if batt else []))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('output')
    parser.add_argument('--dump', action='store_true')
    parser.add_argument('--qemu', action='store_true')
    parser.add_argument('--highmem', action='store_true')
    parser.add_argument('--no-ufs', action='store_true', help='omit the /sweet-ufs node (no root device yet: the UFS driver must not probe)')
    parser.add_argument('--board', choices=BOARDS, default='sweet')
    args = parser.parse_args()
    root = build(qemu=args.qemu, highmem=args.highmem, board=args.board, no_ufs=args.no_ufs)
    blob = adt.serialize(root)
    open(args.output, 'wb').write(blob)
    if args.dump:
        adt.dump(adt.parse(blob)[0])
    print(f'{args.output}: {len(blob)} bytes', file=sys.stderr)
