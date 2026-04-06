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
static void apple_set_os(void) {
    if (gST->FirmwareVendor == NULL) {
        return;
    }

    static const CHAR16 apple[] = L"Apple";
    for (size_t i = 0; i < SIZEOF_ARRAY(apple) - 1; i++) {
        if (gST->FirmwareVendor[i] != apple[i]) {
            return;
        }
    }

    EFI_GUID apple_set_os_guid = {
        0xc5c5da95, 0x7d5c, 0x45e6,
        { 0xb2, 0xf1, 0x3f, 0xd5, 0x2b, 0xb1, 0x00, 0x77 }
    };

    struct apple_set_os_iface {
        uint64_t version;
        EFI_STATUS (EFIAPI *set_os_version)(const char *version);
        EFI_STATUS (EFIAPI *set_os_vendor)(const char *vendor);
    } *set_os = NULL;

    EFI_STATUS status = gBS->LocateProtocol(
        &apple_set_os_guid, NULL, (void **)&set_os);
    if (status || set_os == NULL) {
        return;
    }

    if (set_os->version >= 2) {
        set_os->set_os_vendor("Apple Inc.");
    }
    if (set_os->version > 0) {
        set_os->set_os_version("Mac OS X 10.9");
    }

    printv("apple: Set OS version via Apple set_os protocol (version %u)\n",
           (uint32_t)set_os->version);
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
        size_t image_size = ALIGN_UP((uintptr_t)__image_end - (uintptr_t)__image_base, 4096, panic(false, "Alignment overflow"));
        size_t image_size_pages = ALIGN_UP((size_t)image_size, 4096, panic(false, "Alignment overflow")) / 4096;
        size_t new_base;
        for (new_base = 0x1000; new_base + (size_t)image_size < 0x100000000; new_base += 0x1000) {
            EFI_PHYSICAL_ADDRESS _new_base = (EFI_PHYSICAL_ADDRESS)new_base;
            status = gBS->AllocatePages(AllocateAddress, EfiLoaderCode, image_size_pages, &_new_base);
            if (status == 0) {
                goto new_base_gotten;
            }
        }
        deferred_error = "Limine does not support being loaded above 4GiB and no alternative loading spot found";
        goto defer_error;
new_base_gotten:
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

#if defined (__x86_64__)
    apple_set_os();
#endif

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
