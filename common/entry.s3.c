#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>
#include <lib/term.h>
#include <lib/real.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/part.h>
#include <lib/config.h>
#include <lib/trace.h>
#include <lib/bli.h>
#include <lib/tpm.h>
#include <sys/e820.h>
#include <sys/a20.h>
#include <sys/idt.h>
#include <sys/gdt.h>
#include <lib/print.h>
#include <fs/file.h>
#include <lib/elf.h>
#include <mm/pmm.h>
#include <menu.h>
#include <pxe/pxe.h>
#include <pxe/tftp.h>
#include <drivers/disk.h>
#include <sys/lapic.h>
#include <lib/getchar.h>
#include <sys/cpu.h>
#include <libfdt.h>

void stage3_common(void);

#if defined(UEFI)
noreturn void uefi_entry(efi_handle image_handle, struct efi_system_table *system_table) {
    gST = system_table;
    gBS = system_table->boot_services;
    gRT = system_table->runtime_services;
    efi_image_handle = image_handle;

    reseed_stack_guard();

    calibrate_tsc();
    usec_at_bootloader_entry = rdtsc_usec();

    efi_status status;

    gST->con_out->enable_cursor(gST->con_out, false);

    init_memmap();

    term_fallback();

    status = gBS->set_watchdog_timer(0, 0x10000, 0, NULL);
    if (status) {
        print("WARNING: Failed to disable watchdog timer!\n");
    }

#if defined(__x86_64__) || defined(__i386__)
    init_gdt();
#endif
#if defined(__x86_64__)
    // Stage the below-4GiB handoff stub while allocations are still permitted.
    prepare_spinup_tramp();
#endif

    disk_create_index();

    efi_init_secure_boot();
    tpm_init();

    boot_volume = NULL;

    efi_handle current_handle = image_handle;
    for (size_t j = 0; j < 25; j++) {
        if (current_handle == NULL) {
could_not_match:
            print("WARNING: Could not meaningfully match the boot device handle with a volume.\n");
            print("         Using the first volume containing a Limine configuration!\n");
            print("\n");
            print("THIS IS A BUG! Please report this issue upstream.\n");
            print("Press any key to continue...\n");
            for (;;) {
                int ret = pit_sleep_and_quit_on_keypress(65535);
                if (ret != 0) {
                    break;
                }
            }

            for (size_t i = 0; i < volume_index_i; i++) {
                struct file_handle *f;

                bool old_cif = case_insensitive_fopen;
                case_insensitive_fopen = true;
                if (
                 false
#if defined(UEFI)
                 || (f = fopen(volume_index[i], "/EFI/limine/limine.conf")) != NULL
                 || (f = fopen(volume_index[i], "/EFI/BOOT/limine.conf")) != NULL
#endif
                 || (f = fopen(volume_index[i], "/boot/limine/limine.conf")) != NULL
                 || (f = fopen(volume_index[i], "/boot/limine.conf")) != NULL
                 || (f = fopen(volume_index[i], "/limine/limine.conf")) != NULL
                 || (f = fopen(volume_index[i], "/limine.conf")) != NULL
                ) {
                    goto opened;
                }

                case_insensitive_fopen = old_cif;
                continue;

opened:
                case_insensitive_fopen = old_cif;

                fclose(f);

                if (volume_index[i]->backing_dev != NULL) {
                    boot_volume = volume_index[i]->backing_dev;
                } else {
                    boot_volume = volume_index[i];
                }

                break;
            }

            if (boot_volume != NULL) {
                stage3_common();
            }

            panic(false, "No volume contained a Limine configuration file");
        }

        struct efi_guid loaded_img_prot_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        struct efi_loaded_image_protocol *loaded_image = NULL;

        status = gBS->handle_protocol(current_handle, &loaded_img_prot_guid,
                                     (void **)&loaded_image);

        if (status) {
            goto could_not_match;
        }

        boot_volume = disk_volume_from_efi_handle(loaded_image->device_handle);

        if (boot_volume != NULL) {
            stage3_common();
        }

        current_handle = loaded_image->parent_handle;
    }

    goto could_not_match;
}
#endif

#if defined(__riscv) && defined(UEFI)
// Firmware predating RISCV_EFI_BOOT_PROTOCOL passes the boot hart in the
// device tree it publishes instead.
static bool get_dtb_boot_hartid(size_t *hartid) {
    struct efi_guid dtb_guid = EFI_DTB_TABLE_GUID;
    void *dtb = NULL;

    for (size_t i = 0; i < gST->number_of_table_entries; i++) {
        struct efi_configuration_table *table = &gST->configuration_table[i];
        if (memcmp(&table->vendor_guid, &dtb_guid, sizeof(struct efi_guid)) == 0) {
            dtb = table->vendor_table;
            break;
        }
    }

    if (dtb == NULL || fdt_check_header(dtb) != 0) {
        return false;
    }

    int chosen = fdt_path_offset(dtb, "/chosen");
    if (chosen < 0) {
        return false;
    }

    int len;
    const void *prop = fdt_getprop(dtb, chosen, "boot-hartid", &len);
    if (prop == NULL) {
        return false;
    }

    if (len == 8) {
        *hartid = fdt64_ld(prop);
    } else if (len == 4) {
        *hartid = fdt32_ld(prop);
    } else {
        return false;
    }

    return true;
}
#endif

noreturn void stage3_common(void) {
#if defined(__x86_64__) || defined(__i386__)
    init_flush_irqs();
    init_io_apics();
#endif

#if defined(__riscv)
#if defined(UEFI)
    struct efi_riscv_efi_boot_protocol *rv_proto = get_riscv_boot_protocol();
    if (rv_proto == NULL || rv_proto->get_boot_hart_id(rv_proto, &bsp_hartid) != EFI_SUCCESS) {
        if (!get_dtb_boot_hartid(&bsp_hartid)) {
            panic(false, "failed to get BSP's hartid");
        }
    }
#else
#error riscv: only UEFI is supported
#endif
#endif

    term_notready();

#if defined(UEFI)
    init_bli();
#endif

    menu(true);
}
