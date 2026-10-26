#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "common.h"
#include "state.h"
#include "arch.h"
#include "variant.h"

static inline void outb(uint16_t port, uint8_t v) {
    asm volatile ("outb %0, %1" :: "a"(v), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile ("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    asm volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

void arch_init_output(void) {
}

void arch_out_char(char c) {
    outb(0xe9, (uint8_t)c);
}

const char *arch_name(void) {
    return "x86-64";
}

void arch_cpu_relax(void) {
    asm volatile ("pause");
}

uint64_t arch_counter(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t arch_counter_hw_freq(void) {
    // CPUID 0x15 states the TSC rate on processors that enumerate it; QEMU's
    // default models do not, so there is nothing to compare against here.
    return 0;
}

bool arch_has_nx(void) {
    uint32_t r[4];
    cpuid(0x80000001, 0, r);
    return (r[3] >> 20) & 1;
}

int arch_cache_normal(void) {
    return 0;
}

int arch_cache_fb(void) {
    return 5;
}

uint64_t arch_paging_mode(void) {
    uint64_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    return (cr4 >> 12) & 1;
}

#define ADDR_MASK 0x000ffffffffff000

bool arch_walk(uint64_t va, struct pt_walk *w) {
    if (!have_hhdm) {
        return false;
    }
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    int levels = arch_paging_mode() ? 5 : 4;
    uint64_t table = cr3 & ADDR_MASK;
    bool writable = true, nx = false;
    for (int level = levels; level >= 1; level--) {
        int shift = 12 + 9 * (level - 1);
        uint64_t e = ((volatile uint64_t *)(uintptr_t)(hhdm_offset + table))[(va >> shift) & 511];
        if (!(e & 1)) {
            return false;
        }
        writable = writable && (e & 2);
        nx = nx || (e >> 63);
        bool leaf = level == 1 || ((level == 2 || level == 3) && (e & 0x80));
        if (leaf) {
            uint64_t size = (uint64_t)1 << shift;
            int pat = level == 1 ? (e >> 7) & 1 : (e >> 12) & 1;
            w->phys = (e & ADDR_MASK & ~(size - 1)) + (va & (size - 1));
            w->page_size = size;
            w->writable = writable;
            w->executable = !nx;
            w->cache = pat * 4 + ((e >> 4) & 1) * 2 + ((e >> 3) & 1);
            return true;
        }
        table = e & ADDR_MASK;
    }
    return false;
}

// A window of page table entries the kernel owns, for MMIO and for firmware
// tables a base revision leaves out of the HHDM. It takes a free 2MiB slot of
// the topmost GiB, as KASLR can place the executable in any of them.
#define WINDOW_GIB 0xffffffffc0000000
#define WINDOW_PAGES 512
static uint64_t window_pt[512] __attribute__((aligned(4096)));
static uint64_t window_pd[512] __attribute__((aligned(4096)));
static uint64_t window_pdpt[512] __attribute__((aligned(4096)));
static uint64_t window_base;
static unsigned window_used;
static bool window_ready;

static uint64_t virt_to_phys(void *p) {
    struct pt_walk w;
    if (!arch_walk((uint64_t)(uintptr_t)p, &w)) {
        return 0;
    }
    return w.phys;
}

static bool window_init(void) {
    if (window_ready) {
        return true;
    }
    if (!have_hhdm) {
        return false;
    }
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    int levels = arch_paging_mode() ? 5 : 4;
    uint64_t table = cr3 & ADDR_MASK;
    uint64_t va = WINDOW_GIB;
    // Descend to the PML4 (or PML5's last entry, then the PML4).
    for (int level = levels; level > 3; level--) {
        int shift = 12 + 9 * (level - 1);
        volatile uint64_t *t = (volatile uint64_t *)(uintptr_t)(hhdm_offset + table);
        uint64_t idx = (va >> shift) & 511;
        if (!(t[idx] & 1)) {
            if (level == 4) {
                t[idx] = virt_to_phys(window_pdpt) | 3;
            } else {
                return false;
            }
        }
        table = t[idx] & ADDR_MASK;
    }
    volatile uint64_t *pdpt = (volatile uint64_t *)(uintptr_t)(hhdm_offset + table);
    uint64_t pdpt_idx = (va >> 30) & 511;
    if (!(pdpt[pdpt_idx] & 1)) {
        pdpt[pdpt_idx] = virt_to_phys(window_pd) | 3;
    } else if (pdpt[pdpt_idx] & 0x80) {
        return false;
    }
    volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)(hhdm_offset + (pdpt[pdpt_idx] & ADDR_MASK));
    int pd_idx = 511;
    while (pd_idx >= 0 && (pd[pd_idx] & 1)) {
        pd_idx--;
    }
    if (pd_idx < 0) {
        return false;
    }
    pd[pd_idx] = virt_to_phys(window_pt) | 3;
    window_base = va + ((uint64_t)pd_idx << 21);
    window_ready = true;
    return true;
}

// Maps [phys, phys + len) uncached and returns its address, or NULL.
static volatile void *map_phys(uint64_t phys, size_t len) {
    if (!window_init()) {
        return NULL;
    }
    uint64_t first = phys & ~(uint64_t)4095;
    uint64_t pages = (phys + len - first + 4095) / 4096;
    if (window_used + pages > WINDOW_PAGES) {
        return NULL;
    }
    uint64_t va = window_base + (uint64_t)window_used * 4096;
    for (uint64_t i = 0; i < pages; i++) {
        // PCD and PWT: PAT 3, uncached.
        window_pt[window_used + i] = (first + i * 4096) | 0x1b;
        asm volatile ("invlpg (%0)" :: "r"(va + i * 4096) : "memory");
    }
    window_used += pages;
    return (volatile void *)(uintptr_t)(va + (phys - first));
}

struct gdt_desc {
    uint64_t base, limit;
    unsigned type, s, dpl, p, l, db, g;
};

static struct gdt_desc decode_desc(uint64_t d) {
    struct gdt_desc r;
    r.limit = (d & 0xffff) | ((d >> 32) & 0xf0000);
    r.base = ((d >> 16) & 0xffffff) | (((d >> 56) & 0xff) << 24);
    r.type = (d >> 40) & 0xf;
    r.s = (d >> 44) & 1;
    r.dpl = (d >> 45) & 3;
    r.p = (d >> 47) & 1;
    r.l = (d >> 53) & 1;
    r.db = (d >> 54) & 1;
    r.g = (d >> 55) & 1;
    if (r.g) {
        r.limit = (r.limit << 12) | 0xfff;
    }
    return r;
}

static void check_gdt(int base_rev) {
    // SGDT stored the limit and then the base, ten bytes, from this slot on.
    uint16_t limit = bsp_state[ST_GDTR] & 0xffff;
    uint64_t base = (bsp_state[ST_GDTR] >> 16) | (bsp_state[ST_GDTR + 1] << 48);
    fact("x86.gdtr", "%lx %x", base, limit);
    check(limit >= 7 * 8 - 1, "x86.gdt_limit", "limit %x leaves out descriptors", limit);
    if (have_hhdm) {
        check(base >= hhdm_offset, "x86.gdt_hhdm", "GDT base %lx is not an HHDM address", base);
    }
    struct pt_walk w;
    // Without the HHDM there is no walking the tables, but the GDT is mapped
    // all the same and is read through its own address below.
    if (have_hhdm && !arch_walk(base, &w)) {
        check(false, "x86.gdt_mapped", "GDT base %lx is not mapped", base);
        return;
    }
    volatile uint64_t *gdt = (volatile uint64_t *)(uintptr_t)base;
    check(gdt[0] == 0, "x86.gdt_null", "entry 0 is %lx", gdt[0]);
    struct {
        const char *name;
        bool code;
        uint64_t limit;
        unsigned l, db;
    } want[] = {
        { "code16", true, 0xffff, 0, 0 },
        { "data16", false, 0xffff, 0, 0 },
        { "code32", true, 0xffffffff, 0, 1 },
        { "data32", false, 0xffffffff, 0, 1 },
        { "code64", true, 0, 1, 0 },
        { "data64", false, 0, 0, 0 },
    };
    for (int i = 0; i < 6; i++) {
        struct gdt_desc d = decode_desc(gdt[i + 1]);
        bool ok = d.base == 0 && d.p && d.s && d.dpl == 0;
        if (want[i].code) {
            // Execute/read code, the accessed bit aside.
            ok = ok && (d.type & 0xa) == 0xa;
        } else {
            ok = ok && (d.type & 0xa) == 0x2;
        }
        if (want[i].limit != 0) {
            ok = ok && d.limit == want[i].limit;
        }
        if (want[i].code && i != 4) {
            ok = ok && d.db == want[i].db && d.l == 0;
        }
        if (i == 3) {
            ok = ok && d.db == 1;
        }
        if (i == 4) {
            ok = ok && d.l == 1 && d.db == 0;
        }
        kprintf("K x86.gdt.%d=%016lx\n", i + 1, gdt[i + 1]);
        check(ok, want[i].name, "descriptor %016lx: base %lx limit %lx type %x p %u s %u l %u db %u",
              gdt[i + 1], d.base, d.limit, d.type, d.p, d.s, d.l, d.db);
    }
    (void)base_rev;
}

static const char *gpr_names[16] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static void check_regs(const char *who, uint64_t *st, int base_rev, bool rdi_is_arg, uint64_t rdi_want) {
    char name[48];
    size_t n = 0;
    while (who[n] != 0) {
        name[n] = who[n];
        n++;
    }
#define NAME(S) (memcpy(name + n, S, sizeof(S)), name)
    bool gprs_zero = true;
    for (int i = 0; i < 16; i++) {
        if (i == ST_RSP) {
            continue;
        }
        if (i == ST_RDI && rdi_is_arg) {
            check(st[ST_RDI] == rdi_want, NAME(".rdi"), "rdi %lx, want %lx", st[ST_RDI], rdi_want);
            continue;
        }
        if (st[i] != 0) {
            gprs_zero = false;
            kprintf("C FAIL %s.gpr: %s=%lx\n", who, gpr_names[i], st[i]);
        }
    }
    check(gprs_zero, NAME(".gprs_zero"), "see above");
    check(st[ST_RET] == 0, NAME(".return_address"), "[rsp] is %lx", st[ST_RET]);
    check(st[ST_RSP] % 16 == 8, NAME(".rsp_alignment"), "rsp %lx", st[ST_RSP]);
    check(st[ST_CS] == 0x28, NAME(".cs"), "cs %lx", st[ST_CS]);
    bool data_sel = st[ST_DS] == 0x30 && st[ST_ES] == 0x30 && st[ST_SS] == 0x30
                 && st[ST_FS] == 0x30 && st[ST_GS] == 0x30;
    check(data_sel, NAME(".data_selectors"), "ds %lx es %lx ss %lx fs %lx gs %lx",
          st[ST_DS], st[ST_ES], st[ST_SS], st[ST_FS], st[ST_GS]);
    check(st[ST_FS_BASE] == 0 && st[ST_GS_BASE] == 0, NAME(".fs_gs_base"), "fs base %lx gs base %lx",
          st[ST_FS_BASE], st[ST_GS_BASE]);
    uint64_t rf = st[ST_RFLAGS];
    check(!(rf & (1 << 9)) && !(rf & (1 << 17)) && !(rf & (1 << 10)), NAME(".rflags_if_vm_df"),
          "rflags %lx", rf);
    uint64_t cr0 = st[ST_CR0], cr4 = st[ST_CR4], efer = st[ST_EFER];
    bool nx = arch_has_nx();
    check((cr0 & 0x80010011) == 0x80010011, NAME(".cr0_required"), "cr0 %lx", cr0);
    check(cr4 & (1 << 5), NAME(".cr4_pae"), "cr4 %lx", cr4);
    check((efer & 0x500) == 0x500, NAME(".efer_lme_lma"), "efer %lx", efer);
    check(!!(efer & (1 << 11)) == nx, NAME(".efer_nxe"), "efer %lx, nx available %d", efer, nx);
    if (base_rev >= 5) {
        check(rf == 0x2, NAME(".rflags_exact"), "rflags %lx", rf);
        check(cr0 == 0x80010011, NAME(".cr0_exact"), "cr0 %lx", cr0);
        uint64_t want_cr4 = (1 << 5) | (arch_paging_mode() ? (1 << 12) : 0);
        check(cr4 == want_cr4, NAME(".cr4_exact"), "cr4 %lx, want %lx", cr4, want_cr4);
        uint64_t want_efer = 0x500 | (nx ? 0x800 : 0);
        check(efer == want_efer, NAME(".efer_exact"), "efer %lx, want %lx", efer, want_efer);
        uint16_t idt_limit = *(uint16_t *)&st[ST_IDTR];
        uint64_t idt_base = *(uint64_t *)((uint8_t *)&st[ST_IDTR] + 2);
        check(idt_base == 0 && idt_limit == 0, NAME(".idtr"), "idtr %lx:%x", idt_base, idt_limit);
        check(st[ST_LDTR] == 0, NAME(".ldtr"), "ldtr selector %lx", st[ST_LDTR]);
        // A null selector cannot be loaded into TR, so "base 0 and limit 0"
        // means a TSS descriptor with both, whatever its selector.
        uint16_t gdt_limit = *(uint16_t *)&st[ST_GDTR];
        uint64_t gdt_base = *(uint64_t *)((uint8_t *)&st[ST_GDTR] + 2);
        uint64_t sel = st[ST_TR] & ~(uint64_t)7;
        struct pt_walk w;
        if (sel == 0 || sel + 15 > gdt_limit || (have_hhdm && !arch_walk(gdt_base + sel, &w))) {
            check(false, NAME(".tr"), "tr selector %lx does not name a descriptor", st[ST_TR]);
        } else {
            volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)(gdt_base + sel);
            struct gdt_desc t = decode_desc(d[0]);
            uint64_t base = t.base | (d[1] & 0xffffffff) << 32;
            check((t.type == 0x9 || t.type == 0xb) && t.s == 0 && t.p && base == 0 && t.limit == 0,
                  NAME(".tr"), "tr %lx descriptor %016lx %016lx: type %x base %lx limit %lx",
                  st[ST_TR], d[0], d[1], t.type, base, t.limit);
        }
    }
    uint32_t r[4];
    cpuid(1, 0, r);
    if (r[3] & (1 << 16)) {
        check((st[ST_PAT] & 0xffffffffffff) == 0x010500070406, NAME(".pat"),
              "PAT %016lx", st[ST_PAT]);
    }
#undef NAME
}

void arch_check_entry(int base_rev, uint64_t entry_point) {
    (void)entry_point;
    uint64_t *st = bsp_state;
    kprintf("K x86.entry=rflags %lx cr0 %lx cr3 %lx cr4 %lx efer %lx pat %lx apic_base %lx\n",
            st[ST_RFLAGS], st[ST_CR0], st[ST_CR3], st[ST_CR4], st[ST_EFER], st[ST_PAT],
            st[ST_APIC_BASE]);
    kprintf("K x86.selectors=cs %lx ds %lx es %lx ss %lx fs %lx gs %lx tr %lx ldtr %lx\n",
            st[ST_CS], st[ST_DS], st[ST_ES], st[ST_SS], st[ST_FS], st[ST_GS], st[ST_TR], st[ST_LDTR]);
    fact("x86.nx", "%d", arch_has_nx());
    check_regs("x86.bsp", st, base_rev, false, 0);
    check_gdt(base_rev);
}

// What an AP saw of its local APIC, read by the AP itself.
static volatile uint32_t ap_lapic[MAX_APS][8];

static bool x2apic_mode(void) {
    return (rdmsr(0x1b) >> 10) & 1;
}

static volatile uint32_t *xapic;

static uint32_t lapic_read(uint32_t reg) {
    if (x2apic_mode()) {
        return (uint32_t)rdmsr(0x800 + reg / 16);
    }
    return xapic[reg / 4];
}

static const uint32_t lvt_regs[] = { 0x320, 0x330, 0x340, 0x350, 0x360, 0x370, 0x2f0 };
static const char *lvt_names[] = { "timer", "thermal", "perf", "lint0", "lint1", "error", "cmci" };

static void lapic_snapshot(volatile uint32_t *out) {
    out[0] = lapic_read(0xf0);
    out[1] = lapic_read(0x80);
    for (int i = 0; i < 5; i++) {
        out[2 + i] = lapic_read(lvt_regs[i]);
    }
    out[7] = lapic_read(lvt_regs[5]);
}

void arch_prepare_aps(void) {
    if (have_hhdm && !x2apic_mode() && xapic == NULL) {
        xapic = map_phys(bsp_state[ST_APIC_BASE] & 0xfffff000, 4096);
    }
}

void arch_ap_hook(unsigned slot) {
    if (slot < MAX_APS && (x2apic_mode() || xapic != NULL)) {
        lapic_snapshot(ap_lapic[slot]);
        ap_lapic[slot][7] = 1;
    }
}

void arch_check_ap(int base_rev, unsigned slot, struct limine_mp_info *info) {
    uint64_t *st = ap_states[slot];
    char who[24] = "x86.ap.";
    who[7] = '0' + (slot / 10) % 10;
    who[8] = '0' + slot % 10;
    who[9] = 0;
    check_regs(who, st, base_rev, true, (uint64_t)(uintptr_t)info);
    check(st[ST_CR3] == bsp_state[ST_CR3], "x86.ap.cr3", "AP %u cr3 %lx, BSP %lx", slot,
          st[ST_CR3], bsp_state[ST_CR3]);
    check(memcmp(&st[ST_GDTR], &bsp_state[ST_GDTR], 10) == 0, "x86.ap.gdtr", "AP %u GDTR differs", slot);
    volatile uint32_t *l = ap_lapic[slot];
    if (l[7] == 1) {
        kprintf("K x86.ap.%u.lapic=svr %x tpr %x timer %x thermal %x perf %x lint0 %x\n", slot,
                l[0], l[1], l[2], l[3], l[4], l[5]);
        if (base_rev >= 5) {
            check(l[0] == 0x1ff && l[1] == 0, "x86.ap.lapic_svr_tpr", "AP %u SVR %x TPR %x", slot, l[0], l[1]);
            bool masked = true;
            for (int i = 0; i < 5; i++) {
                unsigned mode = (l[2 + i] >> 8) & 7;
                if ((mode == 0 || mode == 1 || mode == 4 || mode == 7) && !(l[2 + i] & (1 << 16))) {
                    masked = false;
                }
            }
            check(masked, "x86.ap.lapic_lvt_masked", "AP %u has an unmasked LVT entry", slot);
        }
    }
}

static void check_pic(void) {
    uint8_t m1 = inb(0x21), m2 = inb(0xa1);
    fact("x86.pic_masks", "%02x %02x", m1, m2);
    check(m1 == 0xff && m2 == 0xff, "x86.pic_masked", "masks %02x %02x", m1, m2);
}

struct acpi_sdt {
    char sig[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_rev;
    uint32_t creator;
    uint32_t creator_rev;
} __attribute__((packed));

static volatile struct acpi_sdt *map_table(uint64_t phys) {
    volatile struct acpi_sdt *hdr = map_phys(phys, sizeof(struct acpi_sdt));
    if (hdr == NULL) {
        return NULL;
    }
    return map_phys(phys, hdr->length);
}

static volatile struct acpi_sdt *find_sdt(uint64_t rsdp_phys, const char *sig) {
    volatile uint8_t *rsdp = map_phys(rsdp_phys, 36);
    if (rsdp == NULL) {
        return NULL;
    }
    bool xsdt = rsdp[15] >= 2;
    uint64_t root = xsdt ? *(volatile uint64_t *)(rsdp + 24) : *(volatile uint32_t *)(rsdp + 16);
    volatile struct acpi_sdt *rt = map_table(root);
    if (rt == NULL) {
        return NULL;
    }
    size_t esize = xsdt ? 8 : 4;
    size_t n = (rt->length - sizeof(struct acpi_sdt)) / esize;
    for (size_t i = 0; i < n; i++) {
        volatile uint8_t *ents = (volatile uint8_t *)(rt + 1);
        uint64_t p = xsdt ? *(volatile uint64_t *)(ents + i * 8) : *(volatile uint32_t *)(ents + i * 4);
        volatile struct acpi_sdt *t = map_phys(p, sizeof(struct acpi_sdt));
        if (t != NULL && t->sig[0] == sig[0] && t->sig[1] == sig[1] && t->sig[2] == sig[2] && t->sig[3] == sig[3]) {
            return map_table(p);
        }
    }
    return NULL;
}

static void check_ioapics(int base_rev, volatile struct acpi_sdt *madt) {
    volatile uint8_t *p = (volatile uint8_t *)madt + 44;
    volatile uint8_t *end = (volatile uint8_t *)madt + madt->length;
    unsigned count = 0;
    bool ok = true;
    while (p + 2 <= end && p[1] >= 2) {
        if (p[0] == 1) {
            uint32_t addr = *(volatile uint32_t *)(p + 4);
            volatile uint32_t *io = map_phys(addr, 0x20);
            if (io == NULL) {
                check(false, "x86.ioapic_map", "could not map IOAPIC at %x", addr);
                return;
            }
            io[0] = 1;
            uint32_t ver = io[4];
            unsigned entries = ((ver >> 16) & 0xff) + 1;
            for (unsigned i = 0; i < entries; i++) {
                io[0] = 0x10 + i * 2;
                uint32_t lo = io[4];
                unsigned mode = (lo >> 8) & 7;
                bool must_mask = mode == 0 || mode == 1 || (base_rev >= 5 && (mode == 4 || mode == 7));
                if (must_mask && !(lo & (1 << 16))) {
                    ok = false;
                    kprintf("C FAIL x86.ioapic_entry: IOAPIC %x pin %u mode %u unmasked (%08x)\n",
                            addr, i, mode, lo);
                }
            }
            count++;
        }
        p += p[1];
    }
    fact("x86.ioapics", "%u", count);
    check(ok, "x86.ioapic_masked", "see above");
}

static void check_lapic(int base_rev) {
    uint64_t base = bsp_state[ST_APIC_BASE];
    fact("x86.apic_mode", "%s", x2apic_mode() ? "x2apic" : "xapic");
    if (!x2apic_mode()) {
        xapic = map_phys(base & 0xfffff000, 4096);
        if (xapic == NULL) {
            check(false, "x86.lapic_map", "could not map the local APIC");
            return;
        }
    }
    uint32_t snap[8];
    lapic_snapshot(snap);
    fact("x86.lapic", "svr %x tpr %x timer %x thermal %x perf %x lint0 %x lint1 %x error %x",
         snap[0], snap[1], snap[2], snap[3], snap[4], snap[5], snap[6], snap[7]);
    if (base_rev < 5) {
        return;
    }
    check(rdmsr(0x1b) & (1 << 11), "x86.lapic_enabled", "IA32_APIC_BASE %lx", rdmsr(0x1b));
    check(snap[0] == 0x1ff, "x86.lapic_svr", "SVR %x", snap[0]);
    check(snap[1] == 0, "x86.lapic_tpr", "TPR %x", snap[1]);
    unsigned max_lvt = (lapic_read(0x30) >> 16) & 0xff;
    fact("x86.lapic_max_lvt", "%u", max_lvt);
    for (int i = 0; i < 6; i++) {
        // The thermal and performance counter entries need LVT counts 5 and 4.
        if ((i == 1 && max_lvt < 5) || (i == 2 && max_lvt < 4)) {
            continue;
        }
        uint32_t v = snap[2 + i];
        unsigned mode = (v >> 8) & 7;
        // Timer and error entries have no delivery mode field, read as fixed.
        bool must_mask = mode == 0 || mode == 1 || mode == 4 || mode == 7;
        if (must_mask) {
            char name[32] = "x86.lapic_lvt_";
            size_t n = 14;
            for (const char *s = lvt_names[i]; *s != 0; s++) {
                name[n++] = *s;
            }
            name[n] = 0;
            check(v & (1 << 16), name, "LVT %x is %x, unmasked", lvt_regs[i], v);
        }
    }
}

// The global status of each VT-d unit the DMAR table lists (Intel VT-d 5.20
// sections 8.3 and 11.4.4.2): whether DMA and interrupt remapping are on at
// entry.
static void report_vtd(volatile struct acpi_sdt *dmar) {
    if (dmar == NULL) {
        return;
    }
    volatile uint8_t *p = (volatile uint8_t *)dmar + 48;
    volatile uint8_t *end = (volatile uint8_t *)dmar + dmar->length;
    unsigned n = 0;
    while (p + 16 <= end && *(volatile uint16_t *)(p + 2) >= 4) {
        if (*(volatile uint16_t *)p == 0) {
            volatile uint32_t *regs = map_phys(*(volatile uint64_t *)(p + 8), 4096);
            if (regs != NULL) {
                kprintf("K x86.vtd.%u.gsts=%x\n", n, regs[0x1c / 4]);
            }
            n++;
        }
        p += *(volatile uint16_t *)(p + 2);
    }
}

// The control and status registers of each AMD-Vi IOMMU the IVRS table
// lists (AMD IOMMU 3.11 section 3.4): whether it and its command buffer and
// logs are on at entry.
static void report_amdvi(volatile struct acpi_sdt *ivrs) {
    if (ivrs == NULL) {
        return;
    }
    volatile uint8_t *p = (volatile uint8_t *)ivrs + 48;
    volatile uint8_t *end = (volatile uint8_t *)ivrs + ivrs->length;
    unsigned n = 0;
    while (p + 16 <= end && *(volatile uint16_t *)(p + 2) >= 4) {
        if (*p == 0x10 || *p == 0x11 || *p == 0x40) {
            volatile uint8_t *regs = map_phys(*(volatile uint64_t *)(p + 8), 0x3000);
            if (regs != NULL) {
                kprintf("K x86.amdvi.%u=control %lx status %lx\n", n, *(volatile uint64_t *)(regs + 0x18),
                        *(volatile uint64_t *)(regs + 0x2020));
            }
            n++;
        }
        p += *(volatile uint16_t *)(p + 2);
    }
}

void arch_check_late(int base_rev, uint64_t rsdp_phys) {
    check_pic();
    if (!have_hhdm) {
        return;
    }
    uint32_t r[4];
    cpuid(1, 0, r);
    if (r[3] & (1 << 9)) {
        check_lapic(base_rev);
    }
    if (rsdp_phys != 0) {
        volatile struct acpi_sdt *madt = find_sdt(rsdp_phys, "APIC");
        fact("x86.madt", "%d", madt != NULL);
        if (madt != NULL) {
            check_ioapics(base_rev, madt);
        }
        report_vtd(find_sdt(rsdp_phys, "DMAR"));
        report_amdvi(find_sdt(rsdp_phys, "IVRS"));
    }
}

void arch_dtb(const void *dtb) {
    (void)dtb;
}
