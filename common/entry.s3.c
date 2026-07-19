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
extern symbol __slide, __image_base, __image_end;
extern symbol _start;

#if defined (__x86_64__)
// Some firmware boots Limine chainloaded from Limine but not directly, so
// prefer letting the firmware load a fresh copy of us over relocating
// ourselves behind its back. Returns only if that could not be arranged.
static void uefi_reload_self(void) {
    EFI_GUID loaded_img_dp_prot_guid = EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *self_path;

    if (gBS->HandleProtocol(efi_image_handle, &loaded_img_dp_prot_guid,
                            (void **)&self_path) != 0) {
        return;
    }

    EFI_HANDLE new_handle = NULL;

    if (gBS->LoadImage(false, efi_image_handle, self_path,
                       NULL, 0, &new_handle) != 0) {
        // An image rejected by platform policy is still loaded and still ours.
        if (new_handle != NULL) {
            gBS->UnloadImage(new_handle);
        }
        return;
    }

    EFI_GUID loaded_img_prot_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *new_image;

    if (gBS->HandleProtocol(new_handle, &loaded_img_prot_guid,
                            (void **)&new_image) == 0) {
        // Nothing stops the firmware from placing the copy as high as it
        // placed us, and starting it then would recurse until memory ran out.
        if ((uintptr_t)new_image->ImageBase + new_image->ImageSize <= 0x100000000) {
            UINTN exit_data_size = 0;
            CHAR16 *exit_data = NULL;

            EFI_STATUS exit_status = gBS->StartImage(new_handle,
                                                     &exit_data_size, &exit_data);

            gBS->Exit(efi_image_handle, exit_status, exit_data_size, exit_data);
        }
    }

    gBS->UnloadImage(new_handle);
}
#endif

noreturn void uefi_entry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gRT = SystemTable->RuntimeServices;
    efi_image_handle = ImageHandle;

    calibrate_tsc();
    usec_at_bootloader_entry = rdtsc_usec();

    EFI_STATUS status;

    const char *deferred_error = NULL;

#if defined (__x86_64__)
    if ((uintptr_t)__slide >= 0x100000000) {
        uefi_reload_self();

        size_t image_size = ALIGN_UP((uintptr_t)__image_end - (uintptr_t)__image_base, 4096, panic(false, "Alignment overflow"));
        size_t image_size_pages = image_size / 4096;

        // Probing page by page is up to a million AllocatePages() calls, and
        // this runs before the terminal exists and before the watchdog is
        // disabled: minutes of blank screen, then a firmware reset.
        EFI_PHYSICAL_ADDRESS _new_base = 0xffffffff;
        status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderCode,
                                    image_size_pages, &_new_base);
        if (status != 0) {
            deferred_error = "Limine does not support being loaded above 4GiB and no alternative loading spot found";
            goto defer_error;
        }

        size_t new_base = (size_t)_new_base;

        memcpy((void *)new_base, __slide, (size_t)image_size);
        __attribute__((ms_abi))
        void (*new_entry_point)(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
        new_entry_point = (void *)(new_base + ((uintptr_t)_start - (uintptr_t)__slide));
        new_entry_point(ImageHandle, SystemTable);
        __builtin_unreachable();
    }

defer_error:
#endif

    gST->ConOut->EnableCursor(gST->ConOut, false);

    init_memmap();

    term_fallback();

    status = gBS->SetWatchdogTimer(0, 0x10000, 0, NULL);
    if (status) {
        print("WARNING: Failed to disable watchdog timer!\n");
    }

    if (deferred_error != NULL) {
        panic(false, "%s", deferred_error);
    }

#if defined (__x86_64__) || defined (__i386__)
    init_gdt();
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
