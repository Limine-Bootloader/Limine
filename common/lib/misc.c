#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/trace.h>
#include <lib/real.h>
#include <lib/config.h>
#include <lib/uri.h>
#include <lib/bli.h>
#include <lib/rng_seed.h>
#include <lib/term.h>
#include <lib/tpm.h>
#include <fs/file.h>
#include <mm/pmm.h>
#include <libfdt.h>

#if defined (UEFI)
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_HANDLE efi_image_handle;
EFI_MEMORY_DESCRIPTOR *efi_mmap = NULL;
UINTN efi_mmap_size = 0, efi_desc_size = 0, efi_mmap_key = 0;
UINT32 efi_desc_ver = 0;
#endif

#if defined (UEFI) && defined (__x86_64__)
// The handoff drops to 32-bit protected mode with paging off, so the code that
// does it (spinup_go32 and the *_spinup_32 routines) and its stack must be
// below 4GiB wherever the firmware loaded us. Copy them there.
extern symbol spinup_go32, spinup_go32_end;
extern symbol limine_spinup_32, limine_spinup_32_end;
extern symbol linux_spinup, linux_spinup_end;
extern symbol multiboot_spinup_32, multiboot_spinup_32_end;

// Consumed by common_spinup.
uintptr_t spinup_low_go32 = 0;
uintptr_t spinup_low_stack_top = 0;

#define SPINUP_TRAMP_STACK_SIZE 4096

static struct {
    void *hi;
    void *lo;
} spinup_relocs[3];
static size_t spinup_relocs_n = 0;

static uint8_t *spinup_tramp_buf = NULL;
static size_t spinup_tramp_off = 0;

static void *spinup_stow(symbol hi_start, symbol hi_end) {
    size_t size = (uintptr_t)hi_end - (uintptr_t)hi_start;
    void *lo = spinup_tramp_buf + spinup_tramp_off;
    memcpy(lo, hi_start, size);
    spinup_tramp_off = ALIGN_UP(spinup_tramp_off + size, 16,
                                panic(false, "spinup: trampoline overflow"));
    return lo;
}

void prepare_spinup_tramp(void) {
    if (spinup_low_stack_top != 0) {
        return;
    }

    size_t total =
        ALIGN_UP((uintptr_t)spinup_go32_end - (uintptr_t)spinup_go32, 16,
                 panic(false, "spinup: trampoline overflow")) +
        ALIGN_UP((uintptr_t)limine_spinup_32_end - (uintptr_t)limine_spinup_32, 16,
                 panic(false, "spinup: trampoline overflow")) +
        ALIGN_UP((uintptr_t)linux_spinup_end - (uintptr_t)linux_spinup, 16,
                 panic(false, "spinup: trampoline overflow")) +
        ALIGN_UP((uintptr_t)multiboot_spinup_32_end - (uintptr_t)multiboot_spinup_32, 16,
                 panic(false, "spinup: trampoline overflow")) +
        SPINUP_TRAMP_STACK_SIZE;

    spinup_tramp_buf = ext_mem_alloc(total);
    spinup_tramp_off = 0;

    // spinup_go32 runs from here in compatibility mode, before it turns paging
    // off, so the firmware page tables still govern it. An EfiLoaderCode
    // allocation is not on its own a promise that the pages are executable.
    EFI_GUID mem_attr_guid = EFI_MEMORY_ATTRIBUTE_PROTOCOL_GUID;
    EFI_MEMORY_ATTRIBUTE_PROTOCOL *mem_attr = NULL;
    if (gBS->LocateProtocol(&mem_attr_guid, NULL, (void **)&mem_attr) == EFI_SUCCESS) {
        mem_attr->ClearMemoryAttributes(mem_attr,
            (EFI_PHYSICAL_ADDRESS)(uintptr_t)spinup_tramp_buf,
            ALIGN_UP(total, 4096, panic(false, "spinup: trampoline overflow")),
            EFI_MEMORY_XP);
    }

    spinup_low_go32 = (uintptr_t)spinup_stow(spinup_go32, spinup_go32_end);

    spinup_relocs[spinup_relocs_n].hi = limine_spinup_32;
    spinup_relocs[spinup_relocs_n].lo = spinup_stow(limine_spinup_32, limine_spinup_32_end);
    spinup_relocs_n++;

    spinup_relocs[spinup_relocs_n].hi = linux_spinup;
    spinup_relocs[spinup_relocs_n].lo = spinup_stow(linux_spinup, linux_spinup_end);
    spinup_relocs_n++;

    spinup_relocs[spinup_relocs_n].hi = multiboot_spinup_32;
    spinup_relocs[spinup_relocs_n].lo = spinup_stow(multiboot_spinup_32, multiboot_spinup_32_end);
    spinup_relocs_n++;

    // Scratch stack grows down from the tail of the buffer.
    spinup_low_stack_top = ALIGN_DOWN((uintptr_t)spinup_tramp_buf + total, 16);
}

void *spinup_tramp_low(void *hi) {
    for (size_t i = 0; i < spinup_relocs_n; i++) {
        if (spinup_relocs[i].hi == hi) {
            return spinup_relocs[i].lo;
        }
    }
    panic(false, "spinup: request for an unknown trampoline routine");
}
#endif

bool editor_enabled = true;
bool help_hidden = false;
bool secure_boot_active = false;

uint64_t usec_at_bootloader_entry;

#if defined (UEFI)
bool is_efi_serial_present(void) {
    EFI_STATUS status;
    EFI_SERIAL_IO_PROTOCOL *serial_io = NULL;
    EFI_GUID serial_io_guid = EFI_SERIAL_IO_PROTOCOL_GUID;

    status = gBS->LocateProtocol(&serial_io_guid, NULL, (void **)&serial_io);
    if (status) {
        return false;
    }

    if (serial_io == NULL) {
        return false;
    }

    UINT32 control;
    status = serial_io->GetControl(serial_io, &control);
    if (status) {
        return false;
    }

    return true;
}
#endif

bool parse_resolution(size_t *width, size_t *height, size_t *bpp, const char *buf) {
    size_t res[3] = {0};

    const char *first = buf;
    for (size_t i = 0; i < 3; i++) {
        const char *last;
        size_t x = strtoui(first, &last, 10);
        if (first == last)
            break;
        res[i] = x;
        if (*last == 0)
            break;
        first = last + 1;
    }

    if (res[0] == 0 || res[1] == 0)
        return false;

    if (res[2] == 0)
        res[2] = 32;

    *width = res[0], *height = res[1];
    if (bpp != NULL)
        *bpp = res[2];

    return true;
}

// This integer sqrt implementation has been adapted from:
// https://stackoverflow.com/questions/1100090/looking-for-an-efficient-integer-square-root-algorithm-for-arm-thumb2
uint64_t sqrt(uint64_t a_nInput) {
    uint64_t op  = a_nInput;
    uint64_t res = 0;
    uint64_t one = (uint64_t)1 << 62;

    // "one" starts at the highest power of four <= than the argument.
    while (one > op) {
        one >>= 2;
    }

    while (one != 0) {
        if (op >= res + one) {
            op = op - (res + one);
            res = res +  2 * one;
        }
        res >>= 1;
        one >>= 2;
    }

    return res;
}

size_t get_trailing_zeros(uint64_t val) {
    for (size_t i = 0; i < 64; i++) {
        if ((val & 1) != 0) {
            return i;
        }
        val >>= 1;
    }
    return 64;
}

void *get_device_tree_blob(const char *config, size_t extra_size,
                           bool measure) {
    int ret;

    size_t size = 0;
    void *dtb = NULL;

    {
        char *dtb_path = NULL;
        bool soft_panic;
        if (config != NULL) {
            dtb_path = config_get_value(config, 0, "dtb_path");
            soft_panic = true;
        }
        if (dtb_path == NULL) {
            dtb_path = config_get_value(NULL, 0, "global_dtb");
            soft_panic = false;
        }
        if (dtb_path != NULL) {
            struct file_handle *dtb_file;
            if ((dtb_file = uri_open(dtb_path, MEMMAP_BOOTLOADER_RECLAIMABLE, false
#if defined (__i386__)
                , NULL, NULL
#endif
            )) == NULL)
                panic(soft_panic, "dtb: Failed to open device tree blob with path `%#`. Is the path correct?", dtb_path);

            dtb = dtb_file->fd;
            size = dtb_file->size;
            fclose(dtb_file);

            ret = fdt_check_full(dtb, size);
            if (ret != 0) {
                panic(soft_panic, "dtb: Invalid device tree blob at `%#`: '%s'", dtb_path, fdt_strerror(ret));
            }

#if defined (UEFI)
            if (measure) {
                tpm_measure_path(TPM_PCR_BOOT_AUTH, TPM_EV_IPL, "dtb_path: ", dtb_path);
                tpm_measure(TPM_PCR_LOADED_IMAGES, TPM_EV_IPL,
                            dtb, size, "dtb_path: ", dtb_path);
            }
#endif

            printv("dtb: loaded dtb at %p from file `%#`\n", dtb, dtb_path);
        }
    }

#if defined (UEFI)
    if (!dtb) {
        EFI_GUID dtb_guid = EFI_DTB_TABLE_GUID;
        for (size_t i = 0; i < gST->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE *cur_table = &gST->ConfigurationTable[i];
            if (memcmp(&cur_table->VendorGuid, &dtb_guid, sizeof(EFI_GUID)))
                continue;
            size = fdt_totalsize(cur_table->VendorTable);
            if (measure) {
                tpm_measure(TPM_PCR_LOADED_IMAGES, TPM_EV_IPL,
                            cur_table->VendorTable, size, "efi_dtb", NULL);
            }
            dtb = ext_mem_alloc(size);
            ret = fdt_open_into(cur_table->VendorTable, dtb, size);
            if (ret < 0) {
                panic(true, "dtb: failed to resize new DTB");
            }
            printv("dtb: found dtb at %p via EFI\n", cur_table->VendorTable);
            break;
        }
    }
#else
    (void)measure;
#endif

    if (extra_size == 0) {
        return dtb;
    }

    if (dtb) {
        printv("dtb: dtb has size %X\n", (uint64_t)size);

        size_t new_size = CHECKED_ADD(size, extra_size,
            panic(true, "dtb: size overflow"));
        void *new_tab = ext_mem_alloc(new_size);

        ret = fdt_open_into(dtb, new_tab, new_size);
        if (ret < 0) {
            panic(true, "dtb: failed to resize new DTB");
        }

        pmm_free(dtb, size);
        return new_tab;
    }

    dtb = ext_mem_alloc(extra_size);

    ret = fdt_create_empty_tree(dtb, extra_size);
    if (ret < 0) {
        panic(true, "dtb: failed to create a device tree blob: '%s'", fdt_strerror(ret));
    }

    ret = fdt_setprop_u32(dtb, 0, "#address-cells", 2);
    if (ret < 0) {
        panic(true, "dtb: failed to set #address-cells: '%s'", fdt_strerror(ret));
    }

    ret = fdt_setprop_u32(dtb, 0, "#size-cells", 1);
    if (ret < 0) {
        panic(true, "dtb: failed to set #size-cells: '%s'", fdt_strerror(ret));
    }

    return dtb;
}

#if defined (UEFI)

#if defined (__riscv)

RISCV_EFI_BOOT_PROTOCOL *get_riscv_boot_protocol(void) {
    EFI_GUID boot_proto_guid = RISCV_EFI_BOOT_PROTOCOL_GUID;
    RISCV_EFI_BOOT_PROTOCOL *proto;

    // LocateProtocol() is available from EFI version 1.1
    if (gBS->Hdr.Revision >= ((1 << 16) | 10)) {
        if (gBS->LocateProtocol(&boot_proto_guid, NULL, (void **)&proto) == EFI_SUCCESS) {
            return proto;
        }
    }

    UINTN bufsz = 0;
    if (gBS->LocateHandle(ByProtocol, &boot_proto_guid, NULL, &bufsz, NULL) != EFI_BUFFER_TOO_SMALL)
        return NULL;

    UINTN handles_alloc = bufsz;
    EFI_HANDLE *handles_buf = ext_mem_alloc(handles_alloc);
    if (handles_buf == NULL)
        return NULL;

    if (bufsz < sizeof(EFI_HANDLE))
        goto error;

    if (gBS->LocateHandle(ByProtocol, &boot_proto_guid, NULL, &bufsz, handles_buf) != EFI_SUCCESS)
        goto error;

    if (gBS->HandleProtocol(handles_buf[0], &boot_proto_guid, (void **)&proto) != EFI_SUCCESS)
        goto error;

    pmm_free(handles_buf, handles_alloc);
    return proto;

error:
    pmm_free(handles_buf, handles_alloc);
    return NULL;
}

#endif

no_unwind bool efi_boot_services_exited = false;

bool efi_exit_boot_services(void) {
    EFI_STATUS status;

    // Pull entropy from EFI_RNG_PROTOCOL while it's still callable and
    // publish it for the kernel to mix into its early RNG state.
    rng_seed_install();

    // Every path past this point ends with the allocator locked out, and
    // panic() reaches term_fallback(), so build its terminal now.
    term_prepare_post_ebs();

    // Free the buffer init_memmap left us; the loop below manages
    // allocation lifetime itself.
    status = gBS->FreePool(efi_mmap);
    if (status) {
        goto fail;
    }
    efi_mmap = NULL;

    EFI_MEMORY_DESCRIPTOR *efi_copy = NULL;
    UINTN efi_mmap_alloc = 0;
    UINTN efi_copy_alloc = 0;

    bli_on_boot();

    for (size_t retries = 0; ; retries++) {
        if (retries == 128) {
            goto fail;
        }

        efi_mmap_size = efi_mmap_alloc;
        status = gBS->GetMemoryMap(&efi_mmap_size, efi_mmap, &efi_mmap_key,
                                   &efi_desc_size, &efi_desc_ver);
        if (status == EFI_BUFFER_TOO_SMALL) {
            // Map grew (or first iteration). Free both buffers and
            // reallocate, with slack for the descriptors AllocatePool
            // itself may add.
            if (efi_mmap != NULL) {
                gBS->FreePool(efi_mmap);
                efi_mmap = NULL;
            }
            if (efi_copy != NULL) {
                gBS->FreePool(efi_copy);
                efi_copy = NULL;
            }
            efi_mmap_alloc = efi_mmap_size + 4096;
            status = gBS->AllocatePool(EfiLoaderData, efi_mmap_alloc,
                                       (void **)&efi_mmap);
            if (status) {
                goto fail;
            }
            // Cutting a descriptor at both edges of every region we own can
            // add two descriptors per region on top of the firmware's own.
            UINTN split_slack = CHECKED_MUL((UINTN)untouched_memmap_entries, (UINTN)2, goto fail);
            split_slack = CHECKED_MUL(split_slack, efi_desc_size, goto fail);
            efi_copy_alloc = CHECKED_ADD(CHECKED_MUL(efi_mmap_alloc, (UINTN)2, goto fail),
                                         split_slack, goto fail);
            status = gBS->AllocatePool(EfiLoaderData, efi_copy_alloc,
                                       (void **)&efi_copy);
            if (status) {
                goto fail;
            }
            continue;
        }
        if (status) {
            goto fail;
        }

        // Be gone, UEFI!
        status = gBS->ExitBootServices(efi_image_handle, efi_mmap_key);
        if (status == EFI_SUCCESS) {
            // The map rebuild below can panic, and term_fallback() picks its
            // backend off this flag: the console protocol is already gone.
            efi_boot_services_exited = true;
            break;
        }
        // Map key invalidated by an allocation - retry.
    }

    const size_t EFI_COPY_MAX_ENTRIES = efi_copy_alloc / efi_desc_size;

#if defined(__x86_64__) || defined(__i386__)
    asm volatile ("cli" ::: "memory");
#elif defined (__aarch64__)
    asm volatile ("msr daifset, #15" ::: "memory");
#elif defined (__riscv)
    asm volatile ("csrci sstatus, 0x2" ::: "memory");
#elif defined (__loongarch64)
    asm volatile ("csrxchg $r0, %0, 0x0" :: "r" (0x4) : "memory");
#else
#error Unknown architecture
#endif

    // Go through new EFI memmap and free up bootloader entries
    size_t entry_count = efi_mmap_size / efi_desc_size;

    size_t efi_copy_i = 0;

    for (size_t i = 0; i < entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *orig_entry = (void *)efi_mmap + i * efi_desc_size;

        if (orig_entry->NumberOfPages == 0) {
            continue;
        }

        uint64_t base = orig_entry->PhysicalStart;
        uint64_t top = base + orig_entry->NumberOfPages * 4096;

        // Emit the descriptor in runs, cut wherever it crosses the edge of a
        // region we own. Firmware is free to describe several of our regions
        // with one descriptor, so matching just one of them would leave the
        // others held as loader memory.
        for (uint64_t cur = base; cur < top;) {
            uint64_t run_top = top;
            bool owned = false;

            for (size_t j = 0; j < untouched_memmap_entries; j++) {
                if (untouched_memmap[j].type != MEMMAP_USABLE)
                    continue;

                uint64_t reg_base = untouched_memmap[j].base;
                uint64_t reg_top = CHECKED_ADD(reg_base, untouched_memmap[j].length, continue);

                if (cur >= reg_base && cur < reg_top) {
                    owned = true;
                    if (reg_top < run_top) {
                        run_top = reg_top;
                    }
                    break;
                }

                if (reg_base > cur && reg_base < run_top) {
                    run_top = reg_base;
                }
            }

            EFI_MEMORY_DESCRIPTOR *new_entry = (void *)efi_copy + efi_copy_i * efi_desc_size;
            memcpy(new_entry, orig_entry, efi_desc_size);
            new_entry->PhysicalStart = cur;
            new_entry->NumberOfPages = (run_top - cur) / 4096;
            if (owned) {
                new_entry->Type = EfiConventionalMemory;
            }
            if (cur != base) {
                new_entry->VirtualStart = 0;
            }

            efi_copy_i++;
            if (efi_copy_i == EFI_COPY_MAX_ENTRIES) {
                panic(false, "efi: New memory map exhausted");
            }

            cur = run_top;
        }
    }

    efi_mmap = efi_copy;
    efi_mmap_size = efi_copy_i * efi_desc_size;

    printv("efi: Exited boot services.\n");

    return true;

fail:
    panic(false, "efi: Failed to exit boot services");
}

#endif
