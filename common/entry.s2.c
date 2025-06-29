#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <lib/term.h>
#include <lib/real.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/part.h>
#include <lib/config.h>
#include <lib/trace.h>
#include <sys/e820.h>
#include <sys/a20.h>
#include <lib/print.h>
#include <fs/file.h>
#include <lib/elf.h>
#include <mm/pmm.h>
#include <protos/linux.h>
#include <protos/chainload.h>
#include <menu.h>
#include <pxe/pxe.h>
#include <pxe/tftp.h>
#include <drivers/disk.h>
#include <sys/idt.h>
#include <sys/cpu.h>

struct volume *boot_volume;

#if defined (BIOS)

bool stage3_loaded = false;
static bool stage3_found = false;

extern symbol stage3_addr;
extern symbol limine_bios_sys_size;
extern symbol build_id_s2;
extern symbol build_id_s3;

static bool stage3_init(struct volume *part) {
    struct file_handle *stage3;

    bool old_cif = case_insensitive_fopen;
    case_insensitive_fopen = true;
    if ((stage3 = fopen(part, "/limine-bios.sys")) == NULL
     && (stage3 = fopen(part, "/limine/limine-bios.sys")) == NULL
     && (stage3 = fopen(part, "/boot/limine-bios.sys")) == NULL
     && (stage3 = fopen(part, "/boot/limine/limine-bios.sys")) == NULL) {
        case_insensitive_fopen = old_cif;
        return false;
    }
    case_insensitive_fopen = old_cif;

    stage3_found = true;

    if (stage3->size != (size_t)limine_bios_sys_size) {
        print("limine-bios.sys size incorrect.\n");
        return false;
    }

    fread(stage3, stage3_addr,
          (uintptr_t)stage3_addr - 0xf000,
          stage3->size - ((uintptr_t)stage3_addr - 0xf000));

    fclose(stage3);

    if (memcmp(build_id_s2 + 16, build_id_s3 + 16, 20) != 0) {
        print("limine-bios.sys build ID mismatch.\n");
        return false;
    }

    stage3_loaded = true;

    return true;
}

enum {
    BOOTED_FROM_HDD = 0,
    BOOTED_FROM_PXE = 1,
    BOOTED_FROM_CD = 2
};

noreturn void entry(uint8_t boot_drive, int boot_from) {
    // XXX DO NOT MOVE A20 ENABLE CALL
    if (!a20_enable()) {
        panic(false, "Could not enable A20 line");
    }

    uint64_t usec_at_entry = rdtsc_usec();

    init_e820();
    init_memmap();

    init_idt();

    disk_create_index();

    if (boot_from == BOOTED_FROM_HDD || boot_from == BOOTED_FROM_CD) {
        boot_volume = volume_get_by_bios_drive(boot_drive);
    } else if (boot_from == BOOTED_FROM_PXE) {
        pxe_init();
        boot_volume = pxe_bind_volume();
    }

    if (boot_volume == NULL) {
        panic(false, "Could not determine boot drive");
    }

    volume_iterate_parts(boot_volume,
        if (stage3_init(_PART)) {
            break;
        }
    );

    if (!stage3_found) {
        print("\n"
              "!! Stage 3 file not found!\n"
              "!! Have you copied limine-bios.sys to the root, /boot, /limine, or /boot/limine\n"
              "!! directories of one of the partitions on the boot device?\n\n");
    }

    if (!stage3_loaded) {
        panic(false, "Failed to load stage 3.");
    }

    term_fallback();

    usec_at_bootloader_entry = usec_at_entry;
    stage3_common();
}

#endif
