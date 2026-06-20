#include <e9print.h>
#include <stdint.h>
#include <multiboot2.h>

struct multiboot_info {
    uint32_t size;
    uint32_t reserved;
    struct multiboot_tag *first;
};

void multiboot2_main(uint32_t magic, struct multiboot_info* mb_info_addr) {
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        e9_printf("multiboot2: Invalid magic: %x\n", magic);
        goto out;
    }

    e9_printf("Welcome to the multiboot2 test kernel: ");
    e9_printf("\t size=%d", mb_info_addr->size);
    e9_printf("\t reserved=%d", mb_info_addr->reserved);

    e9_print("\nTags:\n");

    size_t add_size = 0;

    // NOTE: We set i to 8 to skip size and reserved fields:
    for (size_t i = 8; i < mb_info_addr->size; i += add_size) {
        struct multiboot_tag *tag = (struct multiboot_tag *)((uint8_t *)mb_info_addr + i);

        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            break;
        }

        switch (tag->type) {
            case MULTIBOOT_TAG_TYPE_CMDLINE: {
                struct multiboot_tag_string *cmdline = (struct multiboot_tag_string *)tag;
                e9_printf("\t cmdline:");
                e9_printf("\t\t string=%s", cmdline->string);
                break;
            }

            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME: {
                struct multiboot_tag_string *name = (struct multiboot_tag_string *)tag;
                e9_printf("\t bootloader_name:");
                e9_printf("\t\t string=%s", name->string);
                break;
            }

            case MULTIBOOT_TAG_TYPE_ACPI_OLD: {
                struct multiboot_tag_old_acpi *old_acpi = (struct multiboot_tag_old_acpi *)tag;
                e9_printf("\t acpi_old:");
                e9_printf("\t\t rsdp=%s", old_acpi->rsdp);
                break;
            }

            case MULTIBOOT_TAG_TYPE_ACPI_NEW: {
                struct multiboot_tag_new_acpi *new_acpi = (struct multiboot_tag_new_acpi *)tag;
                e9_printf("\t acpi_new:");
                e9_printf("\t\t rsdp=%s", new_acpi->rsdp);
                break;
            }

            case MULTIBOOT_TAG_TYPE_MODULE: {
                struct multiboot_tag_module *module = (struct multiboot_tag_module *)tag;
                e9_printf("\t module:");
                e9_printf("\t\t mod_start=%x", module->mod_start);
                e9_printf("\t\t mod_end=%x", module->mod_end);
                e9_printf("\t\t cmdline=%s", module->cmdline);
                break;
            }

            case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO: {
                struct multiboot_tag_basic_meminfo *meminfo = (struct multiboot_tag_basic_meminfo *)tag;
                e9_printf("\t basic_meminfo:");
                e9_printf("\t\t mem_lower=%x", meminfo->mem_lower);
                e9_printf("\t\t mem_upper=%x", meminfo->mem_upper);
                break;
            }

            // unimplemented(Andy-Python-Programmer): MULTIBOOT_TAG_TYPE_BOOTDEV

            case MULTIBOOT_TAG_TYPE_MMAP: {
                struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
                e9_printf("\t mmap:");
                e9_printf("\t\t entry_size=%d", mmap->entry_size);
                e9_printf("\t\t entry_version=%d", mmap->entry_version);
                e9_printf("\t\t entries:");

                struct multiboot_mmap_entry *m = (struct multiboot_mmap_entry *)(mmap->entries);

                size_t entry_count = (mmap->size - sizeof(struct multiboot_tag_mmap)) / sizeof(struct multiboot_mmap_entry);
                e9_printf("\t\t entry count: %d", entry_count);

                for (size_t i = 0; i < entry_count; i++) {
                    e9_printf("\t\t\t addr=%x", m[i].addr);
                    e9_printf("\t\t\t len=%x", m[i].len);
                    e9_printf("\t\t\t type=%x", m[i].type);
                }

                break;
            }

            case MULTIBOOT_TAG_TYPE_EFI_MMAP: {
                struct multiboot_tag_efi_mmap *mmap = (struct multiboot_tag_efi_mmap *)tag;
                e9_printf("\t efi_mmap:");
                e9_printf("\t\t descr_vers=%d", mmap->descr_vers);
                e9_printf("\t\t descr_size=%d", mmap->descr_size);
                e9_printf("\t\t size=%d", mmap->size);
                e9_printf("\t\t entries:");

                struct memory_descriptor {
                    uint32_t type;
                    uint32_t pad;
                    uint64_t physical_start;
                    uint64_t virtual_start;
                    uint64_t pages;
                    uint64_t attribute;
                };

                size_t entry_count = (mmap->size - sizeof(struct multiboot_tag_efi_mmap)) / mmap->descr_size;
                e9_printf("\t\t entry count: %d", entry_count);

                for (size_t i = 0; i < entry_count; i++) {
                    struct memory_descriptor *m = (struct memory_descriptor *)(mmap->efi_mmap + i * mmap->descr_size);

                    e9_printf("\t\t\t type=%x", m->type);
                    e9_printf("\t\t\t physical_start=%x", m->physical_start);
                    e9_printf("\t\t\t virtual_start=%x", m->virtual_start);
                    e9_printf("\t\t\t pages=%x", m->pages);
                    e9_printf("\t\t\t attribute=%x", m->attribute);
                }

                break;
            }

            // unimplemented(Andy-Python-Programmer): MULTIBOOT_TAG_TYPE_VBE

            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
                struct multiboot_tag_framebuffer *fb = (struct multiboot_tag_framebuffer *)tag;

                e9_printf("\t framebuffer:");
                e9_printf("\t\t framebuffer_pitch: %d", fb->common.framebuffer_pitch);
                e9_printf("\t\t framebuffer_width: %d", fb->common.framebuffer_width);
                e9_printf("\t\t framebuffer_height: %d", fb->common.framebuffer_height);
                e9_printf("\t\t framebuffer_bpp: %d", fb->common.framebuffer_bpp);
                e9_printf("\t\t framebuffer_type: %d", fb->common.framebuffer_type);
                e9_printf("\t\t framebuffer_address: %x", fb->common.framebuffer_addr);

                switch (fb->common.framebuffer_type) {
                    case MULTIBOOT_FRAMEBUFFER_TYPE_RGB: {
                        e9_printf("\t\t framebuffer_red_field_position: %x", fb->framebuffer_red_field_position);
                        e9_printf("\t\t framebuffer_red_mask_size: %x", fb->framebuffer_red_mask_size);
                        e9_printf("\t\t framebuffer_green_field_position: %x", fb->framebuffer_green_field_position);
                        e9_printf("\t\t framebuffer_green_mask_size: %x", fb->framebuffer_green_mask_size);
                        e9_printf("\t\t framebuffer_blue_field_position: %x", fb->framebuffer_blue_field_position);
                        e9_printf("\t\t framebuffer_blue_mask_size: %x", fb->framebuffer_blue_mask_size);
                        break;
                    }

                    // Rest are unimplemented(Andy-Python-Programmer):
                }

                break;
            }

            case MULTIBOOT_TAG_TYPE_NETWORK: {
                struct multiboot_tag_network *network = (struct multiboot_tag_network *)tag;
                e9_printf("\t network tag exists");
                break;
            }
        }

        add_size = tag->size;

        // Align the size to 8 bytes.
        if ((add_size % 8) != 0)
            add_size += (8 - add_size % 8);
    }

out:
    for (;;);
}
