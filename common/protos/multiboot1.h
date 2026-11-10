#ifndef PROTOS__MULTIBOOT1_H
#define PROTOS__MULTIBOOT1_H

#include <stdint.h>
#include <stddef.h>
#include <stdnoreturn.h>

#define MULTIBOOT1_HEADER_MAGIC 0x1BADB002

struct multiboot1_header {
    uint32_t magic;
    uint32_t flags;
    uint32_t checksum;

    uint32_t header_addr;
    uint32_t load_addr;
    uint32_t load_end_addr;
    uint32_t bss_end_addr;
    uint32_t entry_addr;

    uint32_t fb_mode;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_bpp;
};

struct multiboot1_elf_sections {
    uint32_t num;
    uint32_t size;
    uint32_t addr;
    uint32_t shndx;
};

struct multiboot1_info {
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;

    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    struct multiboot1_elf_sections elf_sect;

    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drivers_addr;

    uint32_t rom_config_table;

    uint32_t bootloader_name;

    uint32_t apm_table;

    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t fb_bpp;
    uint8_t fb_type;
    // The colour union that follows is 4-byte aligned in GRUB's header and in
    // the specification's own, so kernels built against either find the colour
    // info at 112, not at the 110 the specification's prose states.
    uint16_t fb_reserved;

    uint8_t fb_red_mask_shift;
    uint8_t fb_red_mask_size;
    uint8_t fb_green_mask_shift;
    uint8_t fb_green_mask_size;
    uint8_t fb_blue_mask_shift;
    uint8_t fb_blue_mask_size;
};

struct multiboot1_module {
    uint32_t begin;
    uint32_t end;
    uint32_t cmdline;
    uint32_t pad;
};

struct multiboot1_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

noreturn void multiboot1_load(char *config, char *cmdline);

#endif
