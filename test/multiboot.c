#include <e9print.h>
#include <stdint.h>
#include <multiboot1.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2badb002

void multiboot_main(uint32_t magic, struct multiboot1_info *info) {
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        e9_printf("multiboot: Invalid magic: %x\n", magic);
        goto out;
    }

    e9_printf("Welcome to the multiboot1 test kernel: ");

    e9_printf("\t flags: %x", info->flags);

    e9_printf("\t mem_lower: %x", info->mem_lower);
    e9_printf("\t mem_upper: %x", info->mem_upper);

    e9_printf("\t boot_device: %x", info->boot_device);
    e9_printf("\t cmdline: %s", info->cmdline);

    {
        struct multiboot1_module *start = (struct multiboot1_module *)info->mods_addr;
        struct multiboot1_module *end = start + info->mods_count;

        e9_printf("\t modules:");
        for (struct multiboot1_module* entry = start; entry < end; entry++) {
            e9_printf("\t\t begin=%x", entry->begin);
            e9_printf("\t\t end=%x", entry->end);
            e9_printf("\t\t cmdline=%s", entry->cmdline);
        }
    }

    {
        struct multiboot1_mmap_entry *start = (struct multiboot1_mmap_entry *)info->mmap_addr;
        struct multiboot1_mmap_entry *end = (struct multiboot1_mmap_entry *)(info->mmap_addr + info->mmap_length);

        e9_printf("\t usable_entries_mmap:");

        size_t total_mem = 0;

        // For now we only print the usable memory map entries since
        // printing the whole memory map blows my terminal up. We also
        // iterate through the available memory map entries and add up
        // to find the total amount of usable memory.
        for (struct multiboot1_mmap_entry* entry = start; entry < end; entry++) {
            // Check if the memory map entry is marked as usable!
            if (entry->type != 1) {
                continue;
            }

            e9_printf("\t\t addr=%x", entry->addr);
            e9_printf("\t\t length=%x", entry->len);
            e9_printf("\t\t type=Usable");

            // Now this might be a bit confusing since but `entry->size` represents the
            // is the size of the associated structure in bytes and `entry->len` represents the
            // size of the memory region.
            total_mem += entry->len;
        }

        e9_printf("Total usable memory: %x", total_mem);
    }

    // TODO(Andy-Python-Programmer): Drives are unimplemented
    // TODO(Andy-Python-Programmer): ROM config is unimplemented

    e9_printf("\t bootloader_name: %s", info->bootloader_name);

    // TODO(Andy-Python-Programmer): APM table is unimplemented
    // TODO(Andy-Python-Programmer): VBE tag is unimplemented

    e9_printf("\t fb_addr: %x", info->fb_addr);
    e9_printf("\t fb_pitch: %x", info->fb_pitch);
    e9_printf("\t fb_width: %x", info->fb_width);
    e9_printf("\t fb_height: %x", info->fb_height);
    e9_printf("\t fb_bpp: %x", info->fb_bpp);
    e9_printf("\t fb_type: %x", info->fb_type);

    e9_printf("\t fb_red_mask_shift: %x", info->fb_red_mask_shift);
    e9_printf("\t fb_red_mask_size: %x", info->fb_red_mask_size);

    e9_printf("\t fb_green_mask_shift: %x", info->fb_green_mask_shift);
    e9_printf("\t fb_green_mask_size: %x", info->fb_green_mask_size);

    e9_printf("\t fb_blue_mask_shift: %x", info->fb_blue_mask_shift);
    e9_printf("\t fb_blue_mask_size: %x", info->fb_blue_mask_size);

out:
    for (;;);
}
