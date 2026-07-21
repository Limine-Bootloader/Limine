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

void stage3_common(void);

#if defined (UEFI)
noreturn void uefi_entry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gRT = SystemTable->RuntimeServices;
    efi_image_handle = ImageHandle;

    calibrate_tsc();
    usec_at_bootloader_entry = rdtsc_usec();

    EFI_STATUS status;

    gST->ConOut->EnableCursor(gST->ConOut, false);

    init_memmap();

    term_fallback();

    status = gBS->SetWatchdogTimer(0, 0x10000, 0, NULL);
    if (status) {
        print("WARNING: Failed to disable watchdog timer!\n");
    }

#if defined (__x86_64__) || defined (__i386__)
    init_gdt();
#endif
#if defined (__x86_64__)
    // Stage the below-4GiB handoff stub while allocations are still permitted.
    prepare_spinup_tramp();
#endif

    disk_create_index();

    // Detect UEFI Secure Boot
    {
        EFI_GUID global_variable = EFI_GLOBAL_VARIABLE;
        UINT8 secure_boot = 0;
        UINTN sb_size = sizeof(secure_boot);
        EFI_STATUS sb_status = gRT->GetVariable(L"SecureBoot", &global_variable, NULL, &sb_size, &secure_boot);
        if (sb_status == EFI_SUCCESS && secure_boot == 1) {
            UINT8 setup_mode = 0;
            UINTN sm_size = sizeof(setup_mode);
            EFI_STATUS sm_status = gRT->GetVariable(L"SetupMode", &global_variable, NULL, &sm_size, &setup_mode);
            if (sm_status != EFI_SUCCESS || setup_mode == 0) {
                secure_boot_active = true;
            }
        }
    }

    tpm_init();

    boot_volume = NULL;

    EFI_HANDLE current_handle = ImageHandle;
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
#if defined (UEFI)
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

        EFI_GUID loaded_img_prot_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;

        status = gBS->HandleProtocol(current_handle, &loaded_img_prot_guid,
                                     (void **)&loaded_image);

        if (status) {
            goto could_not_match;
        }

        boot_volume = disk_volume_from_efi_handle(loaded_image->DeviceHandle);

        if (boot_volume != NULL) {
            stage3_common();
        }

        current_handle = loaded_image->ParentHandle;
    }

    goto could_not_match;
}
#endif

noreturn void stage3_common(void) {
#if defined (__x86_64__) || defined (__i386__)
    init_flush_irqs();
    init_io_apics();
#endif

#if defined (__riscv)
#if defined (UEFI)
    RISCV_EFI_BOOT_PROTOCOL *rv_proto = get_riscv_boot_protocol();
    if (rv_proto == NULL || rv_proto->GetBootHartId(rv_proto, &bsp_hartid) != EFI_SUCCESS) {
        panic(false, "failed to get BSP's hartid");
    }
#else
#error riscv: only UEFI is supported
#endif
#endif

    term_notready();

#if defined (UEFI)
    init_bli();
#endif

    menu(true);
}
