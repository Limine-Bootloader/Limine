#include <lib/print.h>
#include <pxe/pxe.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <mm/pmm.h>
#if defined (BIOS)
#include <lib/real.h>
#elif defined (UEFI)
#include <efi.h>
#endif

#if defined (BIOS)

void set_pxe_fp(uint32_t fp);

struct volume *pxe_bind_volume(void) {
    struct volume *volume = ext_mem_alloc(sizeof(struct volume));

    volume->pxe = true;

    return volume;
}

void pxe_init(void) {
    //pxe installation check
    struct rm_regs r = { 0 };
    r.ebx = 0;
    r.ecx = 0;
    r.eax = 0x5650;
    r.es = 0;

    rm_int(0x1a, &r, &r);
    if ((r.eax & 0xffff) != 0x564e) {
        panic(false, "PXE installation check failed");
    }

    struct pxenv *pxenv = NULL;

    pxenv = (struct pxenv *)((r.es << 4) + (r.ebx & 0xffff));
    if (memcmp(pxenv->signature, PXE_SIGNATURE, sizeof(pxenv->signature)) != 0) {
        panic(false, "PXENV structure signature corrupted");
    }

    if (pxenv->version < 0x201) {
        //we won't support pxe < 2.1, grub does this too and it seems to work fine
        panic(false, "pxe version too old");
    }

    struct bangpxe *bangpxe = (struct bangpxe *)((((pxenv->pxe_ptr & 0xffff0000) >> 16) << 4) + (pxenv->pxe_ptr & 0xffff));

    if (memcmp(bangpxe->signature, PXE_BANGPXE_SIGNATURE,
            sizeof(bangpxe->signature))
        != 0) {
        panic(false, "!pxe signature corrupted");
    }
    set_pxe_fp(bangpxe->rm_entry);
    printv("pxe: Successfully initialized\n");
}

#elif defined (UEFI)

struct volume *pxe_bind_volume(EFI_HANDLE efi_handle, EFI_PXE_BASE_CODE *pxe_base_code) {
    struct volume *volume = ext_mem_alloc(sizeof(struct volume));

    volume->efi_handle = efi_handle;
    volume->pxe_base_code = pxe_base_code;
    volume->pxe = true;

    return volume;
}

#endif
