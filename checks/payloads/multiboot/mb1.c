// A Multiboot 1 kernel that reports, on port 0xe9, the machine state it was
// entered with and the information structure it was given, then marks the
// framebuffer or the text screen for the harness to find.

#include "mbio.h"

extern volatile uint32_t mb_regs[16];
extern volatile uint8_t mb_gdtr[8], mb_idtr[8];

#if defined (PIC_TEST)
extern volatile uint32_t mb_irq_vector, mb_irq_count;
void mb_idt_install(void);
#endif

static volatile uint32_t a20_probe = 0x5a5aa5a5;
// Checked before anything writes to it: the loader zeroes the bss.
static volatile uint32_t bss_probe[1024];

static uint32_t rd32(uint32_t a) {
    return *(volatile uint32_t *)(uintptr_t)a;
}

static uint64_t rd64(uint32_t a) {
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
    fact("gdtr.base", *(volatile uint32_t *)(mb_gdtr + 2));
    fact("gdtr.limit", *(volatile uint16_t *)mb_gdtr);
    static const char *const names[] = { "gdt.cs", "gdt.ds", "gdt.es", "gdt.fs", "gdt.gs", "gdt.ss" };
    for (int i = 0; i < 6; i++) {
        report_descriptor(names[i], mb_regs[4 + i]);
    }

    // With A20 off, the probe (bit 20 set) and its alias are one location.
    volatile uint32_t *alias = (volatile uint32_t *)((uintptr_t)&a20_probe & ~(uintptr_t)0x100000);
    a20_probe = ~*alias;
    fact("a20", *alias != a20_probe);
    fact("apic_base", rdmsr(0x1b));
}

static void report_elf_sections(const volatile uint8_t *mbi) {
    uint32_t num = *(volatile uint32_t *)(mbi + 28);
    uint32_t size = *(volatile uint32_t *)(mbi + 32);
    uint32_t addr = *(volatile uint32_t *)(mbi + 36);
    fact("elf.num", num);
    fact("elf.size", size);
    fact("elf.addr", addr);
    fact("elf.shndx", *(volatile uint32_t *)(mbi + 40));
    for (uint32_t i = 0; i < num && i < 64; i++) {
        uint32_t sh = addr + i * size;
        uint64_t type, flags, sh_addr, sh_size;
        if (size >= 64) {
            type = rd32(sh + 4);
            flags = rd64(sh + 8);
            sh_addr = rd64(sh + 16);
            sh_size = rd64(sh + 32);
        } else {
            type = rd32(sh + 4);
            flags = rd32(sh + 8);
            sh_addr = rd32(sh + 12);
            sh_size = rd32(sh + 20);
        }
        puts_("K elf.section=");
        hex(i);
        putc_(' ');
        hex(type);
        putc_(' ');
        hex(flags);
        putc_(' ');
        hex(sh_addr);
        putc_(' ');
        hex(sh_size);
        putc_(' ');
        // SHT_NOBITS occupies no file bytes to compare.
        bool has_bytes = type != 8 && sh_addr != 0 && sh_addr < 0x100000000 && sh_size != 0;
        hex(has_bytes ? fnv1a((const volatile void *)(uintptr_t)sh_addr, sh_size) : 0);
        putc_('\n');
    }
}

static void report_modules(const volatile uint8_t *mbi) {
    uint32_t count = *(volatile uint32_t *)(mbi + 20);
    uint32_t addr = *(volatile uint32_t *)(mbi + 24);
    fact("mods.count", count);
    fact("mods.addr", addr);
    for (uint32_t i = 0; i < count && i < 32; i++) {
        uint32_t m = addr + i * 16;
        uint32_t start = rd32(m), end = rd32(m + 4), string = rd32(m + 8);
        puts_("K mod=");
        hex(start);
        putc_(' ');
        hex(end);
        putc_(' ');
        hex(rd32(m + 12));
        putc_(' ');
        hex(end >= start ? fnv1a((const volatile void *)(uintptr_t)start, end - start) : 0);
        putc_('\n');
        if (string != 0) {
            fact_str("mod.string", (const char *)(uintptr_t)string, 256);
        } else {
            fact("mod.string_ptr", 0);
        }
        fact("mod.string_addr", string);
    }
}

static void report_mmap(const volatile uint8_t *mbi) {
    uint32_t length = *(volatile uint32_t *)(mbi + 44);
    uint32_t addr = *(volatile uint32_t *)(mbi + 48);
    fact("mmap.length", length);
    fact("mmap.addr", addr);
    for (uint32_t off = 0; off < length;) {
        uint32_t e = addr + off;
        uint32_t size = rd32(e);
        puts_("K mmap=");
        hex(size);
        putc_(' ');
        hex(rd64(e + 4));
        putc_(' ');
        hex(rd64(e + 12));
        putc_(' ');
        hex(rd32(e + 20));
        putc_('\n');
        off += size + 4;
        if (size == 0) {
            break;
        }
    }
}

// Four 64x64 squares, red, green, blue and white, from the top left corner.
static void draw_rgb(uint64_t base, uint32_t pitch, uint32_t width, uint32_t height, uint32_t bpp,
                     const volatile uint8_t *colour) {
    if (base >= 0x100000000 || bpp < 15 || width < 256 || height < 64) {
        return;
    }
    uint8_t rpos = colour[0], rsize = colour[1], gpos = colour[2], gsize = colour[3];
    uint8_t bpos = colour[4], bsize = colour[5];
    uint32_t r = ((1u << rsize) - 1) << rpos, g = ((1u << gsize) - 1) << gpos;
    uint32_t b = ((1u << bsize) - 1) << bpos;
    uint32_t colours[4] = { r, g, b, r | g | b };
    uint32_t bytes = (bpp + 7) / 8;
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 256; x++) {
            uint32_t px = colours[x / 64];
            volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)(base + y * pitch + x * bytes);
            for (uint32_t i = 0; i < bytes; i++) {
                p[i] = px >> (i * 8);
            }
        }
    }
    fact("fb.drawn", 1);
}

static void draw_text(uint64_t base) {
    static const char msg[] = "MB1 TEXT OK";
    volatile uint16_t *cell = (volatile uint16_t *)(uintptr_t)base;
    for (size_t i = 0; msg[i] != 0; i++) {
        cell[i] = 0x1f00 | (uint8_t)msg[i];
    }
    fact("fb.drawn", 1);
}

static void report_mbi(const volatile uint8_t *mbi) {
    uint32_t flags = *(volatile uint32_t *)mbi;
    fact_hexdump("mbi.raw", mbi, 120);
    fact("mbi.flags", flags);
    if (flags & (1 << 0)) {
        fact("mbi.mem_lower", *(volatile uint32_t *)(mbi + 4));
        fact("mbi.mem_upper", *(volatile uint32_t *)(mbi + 8));
    }
    if (flags & (1 << 1)) {
        fact("mbi.boot_device", *(volatile uint32_t *)(mbi + 12));
    }
    if (flags & (1 << 2)) {
        uint32_t cmdline = *(volatile uint32_t *)(mbi + 16);
        fact("mbi.cmdline_addr", cmdline);
        fact_str("mbi.cmdline", (const char *)(uintptr_t)cmdline, 4096);
    }
    if (flags & (1 << 3)) {
        report_modules(mbi);
    }
    if (flags & (1 << 5)) {
        report_elf_sections(mbi);
    }
    if (flags & (1 << 6)) {
        report_mmap(mbi);
    }
    if (flags & (1 << 9)) {
        fact_str("mbi.boot_loader_name", (const char *)(uintptr_t)*(volatile uint32_t *)(mbi + 64), 256);
    }
    if (flags & (1 << 12)) {
        uint64_t base = *(volatile uint32_t *)(mbi + 88) | ((uint64_t)*(volatile uint32_t *)(mbi + 92) << 32);
        uint32_t pitch = *(volatile uint32_t *)(mbi + 96);
        uint32_t width = *(volatile uint32_t *)(mbi + 100);
        uint32_t height = *(volatile uint32_t *)(mbi + 104);
        uint8_t bpp = mbi[108], type = mbi[109];
        fact("fb.addr", base);
        fact("fb.pitch", pitch);
        fact("fb.width", width);
        fact("fb.height", height);
        fact("fb.bpp", bpp);
        fact("fb.type", type);
        // Colour info at 112, as Multiboot 0.6.96's multiboot.h lays it out.
        fact_hexdump("fb.colour", mbi + 112, 6);
        if (type == 1) {
            draw_rgb(base, pitch, width, height, bpp, mbi + 112);
        } else if (type == 2 && base == 0xb8000) {
            draw_text(base);
        }
    }
}

#if defined (PIC_TEST)
static void wait_irq(uint32_t before) {
    for (uint32_t i = 0; i < 0x10000000 && mb_irq_count == before; i++) {
        asm volatile ("pause");
    }
}

// Where IRQ0 (the PIT) and IRQ8 (the RTC, through the cascade) arrive.
static void report_pic(void) {
    fact("pic.imr_master", inb(0x21));
    fact("pic.imr_slave", inb(0xa1));
    mb_idt_install();

    outb(0xa1, 0xff);
    outb(0x21, 0xfe);
    uint32_t before = mb_irq_count;
    asm volatile ("sti");
    wait_irq(before);
    asm volatile ("cli");
    fact("pic.irq0_vector", mb_irq_vector);

    outb(0x21, 0xff);
    outb(0x70, 0x0b);
    uint8_t reg_b = inb(0x71);
    outb(0x70, 0x0b);
    outb(0x71, reg_b | 0x40);
    outb(0x70, 0x0c);
    (void)inb(0x71);
    mb_irq_vector = 0xffffffff;
    outb(0xa1, 0xfe);
    outb(0x21, 0xfb);
    before = mb_irq_count;
    asm volatile ("sti");
    wait_irq(before);
    asm volatile ("cli");
    fact("pic.irq8_vector", mb_irq_vector);
    outb(0x70, 0x0b);
    outb(0x71, reg_b);
    outb(0xa1, 0xff);
    outb(0x21, 0xff);
}
#endif

void mb1_main(void) {
    bool bss_zero = true;
    for (size_t i = 0; i < sizeof(bss_probe) / sizeof(bss_probe[0]); i++) {
        bss_zero = bss_zero && bss_probe[i] == 0;
    }
    puts_("\nB MB1-TEST\n");
    fact("bss.zero", bss_zero);
    fact("bss.probe", (uintptr_t)bss_probe);
    report_state();
    report_mbi((const volatile uint8_t *)(uintptr_t)mb_regs[1]);
#if defined (PIC_TEST)
    report_pic();
#endif
    puts_("E DONE\n");
}
