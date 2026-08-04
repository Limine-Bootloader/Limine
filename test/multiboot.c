#include <e9print.h>
#include <stdint.h>
#include <multiboot1.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2badb002

void multiboot_main(uint32_t magic, struct multiboot1_info *info) {
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        printf("multiboot: Invalid magic: %#x\n", magic);
        goto out;
    }

    printf("Welcome to the multiboot1 test kernel: \n");

    printf("\t flags: %#x\n", info->flags);

    printf("\t mem_lower: %#x\n", info->mem_lower);
    printf("\t mem_upper: %#x\n", info->mem_upper);

    printf("\t boot_device: %#x\n", info->boot_device);
    printf("\t cmdline: %s\n", (char *)(uintptr_t)info->cmdline);

    {
        struct multiboot1_module *start = (struct multiboot1_module *)info->mods_addr;
        struct multiboot1_module *end = start + info->mods_count;

        printf("\t modules:\n");
        for (struct multiboot1_module* entry = start; entry < end; entry++) {
            printf("\t\t begin=%#x\n", entry->begin);
            printf("\t\t end=%#x\n", entry->end);
            printf("\t\t cmdline=%s\n", (char *)(uintptr_t)entry->cmdline);
        }
    }

    {
        struct multiboot1_mmap_entry *start = (struct multiboot1_mmap_entry *)info->mmap_addr;
        struct multiboot1_mmap_entry *end = (struct multiboot1_mmap_entry *)(info->mmap_addr + info->mmap_length);

        printf("\t usable_entries_mmap:\n");

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

            printf("\t\t addr=%#llx\n", entry->addr);
            printf("\t\t length=%#llx\n", entry->len);
            printf("\t\t type=Usable\n");

            // Now this might be a bit confusing since but `entry->size` represents the
            // is the size of the associated structure in bytes and `entry->len` represents the
            // size of the memory region.
            total_mem += entry->len;
        }

        printf("Total usable memory: %#x\n", total_mem);
    }

    // TODO(Andy-Python-Programmer): Drives are unimplemented
    // TODO(Andy-Python-Programmer): ROM config is unimplemented

    printf("\t bootloader_name: %s\n", (char *)(uintptr_t)info->bootloader_name);

    // TODO(Andy-Python-Programmer): APM table is unimplemented
    // TODO(Andy-Python-Programmer): VBE tag is unimplemented

    printf("\t fb_addr: %#llx\n", info->fb_addr);
    printf("\t fb_pitch: %#x\n", info->fb_pitch);
    printf("\t fb_width: %#x\n", info->fb_width);
    printf("\t fb_height: %#x\n", info->fb_height);
    printf("\t fb_bpp: %#x\n", info->fb_bpp);
    printf("\t fb_type: %#x\n", info->fb_type);

    printf("\t fb_red_mask_shift: %#x\n", info->fb_red_mask_shift);
    printf("\t fb_red_mask_size: %#x\n", info->fb_red_mask_size);

    printf("\t fb_green_mask_shift: %#x\n", info->fb_green_mask_shift);
    printf("\t fb_green_mask_size: %#x\n", info->fb_green_mask_size);

    printf("\t fb_blue_mask_shift: %#x\n", info->fb_blue_mask_shift);
    printf("\t fb_blue_mask_size: %#x\n", info->fb_blue_mask_size);

out:
    for (;;);
}
