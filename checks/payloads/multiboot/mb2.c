// A Multiboot 2 kernel that reports, on port 0xe9, the machine state it was
// entered with and every tag of the information structure it was given, then
// marks the framebuffer or the text screen for the harness to find. It runs
// wherever it is put, so it holds no addresses in its data.

#pragma GCC visibility push(hidden)

#include "mbio.h"

extern volatile uint32_t mb_regs[16];
extern volatile uint8_t mb_gdtr[8];
extern volatile uint32_t mb_efi_entry;
extern char __load_start[];

static volatile uint32_t a20_probe = 0x5a5aa5a5;
// Checked before anything writes to it: the loader zeroes the bss.
static volatile uint32_t bss_probe[1024];

static uint32_t rd32(uint64_t a) {
    return *(volatile uint32_t *)(uintptr_t)a;
}

static uint64_t rd64(uint64_t a) {
    return rd32(a) | ((uint64_t)rd32(a + 4) << 32);
}

static void report_descriptor(const char *key, uint32_t sel) {
    uint32_t base = *(volatile uint32_t *)(mb_gdtr + 2);
    uint16_t limit = *(volatile uint16_t *)mb_gdtr;
    if ((sel & 4) != 0 || (sel | 7) > limit) {
        fact(key, ~(uint64_t)0);
        return;
    }
    fact(key, rd64(base + (sel & ~7u)));
}

static void report_state(void) {
    fact("eax", mb_regs[0]);
    fact("ebx", mb_regs[1]);
    fact("eflags", mb_regs[3]);
    fact("cs", mb_regs[4]);
    fact("ds", mb_regs[5]);
    fact("es", mb_regs[6]);
    fact("fs", mb_regs[7]);
    fact("gs", mb_regs[8]);
    fact("ss", mb_regs[9]);
    fact("cr0", mb_regs[10]);
    report_descriptor("gdt.cs", mb_regs[4]);
    report_descriptor("gdt.ds", mb_regs[5]);
    report_descriptor("gdt.es", mb_regs[6]);
    report_descriptor("gdt.fs", mb_regs[7]);
    report_descriptor("gdt.gs", mb_regs[8]);
    report_descriptor("gdt.ss", mb_regs[9]);
    fact("load_base", (uintptr_t)__load_start);
    fact("efi_entry", mb_efi_entry);

    // With A20 off, the probe (bit 20 set where the kernel is linked) and its
    // alias are one location.
    volatile uint32_t *alias = (volatile uint32_t *)((uintptr_t)&a20_probe ^ (uintptr_t)0x100000);
    a20_probe = ~*alias;
    fact("a20", *alias != a20_probe);
    fact("apic_base", rdmsr(0x1b));
}

static void hex_field(uint64_t v) {
    putc_(' ');
    hex(v);
}

// The layout Multiboot2 2.0's multiboot2.h gives it: three u32 fields, then
// the section headers.
static void report_elf_sections(uint32_t tag) {
    uint32_t num = rd32(tag + 8);
    uint32_t entsize = rd32(tag + 12);
    fact("elf.num", num);
    fact("elf.entsize", entsize);
    fact("elf.shndx", rd32(tag + 16));
    for (uint32_t i = 0; i < num && i < 64; i++) {
        uint32_t sh = tag + 20 + i * entsize;
        uint64_t type, flags, addr, size;
        if (entsize >= 64) {
            type = rd32(sh + 4);
            flags = rd64(sh + 8);
            addr = rd64(sh + 16);
            size = rd64(sh + 32);
        } else {
            type = rd32(sh + 4);
            flags = rd32(sh + 8);
            addr = rd32(sh + 12);
            size = rd32(sh + 20);
        }
        puts_("K elf.section=");
        hex(i);
        hex_field(type);
        hex_field(flags);
        hex_field(addr);
        hex_field(size);
        // SHT_NOBITS occupies no file bytes to compare.
        bool has_bytes = type != 8 && addr != 0 && addr < 0x100000000 && size != 0;
        hex_field(has_bytes ? fnv1a((const volatile void *)(uintptr_t)addr, size) : 0);
        putc_('\n');
    }
}

// Four 64x64 squares, red, green, blue and white, from the top left corner.
static void draw_rgb(uint64_t base, uint32_t pitch, uint32_t width, uint32_t height, uint32_t bpp,
                     const volatile uint8_t *colour) {
    if (base >= 0x100000000 || bpp < 15 || width < 256 || height < 64) {
        return;
    }
    uint32_t r = ((1u << colour[1]) - 1) << colour[0];
    uint32_t g = ((1u << colour[3]) - 1) << colour[2];
    uint32_t b = ((1u << colour[5]) - 1) << colour[4];
    uint32_t bytes = (bpp + 7) / 8;
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 256; x++) {
            uint32_t px = x < 64 ? r : x < 128 ? g : x < 192 ? b : r | g | b;
            volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)(base + y * pitch + x * bytes);
            for (uint32_t i = 0; i < bytes; i++) {
                p[i] = px >> (i * 8);
            }
        }
    }
    fact("fb.drawn", 1);
}

static void draw_text(uint64_t base) {
    volatile uint16_t *cell = (volatile uint16_t *)(uintptr_t)base;
    const char *msg = "MB2 TEXT OK";
    for (size_t i = 0; msg[i] != 0; i++) {
        cell[i] = 0x1f00 | (uint8_t)msg[i];
    }
    fact("fb.drawn", 1);
}

static void report_framebuffer(uint32_t tag) {
    uint64_t base = rd64(tag + 8);
    uint32_t pitch = rd32(tag + 16), width = rd32(tag + 20), height = rd32(tag + 24);
    volatile uint8_t *t = (volatile uint8_t *)(uintptr_t)tag;
    fact("fb.addr", base);
    fact("fb.pitch", pitch);
    fact("fb.width", width);
    fact("fb.height", height);
    fact("fb.bpp", t[28]);
    fact("fb.type", t[29]);
    fact("fb.reserved", *(volatile uint16_t *)(t + 30));
    // Colour info at 32, as Multiboot2 2.0's multiboot2.h lays it out.
    fact_hexdump("fb.colour", t + 32, 6);
    if (t[29] == 1) {
        draw_rgb(base, pitch, width, height, t[28], t + 32);
    } else if (t[29] == 2 && base == 0xb8000) {
        draw_text(base);
    }
}

static void report_rsdp(const char *key, uint32_t copy, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len && i < 20; i++) {
        sum += *(volatile uint8_t *)(uintptr_t)(copy + i);
    }
    puts_("K ");
    puts_(key);
    putc_('=');
    hex(rd64(copy));
    hex_field(sum);
    hex_field(len);
    hex_field(len >= 36 ? *(volatile uint8_t *)(uintptr_t)(copy + 15) : 0xff);
    putc_('\n');
}

static void report_tag(uint32_t tag, uint32_t type, uint32_t size) {
    switch (type) {
        case 1: {
            fact_str("cmdline", (const char *)(uintptr_t)(tag + 8), size - 8);
            break;
        }
        case 2: {
            fact_str("boot_loader_name", (const char *)(uintptr_t)(tag + 8), size - 8);
            break;
        }
        case 3: {
            uint32_t start = rd32(tag + 8), end = rd32(tag + 12);
            puts_("K mod=");
            hex(start);
            hex_field(end);
            hex_field(end >= start ? fnv1a((const volatile void *)(uintptr_t)start, end - start) : 0);
            putc_('\n');
            fact_str("mod.string", (const char *)(uintptr_t)(tag + 16), size - 16);
            break;
        }
        case 4: {
            fact("mem_lower", rd32(tag + 8));
            fact("mem_upper", rd32(tag + 12));
            break;
        }
        case 5: {
            puts_("K bootdev=");
            hex(rd32(tag + 8));
            hex_field(rd32(tag + 12));
            hex_field(rd32(tag + 16));
            putc_('\n');
            break;
        }
        case 6: {
            uint32_t entry_size = rd32(tag + 8);
            fact("mmap.entry_size", entry_size);
            fact("mmap.entry_version", rd32(tag + 12));
            for (uint32_t e = tag + 16; entry_size != 0 && e + entry_size <= tag + size; e += entry_size) {
                puts_("K mmap=");
                hex(rd64(e));
                hex_field(rd64(e + 8));
                hex_field(rd32(e + 16));
                hex_field(rd32(e + 20));
                putc_('\n');
            }
            break;
        }
        case 8: {
            report_framebuffer(tag);
            break;
        }
        case 9: {
            report_elf_sections(tag);
            break;
        }
        case 11: {
            uint32_t p = rd32(tag + 8);
            fact("efi32.pointer", p);
            fact("efi32.signature", rd64(p));
            break;
        }
        case 12: {
            uint64_t p = rd64(tag + 8);
            fact("efi64.pointer", p);
            fact("efi64.signature", p < 0x100000000 ? rd64(p) : 0);
            break;
        }
        case 13: {
            volatile uint8_t *t = (volatile uint8_t *)(uintptr_t)tag;
            puts_("K smbios=");
            hex(t[8]);
            hex_field(t[9]);
            hex_field(rd32(tag + 16));
            hex_field(size - 16);
            putc_('\n');
            break;
        }
        case 14: {
            report_rsdp("acpi_old", tag + 8, size - 8);
            break;
        }
        case 15: {
            report_rsdp("acpi_new", tag + 8, size - 8);
            break;
        }
        case 16: {
            fact_hexdump("network", (const volatile void *)(uintptr_t)(tag + 8), size - 8);
            break;
        }
        case 17: {
            fact("efi_mmap.descr_size", rd32(tag + 8));
            fact("efi_mmap.descr_vers", rd32(tag + 12));
            fact("efi_mmap.bytes", size - 16);
            break;
        }
        case 19: {
            fact("efi32_ih", rd32(tag + 8));
            break;
        }
        case 20: {
            fact("efi64_ih", rd64(tag + 8));
            break;
        }
        case 21: {
            fact("load_base_addr", rd32(tag + 8));
            break;
        }
        default: {
            break;
        }
    }
}

void mb2_main(void) {
    bool bss_zero = true;
    for (size_t i = 0; i < sizeof(bss_probe) / sizeof(bss_probe[0]); i++) {
        bss_zero = bss_zero && bss_probe[i] == 0;
    }
    puts_("\nB MB2-TEST\n");
    fact("bss.zero", bss_zero);
    report_state();

    uint32_t mbi = mb_regs[1];
    uint32_t total = rd32(mbi);
    fact("mbi.total_size", total);
    fact("mbi.reserved", rd32(mbi + 4));
    uint32_t tag = mbi + 8;
    for (int n = 0; n < 256 && tag + 8 <= mbi + total; n++) {
        uint32_t type = rd32(tag), size = rd32(tag + 4);
        puts_("K tag=");
        hex(tag - mbi);
        hex_field(type);
        hex_field(size);
        putc_('\n');
        if (type == 0 || size < 8) {
            break;
        }
        report_tag(tag, type, size);
        tag += (size + 7) & ~7u;
    }
    puts_("E DONE\n");
}
