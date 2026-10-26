// A stand-in for a Linux kernel on aarch64, riscv64 and loongarch64: it
// reports what the bootloader handed over (registers, device tree /chosen, the
// EFI configuration tables) on the UART, then halts. Built to run anywhere
// with the MMU off, so nothing here may need relocating.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// In .data: the entry stores here before .bss is cleared.
__attribute__((section(".data"))) uint64_t stub_regs[16];

extern char __bss_start[], __bss_end[];

static void putc_(char c) {
#if defined (__aarch64__)
    volatile uint32_t *uart = (volatile uint32_t *)0x09000000;
    while (uart[0x18 / 4] & (1 << 5)) {
    }
    uart[0] = (uint8_t)c;
#elif defined (__riscv)
    volatile uint8_t *uart = (volatile uint8_t *)0x10000000;
    while (!(uart[5] & 0x20)) {
    }
    uart[0] = (uint8_t)c;
#elif defined (__loongarch64)
    // Through the uncached window DMW0 provides.
    volatile uint8_t *uart = (volatile uint8_t *)0x800000001fe001e0;
    while (!(uart[5] & 0x20)) {
    }
    uart[0] = (uint8_t)c;
#endif
}

static void puts_(const char *s) {
    while (*s != 0) {
        putc_(*s++);
    }
}

static void hex(uint64_t v) {
    puts_("0x");
    bool started = false;
    for (int i = 60; i >= 0; i -= 4) {
        unsigned d = (v >> i) & 15;
        if (d != 0 || started || i == 0) {
            putc_("0123456789abcdef"[d]);
            started = true;
        }
    }
}

static void escaped(const char *s, size_t max) {
    for (size_t i = 0; i < max && s[i] != 0; i++) {
        uint8_t c = s[i];
        if (c >= 0x20 && c < 0x7f && c != '\\') {
            putc_(c);
        } else {
            puts_("\\x");
            putc_("0123456789abcdef"[c >> 4]);
            putc_("0123456789abcdef"[c & 15]);
        }
    }
}

static void fact_hex(const char *key, uint64_t v) {
    puts_("K ");
    puts_(key);
    putc_('=');
    hex(v);
    putc_('\n');
}

static uint64_t fnv1a(const uint8_t *p, size_t len) {
    uint64_t h = 0xcbf29ce484222325;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3;
    }
    return h;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static bool streq(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

static bool prefix(const char *s, const char *p) {
    while (*p != 0) {
        if (*s++ != *p++) {
            return false;
        }
    }
    return true;
}

static uint64_t initrd_start, initrd_end;

// Every node under the root named memory or memory@..., and every /chosen
// property, as facts.
static void dump_dtb(const uint8_t *dtb) {
    if (be32(dtb) != 0xd00dfeed) {
        puts_("K dtb.magic=bad\n");
        return;
    }
    puts_("K dtb.magic=ok\n");
    const uint8_t *p = dtb + be32(dtb + 8);
    const char *strings = (const char *)dtb + be32(dtb + 12);
    int depth = -1;
    bool in_chosen = false;
    unsigned memory_nodes = 0;
    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == 1) {
            const char *name = (const char *)p;
            depth++;
            if (depth == 1 && (streq(name, "memory") || prefix(name, "memory@"))) {
                memory_nodes++;
            }
            in_chosen = depth == 1 && streq(name, "chosen");
            p += (slen(name) + 1 + 3) & ~(size_t)3;
        } else if (tok == 2) {
            depth--;
            in_chosen = false;
        } else if (tok == 3) {
            uint32_t len = be32(p);
            const char *name = strings + be32(p + 4);
            const uint8_t *val = p + 8;
            if (depth == 0 && streq(name, "limine-test-marker")) {
                puts_("K dtb.marker=");
                escaped((const char *)val, len);
                putc_('\n');
            }
            if (in_chosen) {
                puts_("K chosen.");
                puts_(name);
                putc_('=');
                if (streq(name, "bootargs")) {
                    escaped((const char *)val, len);
                } else if (len == 8) {
                    hex(be64(val));
                } else if (len == 4) {
                    hex(be32(val));
                } else {
                    puts_("len ");
                    hex(len);
                }
                putc_('\n');
                if (streq(name, "linux,initrd-start")) {
                    initrd_start = len == 8 ? be64(val) : be32(val);
                } else if (streq(name, "linux,initrd-end")) {
                    initrd_end = len == 8 ? be64(val) : be32(val);
                }
            }
            p += 8 + ((len + 3) & ~(uint32_t)3);
        } else if (tok == 4) {
            continue;
        } else {
            break;
        }
    }
    fact_hex("dtb.memory_nodes", memory_nodes);
}

struct guid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};

static bool guid_is(const struct guid *g, uint32_t a, uint16_t b, uint16_t c, uint8_t d0, uint8_t d1) {
    return g->a == a && g->b == b && g->c == c && g->d[0] == d0 && g->d[1] == d1;
}

static void put_guid(const struct guid *g) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4) {
        putc_(digits[(g->a >> i) & 15]);
    }
    putc_('-');
    for (int i = 12; i >= 0; i -= 4) {
        putc_(digits[(g->b >> i) & 15]);
    }
    putc_('-');
    for (int i = 12; i >= 0; i -= 4) {
        putc_(digits[(g->c >> i) & 15]);
    }
    putc_('-');
    for (int i = 0; i < 8; i++) {
        if (i == 2) {
            putc_('-');
        }
        putc_(digits[g->d[i] >> 4]);
        putc_(digits[g->d[i] & 15]);
    }
}

#if defined (__loongarch64)
// The device tree's configuration table, which LoongArch kernels read.
static const uint8_t *efi_dtb;
#endif

static void dump_efi(uint64_t systab) {
    const uint8_t *st = (const uint8_t *)(uintptr_t)systab;
    fact_hex("efi.signature", *(const uint64_t *)st);
    uint64_t count = *(const uint64_t *)(st + 104);
    const uint8_t *tables = (const uint8_t *)(uintptr_t)*(const uint64_t *)(st + 112);
    fact_hex("efi.table_count", count);
    for (uint64_t i = 0; i < count; i++) {
        const struct guid *g = (const struct guid *)(tables + i * 24);
        const uint8_t *t = (const uint8_t *)(uintptr_t)*(const uint64_t *)(tables + i * 24 + 16);
        puts_("K efi.table=");
        put_guid(g);
        putc_('\n');
        if (guid_is(g, 0xe03fc20a, 0x85dc, 0x406e, 0xb9, 0x0e)) {
            // struct screen_info: lfb_width at 0x12, lfb_height 0x14,
            // lfb_depth 0x16, lfb_base 0x18, lfb_linelength 0x24,
            // orig_video_isVGA 0x0f.
            fact_hex("screen_info.isvga", t[0x0f]);
            fact_hex("screen_info.width", t[0x12] | (t[0x13] << 8));
            fact_hex("screen_info.height", t[0x14] | (t[0x15] << 8));
            fact_hex("screen_info.depth", t[0x16] | (t[0x17] << 8));
            fact_hex("screen_info.linelength", t[0x24] | (t[0x25] << 8));
            fact_hex("screen_info.base", (uint64_t)(t[0x18] | (t[0x19] << 8) | (t[0x1a] << 16) | ((uint32_t)t[0x1b] << 24))
                                      | ((uint64_t)(t[0x3a] | (t[0x3b] << 8) | (t[0x3c] << 16) | ((uint32_t)t[0x3d] << 24)) << 32));
        } else if (guid_is(g, 0x5568e427, 0x68fc, 0x4f3d, 0xac, 0x74)) {
            uint64_t base = *(const uint64_t *)t, size = *(const uint64_t *)(t + 8);
            fact_hex("initrd_table.base", base);
            fact_hex("initrd_table.size", size);
        } else if (guid_is(g, 0x800f683f, 0xd08b, 0x423a, 0xa2, 0x93)) {
            fact_hex("boot_memmap.map_size", *(const uint64_t *)t);
            fact_hex("boot_memmap.desc_size", *(const uint64_t *)(t + 8));
            fact_hex("boot_memmap.desc_ver", *(const uint32_t *)(t + 16));
            fact_hex("boot_memmap.buff_size", *(const uint64_t *)(t + 32));
            // The descriptors follow at 40; VirtualStart at 16 in each, its
            // Attribute at 32, EFI_MEMORY_RUNTIME the top bit.
            uint64_t map_size = *(const uint64_t *)t, desc_size = *(const uint64_t *)(t + 8);
            uint64_t runtime = 0, unmapped = 0;
            for (uint64_t off = 0; desc_size != 0 && off + desc_size <= map_size; off += desc_size) {
                const uint8_t *d = t + 40 + off;
                if (*(const uint64_t *)(d + 32) >> 63) {
                    runtime++;
                    unmapped += *(const uint64_t *)(d + 16) == ~(uint64_t)0;
                }
            }
            fact_hex("boot_memmap.runtime", runtime);
            fact_hex("boot_memmap.runtime_unmapped", unmapped);
        } else if (guid_is(g, 0x1ce1e5bc, 0x7ceb, 0x42f2, 0x81, 0xe5)) {
            // The random seed table: a 32-bit size, then the seed.
            fact_hex("rng_seed.size", *(const uint32_t *)t);
        } else if (guid_is(g, 0xb7799cb0, 0xeca2, 0x4943, 0x96, 0x67)) {
            // The TPM event log table: sizes and version ahead of the log,
            // whose first event is 32 bytes of header, then its data.
            fact_hex("tpm_log.size", *(const uint32_t *)t);
            fact_hex("tpm_log.final_events_preboot_size", *(const uint32_t *)(t + 4));
            fact_hex("tpm_log.version", t[8]);
            puts_("K tpm_log.first_event=");
            escaped((const char *)t + 9 + 32, 16);
            putc_('\n');
#if defined (__loongarch64)
        } else if (guid_is(g, 0xb1b621d5, 0xf19c, 0x41a5, 0x83, 0x0b)) {
            efi_dtb = t;
#endif
        } else if (guid_is(g, 0x888eb0c6, 0x8ede, 0x4ff5, 0xa8, 0xf0)) {
            fact_hex("memreserve.capacity", *(const uint32_t *)t);
            fact_hex("memreserve.count", *(const uint32_t *)(t + 4));
            fact_hex("memreserve.next", *(const uint64_t *)(t + 8));
        }
    }
}

void stub_main(void) {
    // A bootloader owes the kernel image_size bytes, not zeroed ones.
    for (char *p = __bss_start; p < __bss_end; p++) {
        *(volatile char *)p = 0;
    }
    puts_("\nB LINUX-STUB\n");
#if defined (__aarch64__)
    puts_("K arch=aarch64\n");
    fact_hex("x0", stub_regs[0]);
    fact_hex("x1", stub_regs[1]);
    fact_hex("x2", stub_regs[2]);
    fact_hex("x3", stub_regs[3]);
    fact_hex("el", stub_regs[4] >> 2);
    fact_hex("mmu", stub_regs[5] & 1);
    fact_hex("daif", stub_regs[6]);
    uint64_t dtb = stub_regs[0];
#elif defined (__riscv)
    puts_("K arch=riscv64\n");
    fact_hex("a0", stub_regs[0]);
    fact_hex("a1", stub_regs[1]);
    fact_hex("satp", stub_regs[4]);
    fact_hex("sstatus_sie", (stub_regs[5] >> 1) & 1);
    fact_hex("sie", stub_regs[6]);
    uint64_t dtb = stub_regs[1];
#elif defined (__loongarch64)
    puts_("K arch=loongarch64\n");
    fact_hex("a0", stub_regs[0]);
    fact_hex("a1", stub_regs[1]);
    fact_hex("a2", stub_regs[2]);
    fact_hex("crmd", stub_regs[4]);
    fact_hex("dmw0", stub_regs[5]);
    fact_hex("dmw1", stub_regs[6]);
    fact_hex("dmw2", stub_regs[7]);
    fact_hex("dmw3", stub_regs[8]);
    puts_("K cmdline=");
    escaped((const char *)(uintptr_t)stub_regs[1], 4096);
    putc_('\n');
    uint64_t dtb = 0;
#endif
    fact_hex("image_base", stub_regs[9]);
    if (dtb != 0) {
        dump_dtb((const uint8_t *)(uintptr_t)dtb);
        if (initrd_end > initrd_start) {
            fact_hex("initrd.hash", fnv1a((const uint8_t *)(uintptr_t)initrd_start, initrd_end - initrd_start));
        }
    }
#if defined (__loongarch64)
    uint64_t systab = stub_regs[2];
#else
    uint64_t systab = 0;
    // The DTB names it; the walk above kept nothing of it, so find it again.
    if (dtb != 0) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)dtb;
        const uint8_t *p = d + be32(d + 8);
        const char *strings = (const char *)d + be32(d + 12);
        for (;;) {
            uint32_t tok = be32(p);
            p += 4;
            if (tok == 1) {
                p += (slen((const char *)p) + 1 + 3) & ~(size_t)3;
            } else if (tok == 3) {
                uint32_t len = be32(p);
                if (streq(strings + be32(p + 4), "linux,uefi-system-table")) {
                    systab = be64(p + 8);
                }
                p += 8 + ((len + 3) & ~(uint32_t)3);
            } else if (tok == 2 || tok == 4) {
                continue;
            } else {
                break;
            }
        }
    }
#endif
    if (systab != 0) {
        dump_efi(systab);
    }
#if defined (__loongarch64)
    if (efi_dtb != NULL) {
        dump_dtb(efi_dtb);
    }
#endif
    puts_("E DONE\n");
}
