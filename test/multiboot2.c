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
        printf("multiboot2: Invalid magic: %#x\n", magic);
        goto out;
    }

    printf("Welcome to the multiboot2 test kernel: \n");
    printf("\t size=%d\n", mb_info_addr->size);
    printf("\t reserved=%d\n", mb_info_addr->reserved);

    print("\nTags:\n");

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
                printf("\t cmdline:\n");
                printf("\t\t string=%s\n", cmdline->string);
                break;
            }

            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME: {
                struct multiboot_tag_string *name = (struct multiboot_tag_string *)tag;
                printf("\t bootloader_name:\n");
                printf("\t\t string=%s\n", name->string);
                break;
            }

            case MULTIBOOT_TAG_TYPE_ACPI_OLD: {
                struct multiboot_tag_old_acpi *old_acpi = (struct multiboot_tag_old_acpi *)tag;
                printf("\t acpi_old:\n");
                printf("\t\t rsdp=%s\n", old_acpi->rsdp);
                break;
            }

            case MULTIBOOT_TAG_TYPE_ACPI_NEW: {
                struct multiboot_tag_new_acpi *new_acpi = (struct multiboot_tag_new_acpi *)tag;
                printf("\t acpi_new:\n");
                printf("\t\t rsdp=%s\n", new_acpi->rsdp);
                break;
            }

            case MULTIBOOT_TAG_TYPE_MODULE: {
                struct multiboot_tag_module *module = (struct multiboot_tag_module *)tag;
                printf("\t module:\n");
                printf("\t\t mod_start=%#x\n", module->mod_start);
                printf("\t\t mod_end=%#x\n", module->mod_end);
                printf("\t\t cmdline=%s\n", module->cmdline);
                break;
            }

            case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO: {
                struct multiboot_tag_basic_meminfo *meminfo = (struct multiboot_tag_basic_meminfo *)tag;
                printf("\t basic_meminfo:\n");
                printf("\t\t mem_lower=%#x\n", meminfo->mem_lower);
                printf("\t\t mem_upper=%#x\n", meminfo->mem_upper);
                break;
            }

            // unimplemented(Andy-Python-Programmer): MULTIBOOT_TAG_TYPE_BOOTDEV

            case MULTIBOOT_TAG_TYPE_MMAP: {
                struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
                printf("\t mmap:\n");
                printf("\t\t entry_size=%d\n", mmap->entry_size);
                printf("\t\t entry_version=%d\n", mmap->entry_version);
                printf("\t\t entries:\n");

                struct multiboot_mmap_entry *m = (struct multiboot_mmap_entry *)(mmap->entries);

                size_t entry_count = (mmap->size - sizeof(struct multiboot_tag_mmap)) / sizeof(struct multiboot_mmap_entry);
                printf("\t\t entry count: %d\n", entry_count);

                for (size_t i = 0; i < entry_count; i++) {
                    printf("\t\t\t addr=%#llx\n", m[i].addr);
                    printf("\t\t\t len=%#llx\n", m[i].len);
                    printf("\t\t\t type=%#x\n", m[i].type);
                }

                break;
            }

            case MULTIBOOT_TAG_TYPE_EFI_MMAP: {
                struct multiboot_tag_efi_mmap *mmap = (struct multiboot_tag_efi_mmap *)tag;
                printf("\t efi_mmap:\n");
                printf("\t\t descr_vers=%d\n", mmap->descr_vers);
                printf("\t\t descr_size=%d\n", mmap->descr_size);
                printf("\t\t size=%d\n", mmap->size);
                printf("\t\t entries:\n");

                struct memory_descriptor {
                    uint32_t type;
                    uint32_t pad;
                    uint64_t physical_start;
                    uint64_t virtual_start;
                    uint64_t pages;
                    uint64_t attribute;
                };

                size_t entry_count = (mmap->size - sizeof(struct multiboot_tag_efi_mmap)) / mmap->descr_size;
                printf("\t\t entry count: %d\n", entry_count);

                for (size_t i = 0; i < entry_count; i++) {
                    struct memory_descriptor *m = (struct memory_descriptor *)(mmap->efi_mmap + i * mmap->descr_size);

                    printf("\t\t\t type=%#x\n", m->type);
                    printf("\t\t\t physical_start=%#llx\n", m->physical_start);
                    printf("\t\t\t virtual_start=%#llx\n", m->virtual_start);
                    printf("\t\t\t pages=%#llx\n", m->pages);
                    printf("\t\t\t attribute=%#llx\n", m->attribute);
                }

                break;
            }

            // unimplemented(Andy-Python-Programmer): MULTIBOOT_TAG_TYPE_VBE

            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
                struct multiboot_tag_framebuffer *fb = (struct multiboot_tag_framebuffer *)tag;

                printf("\t framebuffer:\n");
                printf("\t\t framebuffer_pitch: %d\n", fb->common.framebuffer_pitch);
                printf("\t\t framebuffer_width: %d\n", fb->common.framebuffer_width);
                printf("\t\t framebuffer_height: %d\n", fb->common.framebuffer_height);
                printf("\t\t framebuffer_bpp: %d\n", fb->common.framebuffer_bpp);
                printf("\t\t framebuffer_type: %d\n", fb->common.framebuffer_type);
                printf("\t\t framebuffer_address: %#llx\n", fb->common.framebuffer_addr);

                switch (fb->common.framebuffer_type) {
                    case MULTIBOOT_FRAMEBUFFER_TYPE_RGB: {
                        printf("\t\t framebuffer_red_field_position: %#x\n", fb->framebuffer_red_field_position);
                        printf("\t\t framebuffer_red_mask_size: %#x\n", fb->framebuffer_red_mask_size);
                        printf("\t\t framebuffer_green_field_position: %#x\n", fb->framebuffer_green_field_position);
                        printf("\t\t framebuffer_green_mask_size: %#x\n", fb->framebuffer_green_mask_size);
                        printf("\t\t framebuffer_blue_field_position: %#x\n", fb->framebuffer_blue_field_position);
                        printf("\t\t framebuffer_blue_mask_size: %#x\n", fb->framebuffer_blue_mask_size);
                        break;
                    }

                    // Rest are unimplemented(Andy-Python-Programmer):
                }

                break;
            }

            case MULTIBOOT_TAG_TYPE_NETWORK: {
                struct multiboot_tag_network *network = (struct multiboot_tag_network *)tag;
                printf("\t network tag exists\n");
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
