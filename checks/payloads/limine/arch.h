#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

struct pt_walk {
    uint64_t phys;
    uint64_t page_size;
    bool writable;
    bool executable;
    // x86: PAT index; aarch64: MAIR attribute index; riscv64: PBMT;
    // loongarch64: MAT.
    int cache;
};

// Sets up the report channel. Must not depend on any response.
void arch_init_output(void);
void arch_out_char(char c);

const char *arch_name(void);

// Walks the active page tables. Needs the HHDM to read them.
bool arch_walk(uint64_t va, struct pt_walk *w);
// Cache attribute the protocol prescribes for ordinary memory and framebuffers.
int arch_cache_normal(void);
int arch_cache_fb(void);
// Whether this processor can mark pages non-executable at all.
bool arch_has_nx(void);

// The paging mode in force, in the protocol's numbering.
uint64_t arch_paging_mode(void);

// Checks of the machine state the BSP was entered with.
void arch_check_entry(int base_rev, uint64_t entry_point);
// Run on the BSP before any AP is released, and by each AP as it arrives.
void arch_prepare_aps(void);
void arch_ap_hook(unsigned slot);
// Checks of an AP's state once it has run, against the BSP's.
void arch_check_ap(int base_rev, unsigned slot, struct limine_mp_info *info);
// Further hardware state checks, after all else (x86: PIC, APIC, IOAPIC).
void arch_check_late(int base_rev, uint64_t rsdp);

// Called with the DTB the executable got, if any, before the mapping checks.
void arch_dtb(const void *dtb);

uint64_t arch_counter(void);
// The counter's frequency as the hardware states it, or 0 where it does not.
uint64_t arch_counter_hw_freq(void);

void arch_cpu_relax(void);

#endif
