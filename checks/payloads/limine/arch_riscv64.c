#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "common.h"
#include "state.h"
#include "arch.h"
#include "fdt.h"

#define CSR_READ(c) ({ uint64_t v; asm volatile ("csrr %0, " #c : "=r"(v)); v; })

struct sbiret {
    long error;
    long value;
};

static struct sbiret sbi_call(long ext, long fid, long a0, long a1, long a2) {
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    register long r_a6 asm("a6") = fid;
    register long r_a7 asm("a7") = ext;
    asm volatile ("ecall" : "+r"(r_a0), "+r"(r_a1) : "r"(r_a2), "r"(r_a6), "r"(r_a7) : "memory");
    return (struct sbiret){ r_a0, r_a1 };
}

static bool use_dbcn;

void arch_init_output(void) {
    // The debug console extension where SBI has it, else the legacy call.
    struct sbiret r = sbi_call(0x10, 3, 0x4442434e, 0, 0);
    use_dbcn = r.error == 0 && r.value != 0;
}

void arch_out_char(char c) {
    if (use_dbcn) {
        sbi_call(0x4442434e, 2, (uint8_t)c, 0, 0);
    } else {
        sbi_call(1, 0, (uint8_t)c, 0, 0);
    }
}

const char *arch_name(void) {
    return "riscv64";
}

void arch_cpu_relax(void) {
    asm volatile ("nop");
}

uint64_t arch_counter(void) {
    uint64_t v;
    asm volatile ("rdtime %0" : "=r"(v));
    return v;
}

uint64_t arch_counter_hw_freq(void) {
    // Only the device tree states it; the harness compares against that.
    return 0;
}

bool arch_has_nx(void) {
    return true;
}

static int svpbmt = -1;

int arch_cache_normal(void) {
    return 0;
}

int arch_cache_fb(void) {
    // Without Svpbmt nothing can be overridden; with it, NC (1).
    return svpbmt == 1 ? 1 : 0;
}

uint64_t arch_paging_mode(void) {
    uint64_t mode = CSR_READ(satp) >> 60;
    return mode - 8;
}

bool arch_walk(uint64_t va, struct pt_walk *w) {
    if (!have_hhdm) {
        return false;
    }
    uint64_t satp = CSR_READ(satp);
    int levels = (int)(satp >> 60) - 5;
    uint64_t table = (satp & 0xfffffffffff) << 12;
    for (int level = levels - 1; level >= 0; level--) {
        int shift = 12 + 9 * level;
        uint64_t e = ((volatile uint64_t *)(uintptr_t)(hhdm_offset + table))[(va >> shift) & 511];
        if (!(e & 1)) {
            return false;
        }
        uint64_t ppn = (e >> 10) & 0xfffffffffff;
        if (e & 0xe) {
            uint64_t size = (uint64_t)1 << shift;
            w->phys = ((ppn << 12) & ~(size - 1)) + (va & (size - 1));
            w->page_size = size;
            w->writable = (e >> 2) & 1;
            w->executable = (e >> 3) & 1;
            w->cache = (e >> 61) & 3;
            return true;
        }
        table = ppn << 12;
    }
    return false;
}

static const char *abi_names[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
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
        if (i == 2) {
            continue;
        }
        uint64_t want = 0;
        if (i == 5) {
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
    check(st[2] % 16 == 0, NAME(".sp_alignment"), "sp %lx", st[2]);
    check((st[ST_SSTATUS] & 2) == 0, NAME(".sstatus_sie"), "sstatus %lx", st[ST_SSTATUS]);
    check(st[ST_SIE] == 0, NAME(".sie"), "sie %lx", st[ST_SIE]);
    uint64_t satp = st[ST_SATP];
    check((satp >> 60) >= 8 && (satp >> 60) <= 10, NAME(".satp_mode"), "satp %lx", satp);
    if (base_rev >= 6) {
        check(st[ST_STVEC] == 0, NAME(".stvec"), "stvec %lx", st[ST_STVEC]);
        check(st[ST_SSTATUS] == 0x200000000, NAME(".sstatus_exact"), "sstatus %lx", st[ST_SSTATUS]);
        check(((satp >> 44) & 0xffff) == 0, NAME(".satp_asid"), "satp %lx", satp);
    }
#undef NAME
}

void arch_check_entry(int base_rev, uint64_t entry_point) {
    uint64_t *st = bsp_state;
    kprintf("K riscv64.entry=sstatus %lx sie %lx stvec %lx satp %lx sip %lx t0 %lx\n",
            st[ST_SSTATUS], st[ST_SIE], st[ST_STVEC], st[ST_SATP], st[ST_SIP], st[5]);
    extern void _start(void);
    uint64_t want_t0 = entry_point != 0 ? entry_point : (uint64_t)(uintptr_t)_start;
    check_state("riscv64.bsp", st, base_rev, want_t0, -1, 0);
}

void arch_prepare_aps(void) {
}

void arch_ap_hook(unsigned slot) {
    (void)slot;
}

void arch_check_ap(int base_rev, unsigned slot, struct limine_mp_info *info) {
    uint64_t *st = ap_states[slot];
    char who[24] = "riscv64.ap.";
    who[11] = '0' + (slot / 10) % 10;
    who[12] = '0' + slot % 10;
    who[13] = 0;
    extern void ap_entry(struct limine_mp_info *);
    // The AP's t0 holds the address it was sent to, like the BSP's.
    check_state(who, st, base_rev, (uint64_t)(uintptr_t)ap_entry, 10, (uint64_t)(uintptr_t)info);
    check(st[ST_SATP] == bsp_state[ST_SATP], "riscv64.ap.satp", "AP %u satp %lx, BSP %lx", slot,
          st[ST_SATP], bsp_state[ST_SATP]);
}

void arch_check_late(int base_rev, uint64_t rsdp) {
    (void)base_rev;
    (void)rsdp;
}

static bool has_word(const char *list, int len, const char *word) {
    size_t n = strlen(word);
    for (int i = 0; i + (int)n <= len; i++) {
        if (memcmp(list + i, word, n) == 0
         && (i == 0 || list[i - 1] == '_' || list[i - 1] == 0)
         && (list[i + n] == 0 || list[i + n] == '_')) {
            return true;
        }
    }
    return false;
}

void arch_dtb(const void *dtb) {
    int len;
    const char *ext = fdt_getprop_path(dtb, "/cpus/cpu@0", "riscv,isa-extensions", &len);
    if (ext != NULL) {
        svpbmt = has_word(ext, len, "svpbmt");
    } else {
        const char *isa = fdt_getprop_path(dtb, "/cpus/cpu@0", "riscv,isa", &len);
        svpbmt = isa != NULL && has_word(isa, len, "svpbmt");
    }
    fact("riscv64.svpbmt", "%d", svpbmt);
    const void *tb = fdt_getprop_path(dtb, "/cpus", "timebase-frequency", &len);
    if (tb != NULL && len == 4) {
        fact("riscv64.timebase_frequency", "%u", be32(tb));
    }
}
