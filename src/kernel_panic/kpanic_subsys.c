/*
 * kpanic_subsys.c - fault-domain classifiers with live state.
 *
 * Each kpanic_*() entry in kpanic_core.c lands here to build the AUX
 * line: one dense, greppable sentence with the live numbers for that
 * subsystem (heap bytes for OOM, mode bytes for VRAM, vector decode
 * for CPU). No canned text: every value is a parameter or a fresh
 * probe. The cause tables below tell the developer what to check
 * next for the most common codes in each domain.
*/

#include "kpanic.h"

/* Domain / severity names. */

const char *kpanic_domain_name(kpanic_domain_t d) {
    switch (d) {
    case KP_DOMAIN_CPU: return "CPU-FAULT";
    case KP_DOMAIN_OOM: return "OUT-OF-MEMORY";
    case KP_DOMAIN_VRAM: return "VRAM-FAULT";
    case KP_DOMAIN_GUI: return "GUI-CRASH";
    case KP_DOMAIN_VFS: return "VFS-FAULT";
    case KP_DOMAIN_NET: return "NET-FAULT";
    case KP_DOMAIN_SCHED: return "SCHED-FAULT";
    case KP_DOMAIN_DRIVER: return "DRIVER-FAULT";
    case KP_DOMAIN_HEAP: return "HEAP-CORRUPT";
    case KP_DOMAIN_ASSERT: return "ASSERT";
    default: return "UNKNOWN";
    }
}

const char *kpanic_severity_name(kpanic_severity_t s) {
    switch (s) {
    case KP_SEV_OOPS: return "OOPS";
    case KP_SEV_FATAL: return "FATAL";
    case KP_SEV_CORRUPT: return "CORRUPT";
    case KP_SEV_HANG: return "HANG";
    default: return "?";
    }
}

const char *kpanic_action_line(kpanic_severity_t s) {
    switch (s) {
    case KP_SEV_OOPS:
        return "System halted safely. Reboot to continue; dump saved to serial log.";
    case KP_SEV_FATAL:
        return "Subsystem is dead. Reboot required. Save the Oops # for the bug report.";
    case KP_SEV_CORRUPT:
        return "Memory corruption: NOT syncing disks. Reboot now, fsck on next boot.";
    case KP_SEV_HANG:
        return "Watchdog: CPU alive, nothing schedules. Reboot required.";
    default:
        return "Halted. Reboot required.";
    }
}

/* Cause tables: code -> what it means -> what to check. These are     */
/* documentation, not probes; the live numbers come from the caller.   */

typedef struct kpanic_cause {
    uint32_t code;
    const char *tag;
    const char *fix;
} kpanic_cause_t;

/* GUI / window manager. Codes match the KP_GUI call sites. */
static const kpanic_cause_t k_gui_causes[] = {
    { 0x475501u, "null-window", "caller passed NULL wm_window_t; check create path" },
    { 0x475502u, "bad-magic", "window magic mismatch; use-after-free in wm_close" },
    { 0x475503u, "fb-null", "backbuffer NULL; gpu_setup_framebuffer failed first" },
    { 0x475504u, "clip-oob", "rect outside screen; clamp against gpu_get_width/height" },
    { 0x475505u, "blit-overrun", "copy length exceeds stride; checked mul on w*bpp" },
    { 0x475506u, "font-oob", "glyph index >127; validate char before gpu_draw_char" },
    { 0x475507u, "queue-full", "event queue saturated; drain or grow with backpressure" },
    { 0x475508u, "compositor-dead", "compositor task exited; check sched task list" },
};

/* VFS / block. */
static const kpanic_cause_t k_vfs_causes[] = {
    { 0x564601u, "lba-oob", "LBA past disk end; validate against partition size" },
    { 0x564602u, "fat-bad-cluster", "cluster chain loops; run fsck, check FAT mirrors" },
    { 0x564603u, "iso-bad-dir", "directory record length 0; re-read with bounds check" },
    { 0x564604u, "ata-timeout", "drive did not set DRQ; check cable, master/slave" },
    { 0x564605u, "atapi-sense", "CD sense key set; check disc presence" },
    { 0x564606u, "write-ro", "write to read-only mount; remount rw or redirect" },
    { 0x564607u, "name-too-long", "path exceeds 255; reject at syscall boundary" },
    { 0x564608u, "fd-exhaust", "fd table full; close leaks, raise FD_MAX with audit" },
};

/* Network. */
static const kpanic_cause_t k_net_causes[] = {
    { 0x4E455401u, "arp-spoof", "unsolicited ARP reply; only accept pending XIDs" },
    { 0x4E455402u, "dhcp-bad-xid", "XID mismatch; transaction table corrupt?" },
    { 0x4E455403u, "tcp-bad-seq", "seq outside window; peer or middlebox rewriting?" },
    { 0x4E455404u, "tls-cert", "cert verify failed; check time, hostname, chain" },
    { 0x4E455405u, "tls-version", "peer needs TLS1.3-only; we speak 1.2" },
    { 0x4E455406u, "dns-trunc", "UDP reply truncated; retry over TCP" },
    { 0x4E455407u, "ring-full", "RX ring overrun; NAPI budget or IRQ coalescing" },
    { 0x4E455408u, "phy-down", "link down; cable, switch port, PCI power" },
};

/* Scheduler. */
static const kpanic_cause_t k_sched_causes[] = {
    { 0x53434801u, "stack-overflow", "guard page hit; grow stack or cut recursion" },
    { 0x53434802u, "bad-tss", "TSS RSP0 invalid; check GDT 0x28 setup" },
    { 0x53434803u, "deadlock", "runqueue locked >5s; lock order violated?" },
    { 0x53434804u, "bad-pid", "sched_get on freed pid; generation bits stale?" },
    { 0x53434805u, "fpu-leak", "fxsave/fxrstor mismatch on context switch" },
    { 0x53434806u, "irq-in-crit", "sleep inside cli region; shorten critical section" },
};

/* Drivers / PCI. */
static const kpanic_cause_t k_drv_causes[] = {
    { 0x44525601u, "pci-bar-oob", "BAR outside 32-bit window; check BIOS assignment" },
    { 0x44525602u, "dma-overrun", "device wrote past descriptor; IOMMUavit or length" },
    { 0x44525603u, "irq-storm", ">10k IRQs/s; mask line, check level/edge config" },
    { 0x44525604u, "svga-fifo-full", "VMware FIFO full; wait for FENCE before submit" },
    { 0x44525605u, "ne2k-reset", "NE2000 reset loop; check IO base 0x300 probe" },
    { 0x44525606u, "rtl-tx-stuck", "RTL8139 TX dirty stuck; ring + OWN bits audit" },
};

static const kpanic_cause_t *cause_lookup(const kpanic_cause_t *tab, uint32_t n,
                                          uint32_t code) {
    for (uint32_t i = 0; i < n; i++)
        if (tab[i].code == code) return &tab[i];
    return NULL;
}

/* Small bounded append helpers for building the AUX line.             */

static void aux_str(char *out, uint32_t len, uint32_t *p, const char *s) {
    if (!s) s = "?";
    while (*s && *p + 1 < len) out[(*p)++] = *s++;
}

static void aux_u32(char *out, uint32_t len, uint32_t *p, uint32_t v) {
    char nb[16];
    kpanic_u32_dec(v, nb);
    aux_str(out, len, p, nb);
}

static void aux_hex(char *out, uint32_t len, uint32_t *p, uint32_t v) {
    char hx[16];
    kpanic_u32_hex(v, hx);
    aux_str(out, len, p, hx);
}

static void aux_bytes(char *out, uint32_t len, uint32_t *p, uint32_t v) {
    char b[24];
    kpanic_format_bytes(v, b, sizeof(b));
    aux_str(out, len, p, b);
}

/* The AUX line: one sentence per domain with live numbers.            */

void kpanic_subsys_line(kpanic_domain_t domain, const char *message,
                        uint32_t code, const kpanic_mem_state_t *mem,
                        const kpanic_cpu_state_t *cpu,
                        char *out, uint32_t out_len) {
    uint32_t p = 0;
    const kpanic_cause_t *c = NULL;
    if (!out || !out_len) return;
    out[0] = '\0';

    switch (domain) {
    case KP_DOMAIN_CPU:
        aux_str(out, out_len, &p, kpanic_exception_name(code));
        aux_str(out, out_len, &p, " class=");
        aux_str(out, out_len, &p, kpanic_exception_class(code));
        if (cpu) {
            aux_str(out, out_len, &p, " CR2=");
            aux_hex(out, out_len, &p, cpu->cr2);
        }
        break;

    case KP_DOMAIN_OOM:
        aux_str(out, out_len, &p, "alloc of ");
        aux_bytes(out, out_len, &p, code);
        aux_str(out, out_len, &p, " failed; ");
        if (mem && mem->stats_ok) {
            aux_str(out, out_len, &p, "free ");
            aux_bytes(out, out_len, &p, mem->heap_free);
            aux_str(out, out_len, &p, " used ");
            aux_bytes(out, out_len, &p, mem->heap_used);
            aux_str(out, out_len, &p, " blocks ");
            aux_u32(out, out_len, &p, mem->heap_blocks);
            if (!mem->heap_valid) aux_str(out, out_len, &p, " HEAP-CORRUPT?");
        } else {
            aux_str(out, out_len, &p, "heap stats unavailable");
        }
        break;

    case KP_DOMAIN_VRAM:
        aux_str(out, out_len, &p, "fb alloc ");
        aux_bytes(out, out_len, &p, code);
        aux_str(out, out_len, &p, " failed; ");
        if (mem) {
            aux_str(out, out_len, &p, "screen ");
            aux_u32(out, out_len, &p, (uint32_t)(mem->gpu_w > 0 ? mem->gpu_w : 0));
            aux_str(out, out_len, &p, "x");
            aux_u32(out, out_len, &p, (uint32_t)(mem->gpu_h > 0 ? mem->gpu_h : 0));
            aux_str(out, out_len, &p, mem->gpu_vesa ? " vesa=up" : " vesa=down");
            if (mem->vbe_info_ok) {
                aux_str(out, out_len, &p, " bootfb=");
                aux_hex(out, out_len, &p, mem->vbe_fb);
            }
            aux_str(out, out_len, &p, " heapfree ");
            aux_bytes(out, out_len, &p, mem->heap_free);
        }
        break;

    case KP_DOMAIN_GUI:
        c = cause_lookup(k_gui_causes,
                         sizeof(k_gui_causes) / sizeof(k_gui_causes[0]), code);
        aux_str(out, out_len, &p, "wm fault: ");
        aux_str(out, out_len, &p, message && message[0] ? message : "unknown");
        if (c) {
            aux_str(out, out_len, &p, " [");
            aux_str(out, out_len, &p, c->tag);
            aux_str(out, out_len, &p, ": ");
            aux_str(out, out_len, &p, c->fix);
            aux_str(out, out_len, &p, "]");
        }
        if (mem) {
            aux_str(out, out_len, &p, " scr=");
            aux_u32(out, out_len, &p, (uint32_t)(mem->gpu_w > 0 ? mem->gpu_w : 0));
            aux_str(out, out_len, &p, "x");
            aux_u32(out, out_len, &p, (uint32_t)(mem->gpu_h > 0 ? mem->gpu_h : 0));
        }
        break;

    case KP_DOMAIN_VFS:
        c = cause_lookup(k_vfs_causes,
                         sizeof(k_vfs_causes) / sizeof(k_vfs_causes[0]), code);
        aux_str(out, out_len, &p, "fs fault: ");
        aux_str(out, out_len, &p, message && message[0] ? message : "unknown");
        if (c) {
            aux_str(out, out_len, &p, " [");
            aux_str(out, out_len, &p, c->tag);
            aux_str(out, out_len, &p, ": ");
            aux_str(out, out_len, &p, c->fix);
            aux_str(out, out_len, &p, "]");
        }
        aux_str(out, out_len, &p, " lba/code=");
        aux_hex(out, out_len, &p, code);
        break;

    case KP_DOMAIN_NET:
        c = cause_lookup(k_net_causes,
                         sizeof(k_net_causes) / sizeof(k_net_causes[0]), code);
        aux_str(out, out_len, &p, "net fault: ");
        aux_str(out, out_len, &p, message && message[0] ? message : "unknown");
        if (c) {
            aux_str(out, out_len, &p, " [");
            aux_str(out, out_len, &p, c->tag);
            aux_str(out, out_len, &p, ": ");
            aux_str(out, out_len, &p, c->fix);
            aux_str(out, out_len, &p, "]");
        }
        break;

    case KP_DOMAIN_SCHED:
        c = cause_lookup(k_sched_causes,
                         sizeof(k_sched_causes) / sizeof(k_sched_causes[0]), code);
        aux_str(out, out_len, &p, "sched fault: ");
        aux_str(out, out_len, &p, message && message[0] ? message : "unknown");
        if (c) {
            aux_str(out, out_len, &p, " [");
            aux_str(out, out_len, &p, c->tag);
            aux_str(out, out_len, &p, ": ");
            aux_str(out, out_len, &p, c->fix);
            aux_str(out, out_len, &p, "]");
        }
        if (cpu) {
            aux_str(out, out_len, &p, " ticks=");
            aux_u32(out, out_len, &p, cpu->ticks);
        }
        break;

    case KP_DOMAIN_DRIVER:
        c = cause_lookup(k_drv_causes,
                         sizeof(k_drv_causes) / sizeof(k_drv_causes[0]), code);
        aux_str(out, out_len, &p, "driver fault: ");
        aux_str(out, out_len, &p, message && message[0] ? message : "unknown");
        if (c) {
            aux_str(out, out_len, &p, " [");
            aux_str(out, out_len, &p, c->tag);
            aux_str(out, out_len, &p, ": ");
            aux_str(out, out_len, &p, c->fix);
            aux_str(out, out_len, &p, "]");
        }
        aux_str(out, out_len, &p, " pci/code=");
        aux_hex(out, out_len, &p, code);
        break;

    case KP_DOMAIN_HEAP:
        aux_str(out, out_len, &p, "heap corrupt at ");
        aux_str(out, out_len, &p, message && message[0] ? message : "?");
        aux_str(out, out_len, &p, " DO NOT TRUST free/used above; reboot without sync");
        break;

    case KP_DOMAIN_ASSERT:
    default:
        aux_str(out, out_len, &p, message && message[0] ? message : "explicit panic");
        aux_str(out, out_len, &p, " code=");
        aux_hex(out, out_len, &p, code);
        break;
    }
    if (p < out_len) out[p] = '\0';
    else out[out_len - 1] = '\0';
}

/* PCI class table (PCI SIG, rev 3.0). The DRIVER-FAULT screen prints */
/* the class name for a (class,subclass) pair read live from config   */
/* space, so a ne2000 vs nvme mixup is visible without lspci.         */

typedef struct kpanic_pci_class {
    uint8_t cls;
    uint8_t sub;
    const char *name;
} kpanic_pci_class_t;

static const kpanic_pci_class_t k_pci_classes[] = {
    { 0x00, 0x00, "pre-2.0 non-VGA device" },
    { 0x00, 0x01, "VGA-compatible device" },
    { 0x01, 0x00, "SCSI storage" },
    { 0x01, 0x01, "IDE storage" },
    { 0x01, 0x02, "floppy controller" },
    { 0x01, 0x03, "IPI bus controller" },
    { 0x01, 0x04, "RAID controller" },
    { 0x01, 0x05, "ATA controller" },
    { 0x01, 0x06, "SATA (AHCI) controller" },
    { 0x01, 0x07, "SAS controller" },
    { 0x01, 0x08, "NVM (NVMe) controller" },
    { 0x02, 0x00, "ethernet controller" },
    { 0x02, 0x01, "token-ring controller" },
    { 0x02, 0x02, "FDDI controller" },
    { 0x02, 0x03, "ATM controller" },
    { 0x02, 0x04, "ISDN controller" },
    { 0x02, 0x05, "WorldFip controller" },
    { 0x02, 0x06, "PICMG controller" },
    { 0x02, 0x07, "InfiniBand controller" },
    { 0x02, 0x08, "fabric controller" },
    { 0x03, 0x00, "VGA display controller" },
    { 0x03, 0x01, "XGA display controller" },
    { 0x03, 0x02, "3D controller" },
    { 0x04, 0x00, "multimedia video" },
    { 0x04, 0x01, "multimedia audio" },
    { 0x04, 0x02, "telephony" },
    { 0x04, 0x03, "HD-audio controller" },
    { 0x05, 0x00, "RAM memory controller" },
    { 0x05, 0x01, "flash memory controller" },
    { 0x06, 0x00, "host bridge" },
    { 0x06, 0x01, "ISA bridge" },
    { 0x06, 0x02, "EISA bridge" },
    { 0x06, 0x03, "MCA bridge" },
    { 0x06, 0x04, "PCI-PCI bridge" },
    { 0x06, 0x05, "PCMCIA bridge" },
    { 0x06, 0x06, "NuBus bridge" },
    { 0x06, 0x07, "CardBus bridge" },
    { 0x06, 0x08, "RACEway bridge" },
    { 0x06, 0x09, "semi-transparent PCI-PCI bridge" },
    { 0x06, 0x0A, "InfiniBand-PCI bridge" },
    { 0x07, 0x00, "serial controller" },
    { 0x07, 0x01, "parallel controller" },
    { 0x07, 0x02, "multiport serial" },
    { 0x07, 0x03, "modem" },
    { 0x07, 0x04, "GPIB controller" },
    { 0x07, 0x05, "smart-card controller" },
    { 0x08, 0x00, "PIC (8259)" },
    { 0x08, 0x01, "DMA controller" },
    { 0x08, 0x02, "timer" },
    { 0x08, 0x03, "RTC" },
    { 0x08, 0x04, "PCI hotplug" },
    { 0x08, 0x05, "SD host controller" },
    { 0x08, 0x06, "IOMMU" },
    { 0x09, 0x00, "keyboard controller" },
    { 0x09, 0x01, "digitizer pen" },
    { 0x09, 0x02, "mouse controller" },
    { 0x09, 0x03, "scanner controller" },
    { 0x09, 0x04, "gameport controller" },
    { 0x0A, 0x00, "generic docking" },
    { 0x0B, 0x00, "386 processor" },
    { 0x0B, 0x01, "486 processor" },
    { 0x0B, 0x02, "Pentium processor" },
    { 0x0B, 0x10, "Alpha processor" },
    { 0x0B, 0x20, "PowerPC processor" },
    { 0x0B, 0x30, "MIPS processor" },
    { 0x0C, 0x00, "FireWire (1394)" },
    { 0x0C, 0x01, "ACCESS bus" },
    { 0x0C, 0x02, "SSA" },
    { 0x0C, 0x03, "USB controller" },
    { 0x0C, 0x04, "Fibre Channel" },
    { 0x0C, 0x05, "SMBus" },
    { 0x0C, 0x06, "InfiniBand bridge" },
    { 0x0C, 0x07, "IPMI interface" },
    { 0x0C, 0x08, "SERCOS interface" },
    { 0x0C, 0x09, "CANbus" },
    { 0x0D, 0x00, "IRDA controller" },
    { 0x0D, 0x01, "IR consumer" },
    { 0x0D, 0x10, "RF controller" },
    { 0x0D, 0x11, "bluetooth" },
    { 0x0D, 0x12, "broadband" },
    { 0x0D, 0x20, "802.1a ethernet" },
    { 0x0D, 0x21, "802.1b ethernet" },
    { 0x0E, 0x00, "satcom video" },
    { 0x0E, 0x01, "satcom audio" },
    { 0x0E, 0x02, "satcom voice" },
    { 0x0E, 0x03, "satcom data" },
    { 0x0F, 0x01, "VGA-crypto (obsolete)" },
    { 0x10, 0x00, "network/encrypt" },
    { 0x10, 0x10, "entertainment/encrypt" },
    { 0x11, 0x00, "DPIO signal processing" },
    { 0x11, 0x01, "perf counters" },
    { 0x12, 0x00, "processing accelerator" },
    { 0x13, 0x00, "non-essential instrumentation" },
};

const char *kpanic_pci_class_name(uint8_t cls, uint8_t sub) {
    for (uint32_t i = 0; i < sizeof(k_pci_classes) / sizeof(k_pci_classes[0]); i++)
        if (k_pci_classes[i].cls == cls && k_pci_classes[i].sub == sub)
            return k_pci_classes[i].name;
    /* Fall back to class-only match so unknown subclasses still show */
    /* something better than "unknown". */
    switch (cls) {
    case 0x01: return "mass-storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x06: return "bridge";
    case 0x0C: return "serial-bus controller";
    case 0xFF: return "unassigned/vendor-specific";
    default: return "unknown PCI class";
    }
}

/* ATA status + error decoders (real taskfile bits).                  */

void kpanic_decode_ata_status(uint8_t st, char *out, uint32_t out_len) {
    uint32_t p = 0;
    if (!out || !out_len) return;
    out[0] = '\0';
    if (st & 0x80u) { const char *t = "BSY "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (st & 0x40u) { const char *t = "DRDY "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (st & 0x20u) { const char *t = "DWF "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (st & 0x10u) { const char *t = "DSC "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (st & 0x08u) { const char *t = "DRQ "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (st & 0x04u) { const char *t = "CORR "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (st & 0x02u) { const char *t = "IDX "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (st & 0x01u) { const char *t = "ERR "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (p == 0) {
        const char *t = "(status 0: no drive or floating bus)";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    } else if (p > 0) {
        out[p - 1] = '\0';
        return;
    }
    out[p] = '\0';
}

void kpanic_decode_ata_error(uint8_t er, char *out, uint32_t out_len) {
    uint32_t p = 0;
    if (!out || !out_len) return;
    out[0] = '\0';
    if (er & 0x80u) { const char *t = "BBK "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (er & 0x40u) { const char *t = "UNC "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (er & 0x20u) { const char *t = "MC "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (er & 0x10u) { const char *t = "IDNF "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (er & 0x08u) { const char *t = "MCR "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (er & 0x04u) { const char *t = "ABRT "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (er & 0x02u) { const char *t = "TK0NF "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (er & 0x01u) { const char *t = "AMNF "; while (*t && p + 1 < out_len) out[p++] = *t++; }
    if (p == 0) {
        const char *t = "(no error bits: check STATUS instead)";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    } else if (p > 0) {
        out[p - 1] = '\0';
        return;
    }
    out[p] = '\0';
}

/* TLS alert codes (RFC 5246 section 7.2). The NET-FAULT screen shows */
/* the peer alert as "unexpected_message: check ..." instead of 0x0A. */

typedef struct kpanic_tls_alert {
    uint8_t code;
    const char *name;
    const char *fix;
} kpanic_tls_alert_t;

static const kpanic_tls_alert_t k_tls_alerts[] = {
    { 0, "close_notify", "clean shutdown; not an error" },
    { 10, "unexpected_message", "state machine desync; check record layer" },
    { 20, "bad_record_mac", "key mismatch or tampering; check cipher state" },
    { 22, "record_overflow", "record >16KB; peer framing broken" },
    { 40, "handshake_failure", "no shared cipher; check suite list" },
    { 41, "no_certificate", "client cert required but absent" },
    { 42, "bad_certificate", "cert corrupt; check DER parse" },
    { 43, "unsupported_certificate", "cert type not supported" },
    { 44, "certificate_revoked", "revoked; check OCSP/CRL path" },
    { 45, "certificate_expired", "check wall clock, not just chain" },
    { 46, "certificate_unknown", "untrusted issuer; check trust store" },
    { 47, "illegal_parameter", "handshake field out of range" },
    { 48, "unknown_ca", "CA not in trust store" },
    { 49, "access_denied", "server refused; check SNI/hostname" },
    { 50, "decode_error", "message failed to decode; framing bug" },
    { 51, "decrypt_error", "decryption failed; check keys/IV" },
    { 70, "protocol_version", "peer wants newer TLS than 1.2" },
    { 71, "insufficient_security", "suite too weak; raise minimum" },
    { 72, "internal_error", "peer-side bug" },
    { 73, "user_canceled", "peer aborted" },
    { 74, "no_renegotiation", "peer refused renegotiation" },
    { 80, "unsupported_extension", "extension not implemented here" },
    { 86, "inappropriate_fallback", "downgrade attack blocked; good" },
    { 87, "missing_extension", "required extension absent" },
    { 109, "missing_extension(TLS1.3)", "check supported_versions" },
};

const char *kpanic_tls_alert_name(uint8_t code) {
    for (uint32_t i = 0; i < sizeof(k_tls_alerts) / sizeof(k_tls_alerts[0]); i++)
        if (k_tls_alerts[i].code == code) return k_tls_alerts[i].name;
    return "unknown-alert";
}

const char *kpanic_tls_alert_fix(uint8_t code) {
    for (uint32_t i = 0; i < sizeof(k_tls_alerts) / sizeof(k_tls_alerts[0]); i++)
        if (k_tls_alerts[i].code == code) return k_tls_alerts[i].fix;
    return "capture the handshake bytes";
}

/* errno table (POSIX.1-2017 values as used by the Aurion VFS/syscall */
/* layer). The VFS-FAULT screen prints the name for a negative-errno  */
/* style code so "-2" reads "ENOENT" with the fix attached.           */

typedef struct kpanic_errno {
    int code;
    const char *name;
    const char *fix;
} kpanic_errno_t;

static const kpanic_errno_t k_errnos[] = {
    { 1, "EPERM", "operation not permitted; check capability/mount flags" },
    { 2, "ENOENT", "no such file; check path spelling + cwd" },
    { 3, "ESRCH", "no such process; pid reused or exited" },
    { 4, "EINTR", "interrupted syscall; retry the call" },
    { 5, "EIO", "I/O error; check ATA status bits above" },
    { 6, "ENXIO", "no such device; driver not bound?" },
    { 7, "E2BIG", "arg list too long; shorten exec args" },
    { 8, "ENOEXEC", "exec format error; check ELF magic" },
    { 9, "EBADF", "bad fd; double-close or never opened" },
    { 10, "ECHILD", "no child processes; wait() with none" },
    { 11, "EAGAIN", "try again; nonblocking empty or ring full" },
    { 12, "ENOMEM", "out of memory; see OOM triage" },
    { 13, "EACCES", "permission denied; check mode bits" },
    { 14, "EFAULT", "bad address; user pointer failed validation" },
    { 15, "ENOTBLK", "block device required" },
    { 16, "EBUSY", "device busy; unmount or kill holder first" },
    { 17, "EEXIST", "file exists; O_EXCL path" },
    { 18, "EXDEV", "cross-device link; copy instead" },
    { 19, "ENODEV", "no such device; check major/minor" },
    { 20, "ENOTDIR", "not a directory; path component wrong type" },
    { 21, "EISDIR", "is a directory; write/open mode wrong" },
    { 22, "EINVAL", "invalid argument; check syscall contract" },
    { 23, "ENFILE", "file table overflow; raise limit with audit" },
    { 24, "EMFILE", "too many open files; close leaks" },
    { 25, "ENOTTY", "not a tty; ioctl on wrong fd type" },
    { 26, "ETXTBSY", "text file busy; executing while writing" },
    { 27, "EFBIG", "file too large; check FS limits" },
    { 28, "ENOSPC", "no space left; free blocks or inodes" },
    { 29, "ESPIPE", "illegal seek; pipe/socket/fifo" },
    { 30, "EROFS", "read-only filesystem; remount rw" },
    { 31, "EMLINK", "too many links" },
    { 32, "EPIPE", "broken pipe; reader went away" },
    { 33, "EDOM", "math arg out of domain" },
    { 34, "ERANGE", "math result not representable" },
    { 35, "EDEADLK", "deadlock avoided; fix lock order" },
    { 36, "ENAMETOOLONG", "name too long; reject at boundary" },
    { 37, "ENOLCK", "no record locks available" },
    { 38, "ENOSYS", "function not implemented; check syscall table" },
    { 39, "ENOTEMPTY", "directory not empty" },
    { 40, "ELOOP", "symlink loop; max 40 hops" },
    { 42, "ENOMSG", "no message of desired type" },
    { 43, "EIDRM", "identifier removed" },
    { 44, "ECHRNG", "channel number out of range" },
    { 45, "EL2NSYNC", "level 2 not synchronized" },
    { 46, "EL3HLT", "level 3 halted" },
    { 47, "EL3RST", "level 3 reset" },
    { 48, "ELNRNG", "link number out of range" },
    { 49, "EUNATCH", "protocol driver not attached" },
    { 50, "ENOCSI", "no CSI structure available" },
    { 51, "EL2HLT", "level 2 halted" },
    { 52, "EBADE", "invalid exchange" },
    { 53, "EBADR", "invalid request descriptor" },
    { 54, "EXFULL", "exchange full" },
    { 55, "ENOANO", "no anode" },
    { 56, "EBADRQC", "invalid request code" },
    { 57, "EBADSLT", "invalid slot" },
    { 59, "EBFONT", "bad font file format" },
    { 60, "ENOSTR", "not a stream" },
    { 61, "ENODATA", "no data available" },
    { 62, "ETIME", "timer expired" },
    { 63, "ENOSR", "out of streams resources" },
    { 64, "ENONET", "machine not on network" },
    { 65, "ENOPKG", "package not installed" },
    { 66, "EREMOTE", "object is remote" },
    { 67, "ENOLINK", "link severed" },
    { 68, "EADV", "advertise error" },
    { 69, "ESRMNT", "srmount error" },
    { 70, "ECOMM", "communication error on send" },
    { 71, "EPROTO", "protocol error; check wire bytes" },
    { 72, "EMULTIHOP", "multihop attempted" },
    { 73, "EDOTDOT", "RFS specific error" },
    { 74, "EBADMSG", "bad message; check framing" },
    { 75, "EOVERFLOW", "value too large for datatype" },
    { 76, "ENOTUNIQ", "name not unique on network" },
    { 77, "EBADFD", "fd in wrong state" },
    { 78, "EREMCHG", "remote address changed" },
    { 79, "ELIBACC", "cannot access shared lib" },
    { 80, "ELIBBAD", "corrupt shared lib" },
    { 81, "ELIBSCN", "lib section corrupt" },
    { 82, "ELIBMAX", "too many shared libs" },
    { 83, "ELIBEXEC", "cannot exec shared lib" },
    { 84, "EILSEQ", "illegal byte sequence; check UTF-8" },
    { 85, "ERESTART", "interrupted syscall should restart" },
    { 86, "ESTRPIPE", "streams pipe error" },
    { 87, "EUSERS", "too many users" },
    { 88, "ENOTSOCK", "not a socket" },
    { 89, "EDESTADDRREQ", "destination address required" },
    { 90, "EMSGSIZE", "message too long; fragment first" },
    { 91, "EPROTOTYPE", "wrong protocol type for socket" },
    { 92, "ENOPROTOOPT", "protocol not available" },
    { 93, "EPROTONOSUPPORT", "protocol not supported" },
    { 94, "ESOCKTNOSUPPORT", "socket type not supported" },
    { 95, "EOPNOTSUPP", "operation not supported on transport" },
    { 96, "EPFNOSUPPORT", "protocol family not supported" },
    { 97, "EAFNOSUPPORT", "address family not supported" },
    { 98, "EADDRINUSE", "address in use; SO_REUSEADDR?" },
    { 99, "EADDRNOTAVAIL", "address not available; check bind" },
    { 100, "ENETDOWN", "network down; check link" },
    { 101, "ENETUNREACH", "network unreachable; check route" },
    { 102, "ENETRESET", "network dropped connection on reset" },
    { 103, "ECONNABORTED", "connection aborted; check peer" },
    { 104, "ECONNRESET", "connection reset by peer" },
    { 105, "ENOBUFS", "no buffer space; drain rings" },
    { 106, "EISCONN", "already connected" },
    { 107, "ENOTCONN", "not connected" },
    { 108, "ESHUTDOWN", "cannot send after shutdown" },
    { 109, "ETOOMANYREFS", "too many references" },
    { 110, "ETIMEDOUT", "connection timed out; check route/firewall" },
    { 111, "ECONNREFUSED", "connection refused; listener up?" },
    { 112, "EHOSTDOWN", "host down" },
    { 113, "EHOSTUNREACH", "no route to host; check gateway" },
    { 114, "EALREADY", "operation already in progress" },
    { 115, "EINPROGRESS", "operation now in progress" },
    { 116, "ESTALE", "stale file handle; server rebooted?" },
};

const char *kpanic_errno_name(int code) {
    uint32_t c = (code < 0) ? (uint32_t)(-code) : (uint32_t)code;
    for (uint32_t i = 0; i < sizeof(k_errnos) / sizeof(k_errnos[0]); i++)
        if ((uint32_t)k_errnos[i].code == c) return k_errnos[i].name;
    return "EUNKNOWN";
}

const char *kpanic_errno_fix(int code) {
    uint32_t c = (code < 0) ? (uint32_t)(-code) : (uint32_t)code;
    for (uint32_t i = 0; i < sizeof(k_errnos) / sizeof(k_errnos[0]); i++)
        if ((uint32_t)k_errnos[i].code == c) return k_errnos[i].fix;
    return "unknown errno; check the call site";
}

/* Code detail: second-opinion decode for the record code, labeled    */
/* honestly. Cause-table hits print tag:fix. Misses print a qualified */
/* guess (packing assumption stated) or nothing. Never presented as   */
/* fact when it is a guess: the prefix is "maybe-".                   */

void kpanic_subsys_code_detail(kpanic_domain_t domain, uint32_t code,
                               char *out, uint32_t out_len) {
    uint32_t p = 0;
    const kpanic_cause_t *c = NULL;
    if (!out || !out_len) return;
    out[0] = '\0';
    switch (domain) {
    case KP_DOMAIN_GUI:
        c = cause_lookup(k_gui_causes,
                         sizeof(k_gui_causes) / sizeof(k_gui_causes[0]), code);
        break;
    case KP_DOMAIN_VFS:
        c = cause_lookup(k_vfs_causes,
                         sizeof(k_vfs_causes) / sizeof(k_vfs_causes[0]), code);
        break;
    case KP_DOMAIN_NET:
        c = cause_lookup(k_net_causes,
                         sizeof(k_net_causes) / sizeof(k_net_causes[0]), code);
        break;
    case KP_DOMAIN_SCHED:
        c = cause_lookup(k_sched_causes,
                         sizeof(k_sched_causes) / sizeof(k_sched_causes[0]), code);
        break;
    case KP_DOMAIN_DRIVER:
        c = cause_lookup(k_drv_causes,
                         sizeof(k_drv_causes) / sizeof(k_drv_causes[0]), code);
        break;
    default:
        break;
    }
    if (c) {
        aux_str(out, out_len, &p, c->tag);
        aux_str(out, out_len, &p, ": ");
        aux_str(out, out_len, &p, c->fix);
        out[p] = '\0';
        return;
    }
    /* Miss: domain-specific second opinion, labeled as such. */
    if (domain == KP_DOMAIN_DRIVER) {
        char b[64];
        aux_str(out, out_len, &p, "unlisted driver code; maybe-ATA-status[");
        kpanic_decode_ata_status((uint8_t)(code & 0xFFu), b, sizeof(b));
        aux_str(out, out_len, &p, b);
        aux_str(out, out_len, &p, "] maybe-ATA-error[");
        kpanic_decode_ata_error((uint8_t)(code & 0xFFu), b, sizeof(b));
        aux_str(out, out_len, &p, b);
        aux_str(out, out_len, &p, "] maybe-PCI-class[");
        aux_str(out, out_len, &p,
                kpanic_pci_class_name((uint8_t)((code >> 16) & 0xFFu),
                                      (uint8_t)((code >> 8) & 0xFFu)));
        aux_str(out, out_len, &p, "]");
    } else if (domain == KP_DOMAIN_NET) {
        uint8_t al = (uint8_t)(code & 0xFFu);
        aux_str(out, out_len, &p, "unlisted net code; maybe-TLS-alert[");
        aux_str(out, out_len, &p, kpanic_tls_alert_name(al));
        aux_str(out, out_len, &p, ": ");
        aux_str(out, out_len, &p, kpanic_tls_alert_fix(al));
        aux_str(out, out_len, &p, "]");
    } else if (domain == KP_DOMAIN_VFS) {
        if (code > 0 && code < 200) {
            aux_str(out, out_len, &p, "errno ");
            aux_str(out, out_len, &p, kpanic_errno_name((int)code));
            aux_str(out, out_len, &p, ": ");
            aux_str(out, out_len, &p, kpanic_errno_fix((int)code));
        } else {
            aux_str(out, out_len, &p, "unlisted fs code (not an errno)");
        }
    } else if (domain == KP_DOMAIN_HEAP) {
        aux_str(out, out_len, &p, "see heap-valid flag; code is the detector id");
    } else {
        aux_str(out, out_len, &p, "no reference entry for this code");
    }
    out[p] = '\0';
}

/* Domain hint table for the help screen: every domain, what raises   */
/* it, and which live section to read first.                          */

typedef struct kpanic_domain_hint {
    kpanic_domain_t domain;
    const char *raised_by;
    const char *read_first;
} kpanic_domain_hint_t;

static const kpanic_domain_hint_t k_domain_hints[] = {
    { KP_DOMAIN_CPU, "x86 exception via ISR", "VEC + ERR + CR2 + CODE" },
    { KP_DOMAIN_OOM, "kmalloc returns NULL", "GRADE + HEAP + BUDGET" },
    { KP_DOMAIN_VRAM, "framebuffer alloc fails", "VRAM + BUDGET + HEAP" },
    { KP_DOMAIN_GUI, "window manager fault", "DETAIL + VRAM + TRACE" },
    { KP_DOMAIN_VFS, "FAT/ISO/ATA error", "DETAIL + ERRNO + TRACE" },
    { KP_DOMAIN_NET, "TCP/TLS/driver error", "DETAIL + TRACE" },
    { KP_DOMAIN_SCHED, "watchdog or sched fault", "ticks + TRACE" },
    { KP_DOMAIN_DRIVER, "PCI/ATA/NIC fault", "DETAIL + PCI/ATA decode" },
    { KP_DOMAIN_HEAP, "MCB validation fails", "GRADE + HEAP (distrust!)" },
    { KP_DOMAIN_ASSERT, "explicit KP_ASSERT", "file:line + TRACE" },
};

void kpanic_cause_count_serial(void);
void kpanic_domain_hint_serial(void) {
    char nb[16];
    kpanic_serial_puts("--- domains ---\n");
    for (uint32_t i = 0; i < sizeof(k_domain_hints) / sizeof(k_domain_hints[0]); i++) {
        kpanic_serial_puts("[");
        kpanic_serial_puts(kpanic_domain_name(k_domain_hints[i].domain));
        kpanic_serial_puts("] raised-by: ");
        kpanic_serial_puts(k_domain_hints[i].raised_by);
        kpanic_serial_puts("; read: ");
        kpanic_serial_puts(k_domain_hints[i].read_first);
        kpanic_serial_puts("\n");
    }
    kpanic_serial_puts("cause codes: GUI=0x4755xx VFS=0x5646xx NET=0x4E45xx SCHED=0x534348xx\n");
    kpanic_u32_dec(KP_DOMAIN_COUNT, nb);
    kpanic_serial_puts("domains total: ");
    kpanic_serial_puts(nb);
    kpanic_serial_puts("\n");
    kpanic_cause_count_serial();
    kpanic_serial_puts("--- end domains ---\n");
}

/* Cause-table sizes per domain, for the help screen and for auditing */
/* that every code family has reference text.                         */
void kpanic_cause_count_serial(void) {
    char nb[16];
    kpanic_serial_puts("cause entries: GUI=");
    kpanic_u32_dec(sizeof(k_gui_causes) / sizeof(k_gui_causes[0]), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" VFS=");
    kpanic_u32_dec(sizeof(k_vfs_causes) / sizeof(k_vfs_causes[0]), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" NET=");
    kpanic_u32_dec(sizeof(k_net_causes) / sizeof(k_net_causes[0]), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" SCHED=");
    kpanic_u32_dec(sizeof(k_sched_causes) / sizeof(k_sched_causes[0]), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" DRV=");
    kpanic_u32_dec(sizeof(k_drv_causes) / sizeof(k_drv_causes[0]), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" PCI-class=");
    kpanic_u32_dec(sizeof(k_pci_classes) / sizeof(k_pci_classes[0]), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" TLS-alert=");
    kpanic_u32_dec(sizeof(k_tls_alerts) / sizeof(k_tls_alerts[0]), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" errno=");
    kpanic_u32_dec(sizeof(k_errnos) / sizeof(k_errnos[0]), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts("\n");
}
