// A stand-in for an x86 Linux kernel: reports, on port 0xe9, the registers it
// was entered with and what the zero page (struct boot_params) holds. Built
// twice: 32-bit for the 32-bit boot protocol, 64-bit and position independent
// for the 64-bit one, which may load it anywhere.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Register snapshot the entry code takes: eax ebx ecx edx esi edi ebp esp,
// eflags, cs ds es fs gs ss, cr0, cr4, efer, gdtr base, gdtr limit, own base.
uint64_t x86_regs[24] __attribute__((section(".data")));

static void putc_(char c) {
    asm volatile ("outb %0, $0xe9" :: "a"(c));
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

static void fact(const char *k, uint64_t v) {
    puts_("K ");
    puts_(k);
    putc_('=');
    hex(v);
    putc_('\n');
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

static uint64_t fnv1a(const volatile uint8_t *p, uint64_t len) {
    uint64_t h = 0xcbf29ce484222325;
    for (uint64_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3;
    }
    return h;
}

static uint8_t rd8(uint64_t a) {
    return *(volatile uint8_t *)(uintptr_t)a;
}

static uint16_t rd16(uint64_t a) {
    return rd8(a) | (rd8(a + 1) << 8);
}

static uint32_t rd32(uint64_t a) {
    return rd16(a) | ((uint32_t)rd16(a + 2) << 16);
}

static uint64_t rd64(uint64_t a) {
    return rd32(a) | ((uint64_t)rd32(a + 4) << 32);
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return lo | ((uint64_t)hi << 32);
}

// The configuration tables of the EFI system table efi_info names, in its
// 32-bit form ("EL32") or its 64-bit one ("EL64"): each GUID, what the
// kernel's TPM event log table holds (its first event names the log format),
// and the size of its random seed table.
static void dump_efi_tables(uint64_t bp) {
    bool is64 = rd32(bp + 0x1c0) == 0x34364c45;
    uint64_t systab = rd32(bp + 0x1c4) | ((uint64_t)rd32(bp + 0x1d8) << 32);
    if (systab == 0) {
        return;
    }
    uint64_t count = is64 ? rd64(systab + 104) : rd32(systab + 64);
    uint64_t tables = is64 ? rd64(systab + 112) : rd32(systab + 68);
    uint64_t stride = is64 ? 24 : 20;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t g = tables + i * stride;
        uint64_t t = is64 ? rd64(g + 16) : rd32(g + 16);
        puts_("K efi.table=");
        hex(rd32(g));
        putc_(' ');
        hex(rd16(g + 4));
        putc_(' ');
        hex(rd16(g + 6));
        putc_('\n');
        if (rd32(g) == 0xb7799cb0 && rd16(g + 4) == 0xeca2 && rd16(g + 6) == 0x4943) {
            fact("tpm_log.size", rd32(t));
            fact("tpm_log.final_events_preboot_size", rd32(t + 4));
            fact("tpm_log.version", rd8(t + 8));
            puts_("K tpm_log.first_event=");
            escaped((const char *)(uintptr_t)(t + 9 + 32), 16);
            putc_('\n');
        } else if (rd32(g) == 0x1ce1e5bc && rd16(g + 4) == 0x7ceb && rd16(g + 6) == 0x42f2) {
            // struct linux_efi_random_seed: a 32-bit size, then the seed.
            fact("rng_seed.size", rd32(t));
        }
    }
}

#if defined (__x86_64__)
// Where va maps to, or ~0 if it does not. 4-level or 5-level paging, with
// large pages at the PDPT and PD levels (Intel SDM 325462-093 vol. 3A,
// tables 5-15 to 5-20).
static uint64_t translate(uint64_t va) {
    uint64_t cr3, cr4;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    int levels = (cr4 & (1 << 12)) ? 5 : 4;
    uint64_t table = cr3 & 0x000ffffffffff000;
    for (int level = levels; level >= 1; level--) {
        int shift = 12 + 9 * (level - 1);
        uint64_t e = rd64(table + ((va >> shift) & 511) * 8);
        if (!(e & 1)) {
            return ~(uint64_t)0;
        }
        if (level == 1 || ((level == 2 || level == 3) && (e & 0x80))) {
            uint64_t mask = ((uint64_t)1 << shift) - 1;
            return ((e & 0x000ffffffffff000) & ~mask) | (va & mask);
        }
        table = e & 0x000ffffffffff000;
    }
    return ~(uint64_t)0;
}

// The first page of [base, base + len) not identity mapped, or ~0 if none.
static uint64_t first_unmapped(uint64_t base, uint64_t len) {
    for (uint64_t va = base & ~(uint64_t)0xfff; va < base + len; va += 0x1000) {
        if (translate(va) != va) {
            return va;
        }
    }
    return ~(uint64_t)0;
}
#endif

void stub_main(void) {
    puts_("\nB LINUX-X86-STUB\n");
#if defined (__x86_64__)
    puts_("K mode=64\n");
#else
    puts_("K mode=32\n");
#endif
    fact("reg.ebx", x86_regs[1]);
    fact("reg.esi", x86_regs[4]);
    fact("reg.edi", x86_regs[5]);
    fact("reg.ebp", x86_regs[6]);
    fact("reg.eflags", x86_regs[8]);
    fact("reg.cs", x86_regs[9]);
    fact("reg.ds", x86_regs[10]);
    fact("reg.es", x86_regs[11]);
    fact("reg.fs", x86_regs[12]);
    fact("reg.gs", x86_regs[13]);
    fact("reg.ss", x86_regs[14]);
    fact("reg.cr0", x86_regs[15]);
    fact("reg.cr4", x86_regs[16]);
    fact("reg.efer", x86_regs[17]);
    fact("load_address", x86_regs[20]);
    fact("apic_base", rdmsr(0x1b));

    uint64_t gdt = x86_regs[18];
    for (int sel = 0x10; sel <= 0x18; sel += 8) {
        puts_(sel == 0x10 ? "K gdt.10=" : "K gdt.18=");
        hex(rd64(gdt + sel));
        putc_('\n');
    }

    uint64_t bp = x86_regs[4];
    // Zero page offsets, Linux Documentation/arch/x86/zero-page.rst.
    fact("bp.sentinel", rd8(bp + 0x1ef));
    fact("bp.type_of_loader", rd8(bp + 0x210));
    fact("bp.code32_start", rd32(bp + 0x214));
    fact("bp.kernel_alignment", rd32(bp + 0x230));
    fact("bp.relocatable_kernel", rd8(bp + 0x234));
    fact("bp.pref_address", rd64(bp + 0x258));
    fact("bp.init_size", rd32(bp + 0x260));
    fact("bp.loadflags", rd8(bp + 0x211));
    fact("bp.vid_mode", rd16(bp + 0x1fa));
    fact("bp.version", rd16(bp + 0x206));
    uint64_t cmdline = rd32(bp + 0x228) | ((uint64_t)rd32(bp + 0x0c8) << 32);
    fact("bp.cmd_line_ptr", cmdline);
    uint64_t cmdline_len = 0;
    if (cmdline != 0) {
        puts_("K cmdline=");
        escaped((const char *)(uintptr_t)cmdline, 4096);
        putc_('\n');
        while (rd8(cmdline + cmdline_len) != 0) {
            cmdline_len++;
        }
    }
#if defined (__x86_64__)
    // The 64-bit protocol identity maps the init_size window from the load
    // address, the zero page, and the command line.
    fact("idmap.kernel", first_unmapped(x86_regs[20], rd32(bp + 0x260)));
    fact("idmap.zero_page", first_unmapped(bp, 4096));
    fact("idmap.cmdline", first_unmapped(cmdline, cmdline_len + 1));
#endif
    uint64_t rd_image = rd32(bp + 0x218) | ((uint64_t)rd32(bp + 0x0c0) << 32);
    uint64_t rd_size = rd32(bp + 0x21c) | ((uint64_t)rd32(bp + 0x0c4) << 32);
    fact("bp.ramdisk_image", rd_image);
    fact("bp.ramdisk_size", rd_size);
    if (rd_size != 0) {
        fact("ramdisk.hash", fnv1a((const volatile uint8_t *)(uintptr_t)rd_image, rd_size));
    }
    fact("bp.initrd_addr_max", rd32(bp + 0x22c));
    fact("bp.acpi_rsdp_addr", rd64(bp + 0x070));
    uint64_t rsdp = rd64(bp + 0x070);
    if (rsdp != 0) {
        fact("rsdp.signature", rd64(rsdp));
    }

    // screen_info at 0.
    fact("si.isvga", rd8(bp + 0x0f));
    fact("si.width", rd16(bp + 0x12));
    fact("si.height", rd16(bp + 0x14));
    fact("si.depth", rd16(bp + 0x16));
    fact("si.base", rd32(bp + 0x18) | ((uint64_t)rd32(bp + 0x3a) << 32));
    fact("si.size", rd32(bp + 0x1c));
    fact("si.linelength", rd16(bp + 0x24));
    fact("si.red", rd8(bp + 0x26) | (rd8(bp + 0x27) << 8));
    fact("si.green", rd8(bp + 0x28) | (rd8(bp + 0x29) << 8));
    fact("si.blue", rd8(bp + 0x2a) | (rd8(bp + 0x2b) << 8));
    fact("si.capabilities", rd32(bp + 0x36));
    fact("si.flags", rd8(bp + 0x08));
    fact("si.orig_video_mode", rd8(bp + 0x06));
    fact("si.orig_video_cols", rd8(bp + 0x07));
    fact("si.orig_video_lines", rd8(bp + 0x0e));
    fact("edid.hash", fnv1a((const volatile uint8_t *)(uintptr_t)(bp + 0x140), 128));

    // efi_info at 0x1c0.
    fact("efi.loader_signature", rd32(bp + 0x1c0));
    fact("efi.systab", rd32(bp + 0x1c4) | ((uint64_t)rd32(bp + 0x1d8) << 32));
    fact("efi.memdesc_size", rd32(bp + 0x1c8));
    fact("efi.memdesc_version", rd32(bp + 0x1cc));
    fact("efi.memmap", rd32(bp + 0x1d0) | ((uint64_t)rd32(bp + 0x1dc) << 32));
    fact("efi.memmap_size", rd32(bp + 0x1d4));
    fact("bp.secure_boot", rd8(bp + 0x1ec));

    unsigned n = rd8(bp + 0x1e8);
    fact("e820.count", n);
    for (unsigned i = 0; i < n; i++) {
        uint64_t e = bp + 0x2d0 + i * 20;
        puts_("K e820=");
        hex(rd64(e));
        putc_(' ');
        hex(rd64(e + 8));
        putc_(' ');
        hex(rd32(e + 16));
        putc_('\n');
    }
    uint64_t sd = rd64(bp + 0x250);
    while (sd != 0) {
        uint32_t type = rd32(sd + 8), len = rd32(sd + 12);
        puts_("K setup_data=");
        hex(type);
        putc_(' ');
        hex(len);
        putc_('\n');
        if (type == 1) {
            for (uint32_t off = 0; off + 20 <= len; off += 20) {
                uint64_t e = sd + 16 + off;
                puts_("K e820=");
                hex(rd64(e));
                putc_(' ');
                hex(rd64(e + 8));
                putc_(' ');
                hex(rd32(e + 16));
                putc_('\n');
            }
        }
        sd = rd64(sd);
    }
    dump_efi_tables(bp);
    puts_("E DONE\n");
}
