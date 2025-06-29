#ifndef SYS__CPU_H__
#define SYS__CPU_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/misc.h>

#if defined(__x86_64__) || defined(__i386__)

static inline bool cpuid(uint32_t leaf, uint32_t subleaf,
          uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t cpuid_max;
    asm volatile ("cpuid"
                  : "=a" (cpuid_max)
                  : "a" (leaf & 0x80000000)
                  : "ebx", "ecx", "edx");
    if (leaf > cpuid_max)
        return false;
    asm volatile ("cpuid"
                  : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
                  : "a" (leaf), "c" (subleaf));
    return true;
}

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %%al, %1"  : : "a" (value), "Nd" (port) : "memory");
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile ("outw %%ax, %1"  : : "a" (value), "Nd" (port) : "memory");
}

static inline void outd(uint16_t port, uint32_t value) {
    asm volatile ("outl %%eax, %1" : : "a" (value), "Nd" (port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile ("inb %1, %%al"  : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    asm volatile ("inw %1, %%ax"  : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

static inline uint32_t ind(uint16_t port) {
    uint32_t value;
    asm volatile ("inl %1, %%eax" : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

static inline void mmoutb(uintptr_t addr, uint8_t value) {
    asm volatile (
        "movb %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}

static inline void mmoutw(uintptr_t addr, uint16_t value) {
    asm volatile (
        "movw %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}

static inline void mmoutd(uintptr_t addr, uint32_t value) {
    asm volatile (
        "movl %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}

#if defined (__x86_64__)
static inline void mmoutq(uintptr_t addr, uint64_t value) {
    asm volatile (
        "movq %1, (%0)"
        :
        : "r" (addr), "r" (value)
        : "memory"
    );
}
#endif

static inline uint8_t mminb(uintptr_t addr) {
    uint8_t ret;
    asm volatile (
        "movb (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

static inline uint16_t mminw(uintptr_t addr) {
    uint16_t ret;
    asm volatile (
        "movw (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

static inline uint32_t mmind(uintptr_t addr) {
    uint32_t ret;
    asm volatile (
        "movl (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

#if defined (__x86_64__)
static inline uint64_t mminq(uintptr_t addr) {
    uint64_t ret;
    asm volatile (
        "movq (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}
#endif

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t edx, eax;
    asm volatile ("rdmsr"
                  : "=a" (eax), "=d" (edx)
                  : "c" (msr)
                  : "memory");
    return ((uint64_t)edx << 32) | eax;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t edx = value >> 32;
    uint32_t eax = (uint32_t)value;
    asm volatile ("wrmsr"
                  :
                  : "a" (eax), "d" (edx), "c" (msr)
                  : "memory");
}

static inline uint64_t rdtsc(void) {
    uint32_t edx, eax;
    asm volatile ("rdtsc" : "=a" (eax), "=d" (edx));
    return ((uint64_t)edx << 32) | eax;
}

static inline uint64_t tsc_freq_arch(void) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(0x15, 0, &eax, &ebx, &ecx, &edx))
        return 0;
    if (eax == 0 || ebx == 0 || ecx == 0)
        return 0;
    return ecx * ebx / eax;
}

#define rdrand(type) ({ \
    type rdrand__ret; \
    asm volatile ( \
        "1: " \
        "rdrand %0;" \
        "jnc 1b;" \
        : "=r" (rdrand__ret) \
    ); \
    rdrand__ret; \
})

#define rdseed(type) ({ \
    type rdseed__ret; \
    asm volatile ( \
        "1: " \
        "rdseed %0;" \
        "jnc 1b;" \
        : "=r" (rdseed__ret) \
    ); \
    rdseed__ret; \
})

#define write_cr(reg, val) do { \
    asm volatile ("mov %0, %%cr" reg :: "r" (val) : "memory"); \
} while (0)

#define read_cr(reg) ({ \
    size_t read_cr__cr; \
    asm volatile ("mov %%cr" reg ", %0" : "=r" (read_cr__cr) :: "memory"); \
    read_cr__cr; \
})

#define locked_read(var) ({ \
    typeof(*var) locked_read__ret = 0; \
    asm volatile ( \
        "lock xadd %0, %1" \
        : "+r" (locked_read__ret) \
        : "m" (*(var)) \
        : "memory" \
    ); \
    locked_read__ret; \
})

#define locked_write(var, val) do { \
    __auto_type locked_write__ret = val; \
    asm volatile ( \
        "lock xchg %0, %1" \
        : "+r" ((locked_write__ret)) \
        : "m" (*(var)) \
        : "memory" \
    ); \
} while (0)

#elif defined (__aarch64__)

static inline uint64_t rdtsc(void) {
    uint64_t v;
    asm volatile ("mrs %0, cntpct_el0" : "=r" (v));
    return v;
}

static inline uint64_t tsc_freq_arch(void) {
    return 0; // FIXME
}

#define locked_read(var) ({ \
    typeof(*var) locked_read__ret = 0; \
    asm volatile ( \
        "ldar %0, %1" \
        : "=r" (locked_read__ret) \
        : "m" (*(var)) \
        : "memory" \
    ); \
    locked_read__ret; \
})

static inline size_t icache_line_size(void) {
    uint64_t ctr;
    asm volatile ("mrs %0, ctr_el0" : "=r"(ctr));

    return (ctr & 0b1111) << 4;
}

static inline size_t dcache_line_size(void) {
    uint64_t ctr;
    asm volatile ("mrs %0, ctr_el0" : "=r"(ctr));

    return ((ctr >> 16) & 0b1111) << 4;
}

// Clean D-Cache to Point of Coherency
static inline void clean_dcache_poc(uintptr_t start, uintptr_t end) {
    size_t dsz = dcache_line_size();

    uintptr_t addr = start & ~(dsz - 1);
    while (addr < end) {
        asm volatile ("dc cvac, %0" :: "r"(addr) : "memory");
        addr += dsz;
    }

    asm volatile ("dsb sy\n\tisb");
}

// Invalidate I-Cache to Point of Unification
static inline void inval_icache_pou(uintptr_t start, uintptr_t end) {
    size_t isz = icache_line_size();

    uintptr_t addr = start & ~(isz - 1);
    while (addr < end) {
        asm volatile ("ic ivau, %0" :: "r"(addr) : "memory");
        addr += isz;
    }

    asm volatile ("dsb sy\n\tisb");
}

static inline int current_el(void) {
    uint64_t v;

    asm volatile ("mrs %0, currentel" : "=r"(v));
    v = (v >> 2) & 0b11;

    return v;
}

#elif defined (__riscv)

static inline uint64_t rdtsc(void) {
    uint64_t v;
    asm volatile ("rdcycle %0" : "=r"(v));
    return v;
}

static inline uint64_t tsc_freq_arch(void) {
    return 0; // FIXME
}

#define csr_read(csr) ({\
    size_t v;\
    asm volatile ("csrr %0, " csr : "=r"(v));\
    v;\
})

#define csr_write(csr, v) ({\
    size_t old;\
    asm volatile ("csrrw %0, " csr ", %1" : "=r"(old) : "r"(v));\
    old;\
})

#define make_satp(mode, ppn) (((size_t)(mode) << 60) | ((size_t)(ppn) >> 12))

#define locked_read(var) ({ \
    typeof(*var) locked_read__ret; \
    asm volatile ( \
        "ld %0, (%1); fence r, rw" \
        : "=r"(locked_read__ret) \
        : "r"(var) \
        : "memory" \
    ); \
    locked_read__ret; \
})

extern size_t bsp_hartid;

struct riscv_hart {
    struct riscv_hart *next;
    const char *isa_string;
    size_t hartid;
    uint32_t acpi_uid;
    uint8_t mmu_type;
    uint8_t flags;
};

#define RISCV_HART_COPROC  ((uint8_t)1 << 0)  // is a coprocessor
#define RISCV_HART_HAS_MMU ((uint8_t)1 << 1)  // `mmu_type` field is valid

extern struct riscv_hart *hart_list;

bool riscv_check_isa_extension_for(size_t hartid, const char *ext, size_t *maj, size_t *min);

static inline bool riscv_check_isa_extension(const char *ext, size_t *maj, size_t *min) {
    return riscv_check_isa_extension_for(bsp_hartid, ext, maj, min);
}

void init_riscv(const char *config);

#elif defined (__loongarch64)

#define LOONGARCH_CSR_TVAL 0x42

static inline uint64_t rdtsc(void) {
    uint64_t v;
    asm volatile ("csrrd %0, %1" : "=r" (v) : "i" (LOONGARCH_CSR_TVAL));
    return v;
}

static inline uint64_t tsc_freq_arch(void) {
    return 0; // FIXME
}

#else
#error Unknown architecture
#endif

static inline uint64_t tsc_freq(void) {
    uint64_t freq = tsc_freq_arch();
    if (freq != 0) {
        return freq;
    }

#if defined(UEFI)
    uint64_t tsc_start = rdtsc();
    gBS->Stall(1000);
    uint64_t tsc_end = rdtsc();

    if (tsc_end < tsc_start)
        return 0;

    return (tsc_end - tsc_start) * 1000ULL;
#else
    return 0;
#endif
}

static inline void delay(uint64_t cycles) {
    uint64_t next_stop = rdtsc() + cycles;

    while (rdtsc() < next_stop);
}

#endif
