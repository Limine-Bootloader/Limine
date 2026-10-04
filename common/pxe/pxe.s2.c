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

// PXE 2.1, Tables 3-1 and 3-2: the bytes of each structure, over the length it
// declares, sum to zero.
static bool checksum_ok(const void *ptr, size_t len) {
    const uint8_t *bytes = ptr;
    uint8_t sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum += bytes[i];
    }

    return sum == 0;
}

void pxe_init(void) {
    // Stage 1 does not preserve the structure pointers the boot ROM hands to
    // the NBP (PXE 2.1, 4.4.5), so ask the installation check (3.1.1) instead.
    struct rm_regs r = {0};
    r.eax = 0x5650;
    rm_int(0x1a, &r, &r);

    if ((r.eax & 0xffff) != 0x564e || (r.eflags & EFLAGS_CF)) {
        panic(false, "pxe: PXE installation check failed");
    }

    struct pxenv *pxenv = (struct pxenv *)rm_desegment(r.es, r.ebx & 0xffff);

    if (memcmp(pxenv->signature, "PXENV+", sizeof(pxenv->signature)) != 0
     || !checksum_ok(pxenv, pxenv->length)) {
        panic(false, "pxe: Invalid PXENV+ structure");
    }

    // Below 2.1 there is no !PXE structure (PXE 2.1, Table 3-1), only the
    // PXENV+ entry point, which takes its arguments in registers (3.2,
    // Example-3) rather than on the stack as pxe_call() passes them.
    if (pxenv->version < 0x0201) {
        panic(false, "pxe: PXE API version %u.%u is unsupported, 2.1 or newer is required",
              pxenv->version >> 8, pxenv->version & 0xff);
    }

    struct pxe *pxe = (struct pxe *)rm_desegment(pxenv->pxe_ptr.segment, pxenv->pxe_ptr.offset);

    if (memcmp(pxe->signature, "!PXE", sizeof(pxe->signature)) != 0
     || pxe->struct_length < sizeof(struct pxe)
     || !checksum_ok(pxe, pxe->struct_length)) {
        panic(false, "pxe: Invalid !PXE structure");
    }

    // pxe_call() runs in real mode, hence on a 16-bit stack (PXE 2.1, Table 3-2).
    set_pxe_fp(((uint32_t)pxe->entry_point_sp.segment << 16) | pxe->entry_point_sp.offset);
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
