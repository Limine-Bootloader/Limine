#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "common.h"
#include "state.h"
#include "arch.h"

#define CSR_READ(c) ({ uint64_t v; asm volatile ("csrrd %0, " #c : "=r"(v)); v; })
#define CSR_WRITE(c, v) asm volatile ("csrwr %0, " #c :: "r"((uint64_t)(v)) : "memory")

static uint32_t cpucfg(uint32_t word) {
    uint32_t v;
    asm volatile ("cpucfg %0, %1" : "=r"(v) : "r"(word));
    return v;
}

// QEMU virt's 16550, reached through a strongly-ordered uncached window that
// DMW1, which the protocol leaves clear, is set up for here.
#define UART_PHYS 0x1fe001e0
#define DMW1_WINDOW 0x8000000000000000
static volatile uint8_t *uart;

void arch_init_output(void) {
    CSR_WRITE(0x181, DMW1_WINDOW | 1);
    uart = (volatile uint8_t *)(uintptr_t)(DMW1_WINDOW | UART_PHYS);
}

void arch_out_char(char c) {
    if (uart == NULL) {
        return;
    }
    while (!(uart[5] & 0x20)) {
    }
    uart[0] = (uint8_t)c;
}

const char *arch_name(void) {
    return "loongarch64";
}

void arch_cpu_relax(void) {
    asm volatile ("nop");
}

uint64_t arch_counter(void) {
    uint64_t v, id;
    asm volatile ("rdtime.d %0, %1" : "=r"(v), "=r"(id));
    return v;
}

uint64_t arch_counter_hw_freq(void) {
    uint32_t base = cpucfg(4);
    uint32_t mul = cpucfg(5) & 0xffff;
    uint32_t div = cpucfg(5) >> 16;
    if (base == 0 || mul == 0 || div == 0) {
        return 0;
    }
    return (uint64_t)base * mul / div;
}

bool arch_has_nx(void) {
    return (cpucfg(1) >> 22) & 1;
}

int arch_cache_normal(void) {
    return 1;
}

int arch_cache_fb(void) {
    return 2;
}

uint64_t arch_paging_mode(void) {
    return 0;
}

bool arch_walk(uint64_t va, struct pt_walk *w) {
    if (!have_hhdm) {
        return false;
    }
    uint64_t table = (va >> 63) ? CSR_READ(0x1a) : CSR_READ(0x19);
    table &= 0x0000fffffffff000;
    for (int level = 3; level >= 0; level--) {
        int shift = 12 + 9 * level;
        uint64_t e = ((volatile uint64_t *)(uintptr_t)(hhdm_offset + table))[(va >> shift) & 511];
        bool leaf = level == 0 || (e & 0x41) == 0x41;
        if (!leaf) {
            if (e == 0) {
                return false;
            }
            table = e & 0x0000fffffffff000;
            continue;
        }
        if (!(e & 1)) {
            return false;
        }
        uint64_t size = (uint64_t)1 << shift;
        w->phys = (e & 0x0000fffffffff000 & ~(size - 1)) + (va & (size - 1));
        w->page_size = size;
        w->writable = (e >> 8) & 1;
        w->executable = !((e >> 62) & 1);
        w->cache = (e >> 4) & 3;
        return true;
    }
    return false;
}

static const char *abi_names[32] = {
    "zero", "ra", "tp", "sp", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "t0", "t1", "t2", "t3",
    "t4", "t5", "t6", "t7", "t8", "r21", "fp", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8"
};

static void check_state(const char *who, uint64_t *st, int base_rev, uint64_t t0_want, int a0_reg,
                        uint64_t a0_want) {
    char name[48];
    size_t n = 0;
    while (who[n] != 0) {
        name[n] = who[n];
        n++;
    }
#define NAME(S) (memcpy(name + n, S, sizeof(S)), name)
    bool ok = true;
    for (int i = 1; i < 32; i++) {
        if (i == 3) {
            continue;
        }
        uint64_t want = 0;
        if (i == 12) {
            want = t0_want;
        }
        if (i == a0_reg) {
            want = a0_want;
        }
        if (st[i] != want) {
            ok = false;
            kprintf("C FAIL %s.gpr: %s=%lx, want %lx\n", who, abi_names[i], st[i], want);
        }
    }
    check(ok, NAME(".gprs"), "see above");
    check(st[3] % 16 == 0, NAME(".sp_alignment"), "sp %lx", st[3]);
    uint64_t crmd = st[ST_CRMD];
    check((crmd & 0x1f) == 0x10, NAME(".crmd_required"), "CRMD %lx", crmd);
    check(st[ST_TLBRENTRY] != 0, NAME(".tlbrentry"), "TLBRENTRY is 0");
    check(st[ST_PGDL] != 0 || st[ST_PGDH] != 0, NAME(".pgd"), "PGDL %lx PGDH %lx", st[ST_PGDL],
          st[ST_PGDH]);
    if (base_rev >= 6) {
        check(crmd == 0xb0, NAME(".crmd_exact"), "CRMD %lx, want b0", crmd);
        check(st[ST_EUEN] == 0, NAME(".euen"), "EUEN %lx", st[ST_EUEN]);
        check(st[ST_ECFG] == 0, NAME(".ecfg"), "ECFG %lx", st[ST_ECFG]);
        check(st[ST_EENTRY] == 0, NAME(".eentry"), "EENTRY %lx", st[ST_EENTRY]);
        check(st[ST_MERRENTRY] == 0, NAME(".merrentry"), "MERRENTRY %lx", st[ST_MERRENTRY]);
        check(st[ST_DMW0] == 0x11, NAME(".dmw0"), "DMW0 %lx", st[ST_DMW0]);
        check(st[ST_DMW1] == 0 && st[ST_DMW2] == 0 && st[ST_DMW3] == 0, NAME(".dmw123"),
              "DMW1 %lx DMW2 %lx DMW3 %lx", st[ST_DMW1], st[ST_DMW2], st[ST_DMW3]);
        uint64_t pwcl = st[ST_PWCL], pwch = st[ST_PWCH];
        bool pw = (pwcl & 0x1f) == 12 && ((pwcl >> 5) & 0x1f) == 9
               && ((pwcl >> 10) & 0x1f) == 21 && ((pwcl >> 15) & 0x1f) == 9
               && ((pwcl >> 20) & 0x1f) == 30 && ((pwcl >> 25) & 0x1f) == 9
               && ((pwcl >> 30) & 3) == 0
               && (pwch & 0x3f) == 39 && ((pwch >> 6) & 0x3f) == 9 && ((pwch >> 18) & 0x3f) == 0;
        check(pw, NAME(".pwcl_pwch"), "PWCL %lx PWCH %lx", pwcl, pwch);
        check((st[ST_STLBPS] & 0x3f) == 12, NAME(".stlbps"), "STLBPS %lx", st[ST_STLBPS]);
    }
#undef NAME
}

void arch_check_entry(int base_rev, uint64_t entry_point) {
    uint64_t *st = bsp_state;
    kprintf("K loongarch64.entry=crmd %lx prmd %lx euen %lx ecfg %lx eentry %lx merrentry %lx tlbrentry %lx\n",
            st[ST_CRMD], st[ST_PRMD], st[ST_EUEN], st[ST_ECFG], st[ST_EENTRY], st[ST_MERRENTRY],
            st[ST_TLBRENTRY]);
    kprintf("K loongarch64.paging=pgdl %lx pgdh %lx pwcl %lx pwch %lx stlbps %lx\n", st[ST_PGDL],
            st[ST_PGDH], st[ST_PWCL], st[ST_PWCH], st[ST_STLBPS]);
    kprintf("K loongarch64.dmw=%lx %lx %lx %lx\n", st[ST_DMW0], st[ST_DMW1], st[ST_DMW2], st[ST_DMW3]);
    extern void _start(void);
    uint64_t want_t0 = entry_point != 0 ? entry_point : (uint64_t)(uintptr_t)_start;
    check_state("loongarch64.bsp", st, base_rev, want_t0, -1, 0);
}

void arch_prepare_aps(void) {
}

void arch_ap_hook(unsigned slot) {
    (void)slot;
}

void arch_check_ap(int base_rev, unsigned slot, struct limine_mp_info *info) {
    uint64_t *st = ap_states[slot];
    char who[24] = "loongarch64.ap.";
    who[15] = '0' + (slot / 10) % 10;
    who[16] = '0' + slot % 10;
    who[17] = 0;
    extern void ap_entry(struct limine_mp_info *);
    check_state(who, st, base_rev, (uint64_t)(uintptr_t)ap_entry, 4, (uint64_t)(uintptr_t)info);
    check(st[ST_PGDH] == bsp_state[ST_PGDH], "loongarch64.ap.pgdh", "AP %u PGDH %lx, BSP %lx", slot,
          st[ST_PGDH], bsp_state[ST_PGDH]);
}

void arch_check_late(int base_rev, uint64_t rsdp) {
    (void)base_rev;
    (void)rsdp;
}

void arch_dtb(const void *dtb) {
    (void)dtb;
}
