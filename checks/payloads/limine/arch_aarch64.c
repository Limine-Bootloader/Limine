#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "common.h"
#include "state.h"
#include "arch.h"

#define READ_SYSREG(r) ({ uint64_t v; asm volatile ("mrs %0, " #r : "=r"(v)); v; })
#define WRITE_SYSREG(r, v) asm volatile ("msr " #r ", %0" :: "r"((uint64_t)(v)) : "memory")

// QEMU virt's PL011.
#define UART_BASE 0x09000000
static volatile uint32_t *uart;

static uint64_t tables[4][512] __attribute__((aligned(4096)));

static int current_el(void) {
    return (READ_SYSREG(CurrentEL) >> 2) & 3;
}

static uint64_t at_phys(uint64_t va) {
    uint64_t par;
    if (current_el() == 2) {
        asm volatile ("at s1e2r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
    } else {
        asm volatile ("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
    }
    if (par & 1) {
        return 0;
    }
    return (par & 0x0000fffffffff000) | (va & 0xfff);
}

// TTBR0 is the executable's to use from base revision 1: map the UART there,
// as Device-nGnRnE through MAIR attribute 2, which the protocol leaves unused.
void arch_init_output(void) {
    uint64_t tcr = READ_SYSREG(tcr_el1);
    unsigned t0sz = tcr & 0x3f;
    unsigned va_bits = 64 - t0sz;
    // 4KiB granule: each level resolves 9 bits above the 12 of the page.
    int start = va_bits > 48 ? -1 : va_bits > 39 ? 0 : va_bits > 30 ? 1 : 2;
    uint64_t va = UART_BASE;
    int t = 0;
    for (int level = start; level < 2; level++) {
        int shift = 12 + 9 * (3 - level);
        uint64_t idx = (va >> shift) & (level == -1 ? 0xf : 511);
        tables[t][idx] = at_phys((uint64_t)(uintptr_t)tables[t + 1]) | 3;
        t++;
    }
    // A 2MiB block at level 2: AttrIndx 2, AF, PXN, UXN.
    tables[t][(va >> 21) & 511] = (va & ~(uint64_t)0x1fffff) | (2 << 2) | (1 << 10)
                                 | ((uint64_t)3 << 53) | 1;
    uint64_t mair = READ_SYSREG(mair_el1);
    mair &= ~((uint64_t)0xff << 16);
    WRITE_SYSREG(mair_el1, mair);
    uint64_t ttbr0 = at_phys((uint64_t)(uintptr_t)tables[0]);
    asm volatile ("dsb ishst" ::: "memory");
    WRITE_SYSREG(ttbr0_el1, ttbr0);
    asm volatile ("isb\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
    uart = (volatile uint32_t *)UART_BASE;
}

void arch_out_char(char c) {
    if (uart == NULL) {
        return;
    }
    while (uart[0x18 / 4] & (1 << 5)) {
    }
    uart[0] = (uint8_t)c;
}

const char *arch_name(void) {
    return "aarch64";
}

void arch_cpu_relax(void) {
    asm volatile ("yield");
}

uint64_t arch_counter(void) {
    asm volatile ("isb" ::: "memory");
    return READ_SYSREG(cntpct_el0);
}

uint64_t arch_counter_hw_freq(void) {
    return READ_SYSREG(cntfrq_el0) & 0xffffffff;
}

bool arch_has_nx(void) {
    return true;
}

int arch_cache_normal(void) {
    return 0;
}

int arch_cache_fb(void) {
    return 1;
}

uint64_t arch_paging_mode(void) {
    uint64_t tcr = READ_SYSREG(tcr_el1);
    return ((tcr >> 16) & 0x3f) < 16 ? 1 : 0;
}

bool arch_walk(uint64_t va, struct pt_walk *w) {
    if (!have_hhdm) {
        return false;
    }
    uint64_t tcr = READ_SYSREG(tcr_el1);
    bool upper = va >> 63;
    unsigned tsz = upper ? (tcr >> 16) & 0x3f : tcr & 0x3f;
    uint64_t ttbr = upper ? READ_SYSREG(ttbr1_el1) : READ_SYSREG(ttbr0_el1);
    unsigned va_bits = 64 - tsz;
    int start = va_bits > 48 ? -1 : va_bits > 39 ? 0 : va_bits > 30 ? 1 : 2;
    uint64_t table = ttbr & 0x0000fffffffffffe;
    if ((tcr >> 59) & 1) {
        // DS: table address bits 51:48 are in TTBR bits 5:2.
        table = (ttbr & 0x0000ffffffffffc0) | (((ttbr >> 2) & 0xf) << 48);
    }
    bool ap_ro = false, xn = false;
    for (int level = start; level <= 3; level++) {
        int shift = 12 + 9 * (3 - level);
        uint64_t idx = (va >> shift) & (level == -1 ? 0xf : 511);
        uint64_t e = ((volatile uint64_t *)(uintptr_t)(hhdm_offset + table))[idx];
        if (!(e & 1)) {
            return false;
        }
        bool is_table = level < 3 && (e & 2);
        if (is_table) {
            ap_ro = ap_ro || ((e >> 62) & 1);
            xn = xn || ((e >> 59) & 1);
            table = e & 0x0000fffffffff000;
            continue;
        }
        if (level == 3 && !(e & 2)) {
            return false;
        }
        uint64_t size = (uint64_t)1 << shift;
        w->phys = (e & 0x0000fffffffff000 & ~(size - 1)) + (va & (size - 1));
        w->page_size = size;
        w->writable = !ap_ro && !((e >> 7) & 1);
        w->executable = !xn && !((e >> 53) & 1);
        w->cache = (e >> 2) & 7;
        return true;
    }
    return false;
}

static void check_gprs(const char *name, uint64_t *st, int keep_x0, uint64_t x0_want) {
    bool ok = true;
    for (int i = 0; i <= 30; i++) {
        if (i == 0 && keep_x0) {
            check(st[0] == x0_want, "aarch64.ap.x0", "x0 %lx, want %lx", st[0], x0_want);
            continue;
        }
        if (st[ST_X0 + i] != 0) {
            ok = false;
            kprintf("C FAIL %s: x%d=%lx\n", name, i, st[ST_X0 + i]);
        }
    }
    check(ok, name, "see above");
}

static uint64_t expected_tcr(uint64_t tsz, bool ds) {
    uint64_t pa = READ_SYSREG(id_aa64mmfr0_el1) & 0xf;
    return ((uint64_t)ds << 59) | (pa << 32) | ((uint64_t)2 << 30) | ((uint64_t)3 << 28)
         | ((uint64_t)1 << 26) | ((uint64_t)1 << 24) | (tsz << 16) | ((uint64_t)3 << 12)
         | ((uint64_t)1 << 10) | ((uint64_t)1 << 8) | tsz;
}

static void check_state(const char *who, uint64_t *st, int base_rev, bool is_bsp) {
    char name[48];
    size_t n = 0;
    while (who[n] != 0) {
        name[n] = who[n];
        n++;
    }
#define NAME(S) (memcpy(name + n, S, sizeof(S)), name)
    int el = (st[ST_CURRENTEL] >> 2) & 3;
    bool vhe = (READ_SYSREG(id_aa64mmfr1_el1) >> 8) & 0xf;
    check(st[ST_DAIF] == 0x3c0, NAME(".daif"), "DAIF %lx", st[ST_DAIF]);
    uint64_t sctlr = st[ST_SCTLR];
    check((sctlr & 1) && (sctlr & (1 << 2)) && (sctlr & (1 << 12)) && !(sctlr & 2)
       && (sctlr & (1 << 3)) && (sctlr & (1 << 4)), NAME(".sctlr_required"), "SCTLR %lx", sctlr);
    uint64_t tcr = st[ST_TCR];
    uint64_t t0sz = tcr & 0x3f, t1sz = (tcr >> 16) & 0x3f;
    bool five = arch_paging_mode() == 1;
    check(t0sz == (five ? 12 : 16) && t1sz == (five ? 12 : 16) && ((tcr >> 59) & 1) == five,
          NAME(".tcr_size"), "TCR %lx", tcr);
    check(((tcr >> 14) & 3) == 0 && ((tcr >> 30) & 3) == 2, NAME(".tcr_granule"), "TCR %lx", tcr);
    check((st[ST_MAIR] & 0xff) == 0xff, NAME(".mair_attr0"), "MAIR %lx", st[ST_MAIR]);
    if (base_rev >= 6) {
        check(el == 1 || (el == 2 && vhe), NAME(".el"), "entered at EL%d (VHE %d)", el, vhe);
        if (is_bsp) {
            check(bsp_state[ST_NZCV] == 0, NAME(".nzcv"), "NZCV %lx", bsp_state[ST_NZCV]);
        }
        check(st[ST_SPSEL] == 0, NAME(".spsel"), "SPSel %lx", st[ST_SPSEL]);
        check(sctlr == 0x30d0199d, NAME(".sctlr_exact"), "SCTLR %lx, want 30d0199d", sctlr);
        check(st[ST_CPACR] == 0, NAME(".cpacr"), "CPACR %lx", st[ST_CPACR]);
        uint64_t want = expected_tcr(five ? 12 : 16, five);
        check(tcr == want, NAME(".tcr_exact"), "TCR %lx, want %lx", tcr, want);
        if (el == 2) {
            check(st[ST_HCR_EL2] == 0x488000002, NAME(".hcr_el2"), "HCR_EL2 %lx", st[ST_HCR_EL2]);
            check(st[ST_CPTR_EL2] == 0, NAME(".cptr_el2"), "CPTR_EL2 %lx", st[ST_CPTR_EL2]);
            check(st[ST_CNTHCTL_EL2] == 3, NAME(".cnthctl_el2"), "CNTHCTL_EL2 %lx", st[ST_CNTHCTL_EL2]);
            check(st[ST_HSTR_EL2] == 0, NAME(".hstr_el2"), "HSTR_EL2 %lx", st[ST_HSTR_EL2]);
        }
    }
#undef NAME
}

static void check_pstate_extras(int base_rev) {
    // Read where implemented; reading an absent one would fault.
    uint64_t mmfr1 = READ_SYSREG(id_aa64mmfr1_el1);
    uint64_t mmfr2 = READ_SYSREG(id_aa64mmfr2_el1);
    uint64_t pfr0 = READ_SYSREG(id_aa64pfr0_el1);
    uint64_t pfr1 = READ_SYSREG(id_aa64pfr1_el1);
    if ((mmfr1 >> 20) & 0xf) {
        uint64_t v;
        asm volatile ("mrs %0, s3_0_c4_c2_3" : "=r"(v));
        fact("aarch64.pan", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_pan", "PAN %lx", v);
        }
    }
    if ((mmfr2 >> 4) & 0xf) {
        uint64_t v;
        asm volatile ("mrs %0, s3_0_c4_c2_4" : "=r"(v));
        fact("aarch64.uao", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_uao", "UAO %lx", v);
        }
    }
    if ((pfr0 >> 48) & 0xf) {
        uint64_t v;
        asm volatile ("mrs %0, s3_3_c4_c2_5" : "=r"(v));
        fact("aarch64.dit", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_dit", "DIT %lx", v);
        }
    }
    // MRS of SSBS needs FEAT_SSBS2.
    if (((pfr1 >> 4) & 0xf) >= 2) {
        uint64_t v;
        asm volatile ("mrs %0, s3_3_c4_c2_6" : "=r"(v));
        fact("aarch64.ssbs", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_ssbs", "SSBS %lx", v);
        }
    }
    if (((pfr1 >> 8) & 0xf) >= 1) {
        uint64_t v;
        asm volatile ("mrs %0, s3_3_c4_c2_7" : "=r"(v));
        fact("aarch64.tco", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_tco", "TCO %lx", v);
        }
    }
}

void arch_check_entry(int base_rev, uint64_t entry_point) {
    (void)entry_point;
    uint64_t *st = bsp_state;
    fact("aarch64.el", "%lu", (st[ST_CURRENTEL] >> 2) & 3);
    fact("aarch64.vhe", "%lu", (READ_SYSREG(id_aa64mmfr1_el1) >> 8) & 0xf);
    kprintf("K aarch64.entry=daif %lx nzcv %lx spsel %lx sctlr %lx tcr %lx mair %lx cpacr %lx\n",
            st[ST_DAIF], st[ST_NZCV], st[ST_SPSEL], st[ST_SCTLR], st[ST_TCR], st[ST_MAIR],
            st[ST_CPACR]);
    kprintf("K aarch64.ttbr=ttbr0 %lx ttbr1 %lx vbar %lx\n", st[ST_TTBR0], st[ST_TTBR1], st[ST_VBAR]);
    if (((st[ST_CURRENTEL] >> 2) & 3) == 2) {
        kprintf("K aarch64.el2=hcr %lx cptr %lx cnthctl %lx hstr %lx\n", st[ST_HCR_EL2],
                st[ST_CPTR_EL2], st[ST_CNTHCTL_EL2], st[ST_HSTR_EL2]);
    }
    check_gprs("aarch64.bsp.gprs_zero", st, 0, 0);
    check(st[ST_SP] % 16 == 0, "aarch64.bsp.sp_alignment", "SP %lx", st[ST_SP]);
    check_state("aarch64.bsp", st, base_rev, true);
    check_pstate_extras(base_rev);
}

void arch_prepare_aps(void) {
}

void arch_ap_hook(unsigned slot) {
    (void)slot;
}

void arch_check_ap(int base_rev, unsigned slot, struct limine_mp_info *info) {
    uint64_t *st = ap_states[slot];
    char who[24] = "aarch64.ap.";
    who[11] = '0' + (slot / 10) % 10;
    who[12] = '0' + slot % 10;
    who[13] = 0;
    check_gprs("aarch64.ap.gprs_zero", st, 1, (uint64_t)(uintptr_t)info);
    check(st[ST_SP] % 16 == 0, "aarch64.ap.sp_alignment", "AP %u SP %lx", slot, st[ST_SP]);
    check_state(who, st, base_rev, false);
    check(st[ST_TTBR1] == bsp_state[ST_TTBR1] && st[ST_MAIR] == bsp_state[ST_MAIR]
       && st[ST_TCR] == bsp_state[ST_TCR] && st[ST_SCTLR] == bsp_state[ST_SCTLR],
          "aarch64.ap.same_as_bsp", "AP %u differs from the BSP", slot);
    check(((st[ST_CURRENTEL] >> 2) & 3) == ((bsp_state[ST_CURRENTEL] >> 2) & 3), "aarch64.ap.el",
          "AP %u at EL%lu, BSP at EL%lu", slot, (st[ST_CURRENTEL] >> 2) & 3,
          (bsp_state[ST_CURRENTEL] >> 2) & 3);
}

void arch_check_late(int base_rev, uint64_t rsdp) {
    (void)base_rev;
    (void)rsdp;
    fact("aarch64.mpidr", "%lx", READ_SYSREG(mpidr_el1) & 0xff00ffffff);
}

void arch_dtb(const void *dtb) {
    (void)dtb;
}
