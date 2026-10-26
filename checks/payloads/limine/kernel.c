#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "common.h"
#include "state.h"
#include "arch.h"
#include "variant.h"
#include "fdt.h"
#include "tpm.h"

#define REQ __attribute__((used, section(".limine_requests")))

#if !TEST_NO_MARKERS
__attribute__((used, section(".limine_requests_start_marker")))
static volatile uint64_t requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end_marker")))
static volatile uint64_t requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;
#endif

#if TEST_BASE_REVISION >= 0
REQ static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(TEST_BASE_REVISION);
#endif

#if TEST_DUPLICATE_BASE_REVISION
REQ static volatile uint64_t base_revision_dup[] = LIMINE_BASE_REVISION(TEST_BASE_REVISION);
#endif

#if TEST_REQUEST_OUTSIDE_MARKERS
__attribute__((used, section(".limine_requests_before")))
static volatile struct limine_firmware_type_request outside_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0, .response = NULL
};
#endif

void entry_req(void);
void ap_entry(struct limine_mp_info *);

#if !TEST_MINIMAL

#if TEST_ENTRY_REQUEST
REQ static volatile struct limine_entry_point_request entry_point_request = {
    .id = LIMINE_ENTRY_POINT_REQUEST_ID, .revision = 0, .response = NULL,
    .entry = entry_req
};
#endif

REQ static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0, .response = NULL
};

#if TEST_DUPLICATE_REQUEST
REQ static volatile struct limine_bootloader_info_request bootloader_info_request_dup = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0, .response = NULL
};
#endif

REQ static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0, .response = NULL
};

#if !TEST_REQUEST_OUTSIDE_MARKERS
REQ static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0, .response = NULL
};
#endif

#if TEST_STACK_SIZE > 0
REQ static volatile struct limine_stack_size_request stack_size_request = {
    .id = LIMINE_STACK_SIZE_REQUEST_ID, .revision = 0, .response = NULL,
    .stack_size = TEST_STACK_SIZE
};
#endif

REQ static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 1, .response = NULL
};

#if TEST_PAGING_REV >= 0
REQ static volatile struct limine_paging_mode_request paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID, .revision = TEST_PAGING_REV, .response = NULL,
    .mode = TEST_PAGING_MODE, .max_mode = TEST_PAGING_MAX, .min_mode = TEST_PAGING_MIN
};
#endif

#if TEST_MP_FLAGS >= 0
REQ static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .response = NULL, .flags = TEST_MP_FLAGS
};
#endif

#if defined (__riscv)
REQ static volatile struct limine_riscv_bsp_hartid_request bsp_hartid_request = {
    .id = LIMINE_RISCV_BSP_HARTID_REQUEST_ID, .revision = 0, .response = NULL
};
#endif

REQ static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_executable_file_request executable_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID, .revision = 0, .response = NULL
};

#if TEST_INTERNAL_MODULES == 1
static struct limine_internal_module im_required = {
    .path = "im/required.bin", .string = "internal required",
    .flags = LIMINE_INTERNAL_MODULE_REQUIRED
};
static struct limine_internal_module im_optional_missing = {
    .path = "im/missing.bin", .string = "internal optional missing", .flags = 0
};
static struct limine_internal_module im_dotdot = {
    .path = "./im/../im/dotdot.bin", .string = "internal dotdot", .flags = 0
};
static struct limine_internal_module im_absolute = {
    .path = "/internal-abs.bin", .string = "", .flags = LIMINE_INTERNAL_MODULE_REQUIRED
};
static struct limine_internal_module im_compressed = {
    .path = "im/compressed.bin.gz", .string = "internal compressed",
    .flags = LIMINE_INTERNAL_MODULE_REQUIRED | LIMINE_INTERNAL_MODULE_COMPRESSED
};
__attribute__((used)) static struct limine_internal_module *internal_modules[] = {
    &im_required, &im_optional_missing, &im_dotdot, &im_absolute, &im_compressed
};
#elif TEST_INTERNAL_MODULES == 2
static struct limine_internal_module im_required_missing = {
    .path = "im/absent.bin", .string = "internal required missing",
    .flags = LIMINE_INTERNAL_MODULE_REQUIRED
};
__attribute__((used)) static struct limine_internal_module *internal_modules[] = { &im_required_missing };
#elif TEST_INTERNAL_MODULES == 3
static struct limine_internal_module im_optional_missing = {
    .path = "im/missing.bin", .string = "internal optional missing", .flags = 0
};
__attribute__((used)) static struct limine_internal_module *internal_modules[] = { &im_optional_missing };
#endif

REQ static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = TEST_MODULE_REV, .response = NULL,
#if TEST_INTERNAL_MODULES != 0 && TEST_MODULE_REV >= 1
    .internal_module_count = sizeof(internal_modules) / sizeof(internal_modules[0]),
    .internal_modules = internal_modules
#endif
};

REQ static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_smbios_request smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_efi_system_table_request efi_system_table_request = {
    .id = LIMINE_EFI_SYSTEM_TABLE_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_tpm_event_log_request tpm_event_log_request = {
    .id = LIMINE_TPM_EVENT_LOG_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_efi_memmap_request efi_memmap_request = {
    .id = LIMINE_EFI_MEMMAP_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_date_at_boot_request date_at_boot_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST_ID, .revision = 0, .response = NULL
};

REQ static volatile struct limine_bootloader_performance_request perf_request = {
    .id = LIMINE_BOOTLOADER_PERFORMANCE_REQUEST_ID, .revision = 0, .response = NULL
};

#if TEST_KEEP_IOMMU_REQUEST
REQ static volatile struct limine_x86_64_keep_iommu_request keep_iommu_request = {
    .id = LIMINE_X86_64_KEEP_IOMMU_REQUEST_ID, .revision = 0, .response = NULL
};
#endif

REQ static volatile struct limine_tsc_frequency_request tsc_frequency_request = {
    .id = LIMINE_TSC_FREQUENCY_REQUEST_ID, .revision = 0, .response = NULL
};

#if TEST_FLANTERM_REQUEST
REQ static volatile struct limine_flanterm_fb_init_params_request flanterm_request = {
    .id = LIMINE_FLANTERM_FB_INIT_PARAMS_REQUEST_ID, .revision = 0, .response = NULL
};
#endif

#if TEST_ENTROPY_COUNT >= 0
REQ static volatile struct limine_entropy_request entropy_request = {
    .id = LIMINE_ENTROPY_REQUEST_ID, .revision = 0, .response = NULL,
    .value_count = TEST_ENTROPY_COUNT
};
#endif

#if TEST_REQS_SECTION == 1
#define REQS_ENTRY(r) (void *)&r,
__attribute__((used, section(".limine_reqs")))
static void *limine_reqs[] = {
#if TEST_ENTRY_REQUEST
    REQS_ENTRY(entry_point_request)
#endif
    REQS_ENTRY(bootloader_info_request)
    REQS_ENTRY(cmdline_request)
    REQS_ENTRY(firmware_type_request)
    REQS_ENTRY(stack_size_request)
    REQS_ENTRY(hhdm_request)
    REQS_ENTRY(framebuffer_request)
    REQS_ENTRY(paging_mode_request)
    REQS_ENTRY(mp_request)
#if defined (__riscv)
    REQS_ENTRY(bsp_hartid_request)
#endif
    REQS_ENTRY(memmap_request)
    REQS_ENTRY(executable_file_request)
    REQS_ENTRY(module_request)
    REQS_ENTRY(rsdp_request)
    REQS_ENTRY(smbios_request)
    REQS_ENTRY(efi_system_table_request)
    REQS_ENTRY(tpm_event_log_request)
    REQS_ENTRY(efi_memmap_request)
    REQS_ENTRY(executable_address_request)
    REQS_ENTRY(dtb_request)
    REQS_ENTRY(perf_request)
    REQS_ENTRY(keep_iommu_request)
    REQS_ENTRY(tsc_frequency_request)
    REQS_ENTRY(flanterm_request)
    REQS_ENTRY(entropy_request)
    NULL
};
#elif TEST_REQS_SECTION == 2
#define REQS_ENTRY4 (void *)&bootloader_info_request, (void *)&bootloader_info_request, \
    (void *)&bootloader_info_request, (void *)&bootloader_info_request,
#define REQS_ENTRY16 REQS_ENTRY4 REQS_ENTRY4 REQS_ENTRY4 REQS_ENTRY4
__attribute__((used, section(".limine_reqs")))
static void *limine_reqs[] = {
    REQS_ENTRY16 REQS_ENTRY16 REQS_ENTRY16 REQS_ENTRY16 REQS_ENTRY16 REQS_ENTRY16 REQS_ENTRY16 REQS_ENTRY16
    NULL
};
#endif

#endif

// Hidden: the entry code reaches these PC-relatively, which a shared object
// may do only with symbols it cannot have preempted.
__attribute__((visibility("hidden"))) uint64_t bsp_state[ST_COUNT];
__attribute__((visibility("hidden"))) uint64_t ap_states[MAX_APS][ST_COUNT];
__attribute__((visibility("hidden"))) volatile uint64_t entry_id;

#if TEST_SYMBOLS
// In the shared object, the bootloader resolves these through the dynamic
// symbol table: the addresses taken in code from the GOT, the pointers with
// an addend each.
extern char test_absolute_symbol[];
extern char test_weak_undefined[] __attribute__((weak));
static char symbol_target[16];
extern char test_symbol[16] __attribute__((alias("symbol_target")));
char *volatile test_symbol_ptr = &test_symbol[5];
char *volatile test_weak_ptr = &test_weak_undefined[16];

// Hides where an address came from, so that comparisons are left to run.
static uintptr_t opaque(const void *p) {
    uintptr_t v = (uintptr_t)p;
    __asm__ volatile ("" : "+r"(v));
    return v;
}

static void report_symbols(void) {
    fact("symbols.got", "%d", opaque(test_symbol) == opaque(symbol_target));
    fact("symbols.addend", "%d", opaque(test_symbol_ptr) == opaque(symbol_target + 5));
    fact("symbols.absolute", "%lx", (uint64_t)(uintptr_t)test_absolute_symbol);
    fact("symbols.weak", "%lx", (uint64_t)(uintptr_t)test_weak_undefined);
    fact("symbols.weak_addend", "%lx", (uint64_t)(uintptr_t)test_weak_ptr);
}
#endif

uint64_t hhdm_offset;
bool have_hhdm;
static int base_rev;
static struct limine_memmap_response *memmap;

void out_char(char c) {
    arch_out_char(c);
}

__attribute__((noreturn)) void halt(void) {
    for (;;) {
        arch_cpu_relax();
    }
}

// The executable image's own extent, from the linker script. Its ELF header,
// which the linker places at its start, begins the text segment too.
extern char __ehdr_start[], executable_end[];
extern char text_end[], rodata_start[], rodata_end[], data_start[], data_end[];

__attribute__((unused)) static const char *memmap_type_name(uint64_t type) {
    static const char *names[] = {
        "usable", "reserved", "acpi_reclaimable", "acpi_nvs", "bad_memory",
        "bootloader_reclaimable", "executable_and_modules", "framebuffer", "reserved_mapped"
    };
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "invalid";
}

static int64_t region_type(uint64_t phys) {
    if (memmap == NULL) {
        return -1;
    }
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (phys >= e->base && phys - e->base < e->length) {
            return e->type;
        }
    }
    return -1;
}

// Whether [phys, phys + len) lies entirely within regions of the given type.
static bool range_of_type(uint64_t phys, uint64_t len, uint64_t type) {
    uint64_t end = phys + len;
    while (phys < end) {
        bool found = false;
        for (uint64_t i = 0; i < memmap->entry_count; i++) {
            struct limine_memmap_entry *e = memmap->entries[i];
            if (e->type == type && phys >= e->base && phys - e->base < e->length) {
                phys = e->base + e->length;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool is_hhdm_ptr(uint64_t p) {
    return have_hhdm && p >= hhdm_offset;
}

// A response, or anything hanging off one, must be an HHDM pointer into
// bootloader-reclaimable memory.
static void check_resp_ptr(const char *name, const volatile void *p, size_t len) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    if (!have_hhdm) {
        return;
    }
    if (!check_quiet(is_hhdm_ptr(v), name, "pointer %lx is not in the HHDM (offset %lx)", v, hhdm_offset)) {
        return;
    }
    if (memmap != NULL && len != 0) {
        check_quiet(range_of_type(v - hhdm_offset, len, LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE),
              name, "%lx+%lx is not bootloader reclaimable (type %ld)", v - hhdm_offset,
              (uint64_t)len, region_type(v - hhdm_offset));
    }
}

static void fact_uuid(const char *key, struct limine_uuid *u) {
    kprintf("K %s=%08x-%04x-%04x-", key, u->a, u->b, u->c);
    out_hex(u->d, 2);
    out_char('-');
    out_hex(u->d + 2, 6);
    out_char('\n');
}

__attribute__((unused)) static void report_file(const char *prefix, struct limine_file *f) {
    char key[64];
    size_t n = 0;
    while (prefix[n] != 0 && n < 40) {
        key[n] = prefix[n];
        n++;
    }
#define KEY(SUFFIX) (memcpy(key + n, SUFFIX, sizeof(SUFFIX)), key)
    check_resp_ptr(KEY(".struct"), f, sizeof(*f));
    fact(KEY(".revision"), "%lu", f->revision);
    fact(KEY(".size"), "%lu", f->size);
    fact(KEY(".address"), "%lx", (uint64_t)(uintptr_t)f->address);
    fact(KEY(".address_aligned"), "%d", (int)((uint64_t)(uintptr_t)f->address % 4096 == 0));
    check((uint64_t)(uintptr_t)f->address % 4096 == 0, KEY(".address_alignment"),
          "address %p is not 4KiB aligned", f->address);
    if (have_hhdm) {
        check(is_hhdm_ptr((uint64_t)(uintptr_t)f->address), KEY(".address_hhdm"),
              "address %p is not an HHDM pointer", f->address);
        fact(KEY(".hash"), "%016lx", fnv1a(f->address, f->size));
    }
    check_resp_ptr(KEY(".path_ptr"), f->path, strlen(f->path) + 1);
    fact_str(KEY(".path"), f->path);
    check_resp_ptr(KEY(".string_ptr"), f->string, strlen(f->string) + 1);
    fact_str(KEY(".string"), f->string);
    fact(KEY(".media_type"), "%u", f->media_type);
    fact(KEY(".partition_index"), "%u", f->partition_index);
    fact(KEY(".mbr_disk_id"), "%08x", f->mbr_disk_id);
    fact(KEY(".tftp"), "%u.%u.%u.%u:%u", f->tftp_ipv4[0], f->tftp_ipv4[1], f->tftp_ipv4[2],
         f->tftp_ipv4[3], f->tftp_port);
    fact_uuid(KEY(".gpt_disk_uuid"), &f->gpt_disk_uuid);
    fact_uuid(KEY(".gpt_part_uuid"), &f->gpt_part_uuid);
    fact_uuid(KEY(".part_uuid"), &f->part_uuid);
#undef KEY
}

__attribute__((unused)) static void check_memmap(void) {
#if !TEST_MINIMAL
    struct limine_memmap_response *r = memmap_request.response;
    fact("memmap.response", "%d", r != NULL);
    if (r == NULL) {
        return;
    }
    memmap = r;
    check_resp_ptr("memmap.response_ptr", r, sizeof(*r));
    check_resp_ptr("memmap.entries_ptr", r->entries, r->entry_count * sizeof(void *));
    fact("memmap.revision", "%lu", r->revision);
    fact("memmap.count", "%lu", r->entry_count);
    check(r->entry_count > 0, "memmap.nonempty", "no entries");
    uint64_t usable = 0;
    bool sorted = true, aligned = true, nonoverlap = true, types_ok = true, nonzero = true;
    bool low_page_ok = true;
    for (uint64_t i = 0; i < r->entry_count; i++) {
        struct limine_memmap_entry *e = r->entries[i];
        check_resp_ptr("memmap.entry_ptr", e, sizeof(*e));
        kprintf("K memmap.%lu=%lx %lx %s\n", i, e->base, e->length, memmap_type_name(e->type));
        if (e->type > LIMINE_MEMMAP_RESERVED_MAPPED
         || (e->type == LIMINE_MEMMAP_RESERVED_MAPPED && base_rev < 4)) {
            types_ok = false;
            kprintf("C FAIL memmap.type: entry %lu has type %lu\n", i, e->type);
        }
        if (e->length == 0) {
            nonzero = false;
        }
        if (i > 0 && r->entries[i - 1]->base > e->base) {
            sorted = false;
        }
        bool strict = e->type == LIMINE_MEMMAP_USABLE
                   || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;
        if (strict && (e->base % 4096 != 0 || e->length % 4096 != 0)) {
            aligned = false;
            kprintf("C FAIL memmap.alignment: entry %lu %lx+%lx\n", i, e->base, e->length);
        }
        if (strict) {
            for (uint64_t j = 0; j < r->entry_count; j++) {
                struct limine_memmap_entry *o = r->entries[j];
                if (j == i || o->length == 0) {
                    continue;
                }
                if (o->base < e->base + e->length && e->base < o->base + o->length) {
                    nonoverlap = false;
                    kprintf("C FAIL memmap.overlap: entry %lu overlaps entry %lu\n", i, j);
                }
            }
        }
        if (e->type == LIMINE_MEMMAP_USABLE) {
            usable += e->length;
            if (base_rev <= 2 && e->base < 0x1000) {
                low_page_ok = false;
            }
        }
    }
    fact("memmap.usable_bytes", "%lu", usable);
    // How varied the bytes in each usable entry are, for randomise_memory:
    // the distinct values among those of every 4KiB page in up to 64 of them.
    // Only when asked to, as it takes time.
    bool sample = cmdline_request.response != NULL && strcmp(cmdline_request.response->cmdline, "sample-memory") == 0;
    for (uint64_t i = 0; sample && have_hhdm && i < r->entry_count; i++) {
        struct limine_memmap_entry *e = r->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        uint64_t pages = e->length / 4096, step = pages > 64 ? pages / 64 : 1;
        for (uint64_t pg = 0; pg < pages; pg += step) {
            const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(e->base + pg * 4096 + hhdm_offset);
            bool seen[256] = { false };
            unsigned distinct = 0;
            for (uint64_t k = 0; k < 4096; k++) {
                if (!seen[p[k]]) {
                    seen[p[k]] = true;
                    distinct++;
                }
            }
            kprintf("K memmap.usable_page=%lx %u\n", e->base + pg * 4096, distinct);
        }
    }
    check(sorted, "memmap.sorted", "entries are not sorted by base");
    check(aligned, "memmap.strict_alignment", "see above");
    check(nonoverlap, "memmap.strict_nonoverlap", "see above");
    check(types_ok, "memmap.types", "see above");
    check(nonzero, "memmap.nonzero_lengths", "an entry has length 0");
    check(low_page_ok, "memmap.first_page", "memory below 0x1000 is usable under base revision %d",
          base_rev);
#endif
}

__attribute__((unused)) static void check_kernel_mapping(void) {
#if !TEST_MINIMAL
    struct limine_executable_address_response *r = executable_address_request.response;
    fact("executable_address.response", "%d", r != NULL);
    if (r == NULL) {
        return;
    }
    check_resp_ptr("executable_address.response_ptr", r, sizeof(*r));
    fact("executable_address.physical_base", "%lx", r->physical_base);
    fact("executable_address.virtual_base", "%lx", r->virtual_base);
    check(r->virtual_base == (uint64_t)(uintptr_t)__ehdr_start,
          "executable_address.virtual_base_matches", "virtual base %lx, image starts at %p",
          r->virtual_base, __ehdr_start);
    check(r->virtual_base >= 0xffffffff80000000, "executable_address.higher_half",
          "virtual base %lx below 0xffffffff80000000", r->virtual_base);
    check(r->physical_base % 4096 == 0, "executable_address.physical_alignment",
          "physical base %lx", r->physical_base);
    if (!have_hhdm) {
        return;
    }
    if (memmap != NULL) {
        check(range_of_type(r->physical_base, (uint64_t)(executable_end - __ehdr_start),
                            LIMINE_MEMMAP_EXECUTABLE_AND_MODULES),
              "executable_address.memmap_type", "image is not in executable_and_modules memory");
    }
    // Every page of every segment, at its own offset, with its own permissions.
    struct {
        const char *name;
        char *start, *end;
        bool w, x;
    } segs[] = {
        { "text", __ehdr_start, text_end, false, true },
        { "rodata", rodata_start, rodata_end, false, false },
        { "data", data_start, data_end, true, false },
    };
    bool contiguous = true, perms = true, cache = true;
    for (size_t s = 0; s < sizeof(segs) / sizeof(segs[0]); s++) {
        uint64_t va = (uint64_t)(uintptr_t)segs[s].start & ~(uint64_t)0xfff;
        for (; va < (uint64_t)(uintptr_t)segs[s].end; va += 4096) {
            struct pt_walk w;
            if (!arch_walk(va, &w)) {
                contiguous = false;
                kprintf("C FAIL executable.mapped: %s page %lx not mapped\n", segs[s].name, va);
                continue;
            }
            if (w.phys != va - r->virtual_base + r->physical_base) {
                contiguous = false;
                kprintf("C FAIL executable.offset: %s page %lx -> %lx\n", segs[s].name, va, w.phys);
            }
            if (w.writable != segs[s].w || (arch_has_nx() && w.executable != segs[s].x)) {
                perms = false;
                kprintf("C FAIL executable.permissions: %s page %lx w=%d x=%d\n", segs[s].name, va,
                        w.writable, w.executable);
            }
            if (w.cache != arch_cache_normal()) {
                cache = false;
                kprintf("C FAIL executable.cache: %s page %lx cache=%d\n", segs[s].name, va, w.cache);
            }
        }
    }
    check(contiguous, "executable.uniform_offset", "see above");
    check(perms, "executable.segment_permissions", "see above");
    check(cache, "executable.caching", "see above");
#endif
}

// The HHDM must map exactly what the base revision says, at the right offset.
__attribute__((unused)) static void check_hhdm_mappings(void) {
#if !TEST_MINIMAL
    if (!have_hhdm || memmap == NULL) {
        return;
    }
    bool ok = true;
    unsigned unmapped_checked = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        bool want;
        switch (e->type) {
            case LIMINE_MEMMAP_USABLE:
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            case LIMINE_MEMMAP_FRAMEBUFFER:
                want = true;
                break;
            case LIMINE_MEMMAP_RESERVED_MAPPED:
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            case LIMINE_MEMMAP_ACPI_NVS:
                want = base_rev != 3;
                break;
            default:
                want = base_rev == 0;
                break;
        }
        // Below 4GiB, revisions 0 to 2 map everything.
        bool low_forced = base_rev < 3 && e->base < 0x100000000;
        uint64_t first = (e->base + 4095) & ~(uint64_t)4095;
        uint64_t last = (e->base + e->length) & ~(uint64_t)4095;
        if (first >= last) {
            continue;
        }
        // The first, middle and last whole page of each region.
        uint64_t samples[3] = { first, first + ((last - first) / 2 & ~(uint64_t)4095), last - 4096 };
        for (int s = 0; s < 3; s++) {
            uint64_t pa = samples[s];
            struct pt_walk w;
            bool mapped = arch_walk(hhdm_offset + pa, &w);
            if (want || low_forced) {
                if (!mapped || w.phys != pa || !w.writable) {
                    ok = false;
                    kprintf("C FAIL hhdm.mapped: %s region %lu page %lx mapped=%d phys=%lx w=%d\n",
                            memmap_type_name(e->type), i, pa, mapped, mapped ? w.phys : 0,
                            mapped ? w.writable : 0);
                    break;
                }
                int want_cache = e->type == LIMINE_MEMMAP_FRAMEBUFFER ? arch_cache_fb() : arch_cache_normal();
                if (w.cache != want_cache) {
                    ok = false;
                    kprintf("C FAIL hhdm.cache: %s region %lu page %lx cache=%d want %d\n",
                            memmap_type_name(e->type), i, pa, w.cache, want_cache);
                    break;
                }
            } else if (!low_forced) {
                unmapped_checked++;
                // Regions this revision leaves out may still share a page with
                // one it maps; only pages wholly inside are owed absence.
                bool shared = false;
                for (uint64_t j = 0; j < memmap->entry_count; j++) {
                    struct limine_memmap_entry *o = memmap->entries[j];
                    if (j != i && o->base < pa + 4096 && pa < o->base + o->length) {
                        shared = true;
                    }
                }
                if (mapped && !shared) {
                    ok = false;
                    kprintf("C FAIL hhdm.unmapped: %s region %lu page %lx is mapped under base revision %d\n",
                            memmap_type_name(e->type), i, pa, base_rev);
                    break;
                }
            }
        }
    }
    fact("hhdm.unmapped_samples", "%u", unmapped_checked);
    check(ok, "hhdm.mappings", "see above");

    // Base revision 0 alone keeps an identity map.
    struct pt_walk w;
    uint64_t probe = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->base >= 0x100000 && e->length >= 4096) {
            probe = e->base;
            break;
        }
    }
    if (probe != 0) {
        bool identity = arch_walk(probe, &w) && w.phys == probe;
        fact("identity_map", "%d", identity);
    }
#endif
}

__attribute__((unused)) static void check_stack(void) {
#if !TEST_MINIMAL
    uint64_t sp;
#if defined (__x86_64__)
    sp = bsp_state[ST_RSP];
    // The pushed return address is not part of the stack it was pushed on.
    sp += 8;
#elif defined (__aarch64__)
    sp = bsp_state[ST_SP];
#elif defined (__riscv)
    sp = bsp_state[ST_X0 + 2];
#elif defined (__loongarch64)
    sp = bsp_state[ST_R0 + 3];
#endif
    fact("stack.top", "%lx", sp);
    check(sp % 16 == 0, "stack.alignment", "stack top %lx is not 16 byte aligned", sp);
#if TEST_STACK_SIZE > 0
    fact("stack_size.response", "%d", stack_size_request.response != NULL);
    if (stack_size_request.response != NULL) {
        check_resp_ptr("stack_size.response_ptr", stack_size_request.response,
                       sizeof(*stack_size_request.response));
    }
    uint64_t want = TEST_STACK_SIZE > 65536 ? TEST_STACK_SIZE : 65536;
#else
    uint64_t want = 65536;
#endif
    if (!have_hhdm || memmap == NULL) {
        return;
    }
    bool ok = true;
    for (uint64_t va = sp - want; va < sp; va += 4096) {
        struct pt_walk w;
        if (!arch_walk(va, &w) || !w.writable) {
            ok = false;
            kprintf("C FAIL stack.mapped: page %lx of the stack is not mapped writable\n", va);
            break;
        }
        if (region_type(w.phys) != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            ok = false;
            kprintf("C FAIL stack.memmap: page %lx (phys %lx) is %s\n", va, w.phys,
                    memmap_type_name(region_type(w.phys)));
            break;
        }
    }
    check(ok, "stack.size", "need %lx bytes below %lx", want, sp);
#endif
}

static volatile uint32_t aps_in;

void ap_main(struct limine_mp_info *info) {
    arch_ap_hook((unsigned)info->extra_argument);
    __atomic_fetch_add(&aps_in, 1, __ATOMIC_SEQ_CST);
    halt();
}

#if !TEST_MINIMAL
static void check_framebuffers(void) {
    struct limine_framebuffer_response *r = framebuffer_request.response;
    fact("framebuffer.response", "%d", r != NULL);
    if (r == NULL) {
        return;
    }
    check_resp_ptr("framebuffer.response_ptr", r, sizeof(*r));
    fact("framebuffer.revision", "%lu", r->revision);
    fact("framebuffer.count", "%lu", r->framebuffer_count);
    check(r->framebuffer_count > 0, "framebuffer.count_nonzero", "a response with no framebuffers");
    check_resp_ptr("framebuffer.list_ptr", r->framebuffers, r->framebuffer_count * sizeof(void *));
    for (uint64_t i = 0; i < r->framebuffer_count; i++) {
        struct limine_framebuffer *fb = r->framebuffers[i];
        check_resp_ptr("framebuffer.struct_ptr", fb, sizeof(*fb));
        kprintf("K framebuffer.%lu=%lux%lux%u pitch=%lu model=%u r=%u:%u g=%u:%u b=%u:%u\n", i,
                fb->width, fb->height, fb->bpp, fb->pitch, fb->memory_model,
                fb->red_mask_size, fb->red_mask_shift, fb->green_mask_size, fb->green_mask_shift,
                fb->blue_mask_size, fb->blue_mask_shift);
        uint64_t addr = (uint64_t)(uintptr_t)fb->address;
        fact("framebuffer.address", "%lx", addr);
        check(is_hhdm_ptr(addr), "framebuffer.address_hhdm", "%lx", addr);
        check(fb->pitch >= fb->width * ((fb->bpp + 7) / 8), "framebuffer.pitch",
              "pitch %lu too small for %lu pixels of %u bits", fb->pitch, fb->width, fb->bpp);
        check(fb->memory_model == LIMINE_FRAMEBUFFER_RGB, "framebuffer.memory_model", "%u",
              fb->memory_model);
        check(fb->red_mask_shift + fb->red_mask_size <= fb->bpp
           && fb->green_mask_shift + fb->green_mask_size <= fb->bpp
           && fb->blue_mask_shift + fb->blue_mask_size <= fb->bpp,
              "framebuffer.masks", "masks exceed bpp");
        if (memmap != NULL && have_hhdm) {
            check(range_of_type(addr - hhdm_offset, fb->pitch * fb->height, LIMINE_MEMMAP_FRAMEBUFFER),
                  "framebuffer.memmap", "framebuffer memory is not in a framebuffer region");
        }
        fact("framebuffer.edid_size", "%lu", fb->edid_size);
        if (fb->edid != NULL) {
            check_resp_ptr("framebuffer.edid_ptr", fb->edid, fb->edid_size);
            fact("framebuffer.edid_hash", "%016lx", fnv1a(fb->edid, fb->edid_size));
            const uint8_t *edid = fb->edid;
            static const uint8_t edid_magic[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
            check(fb->edid_size >= 128 && memcmp(edid, edid_magic, 8) == 0, "framebuffer.edid_header",
                  "bad EDID header");
        } else {
            check(fb->edid_size == 0, "framebuffer.edid_null_size", "NULL EDID with size %lu",
                  fb->edid_size);
        }
        if (r->revision >= 1) {
            fact("framebuffer.mode_count", "%lu", fb->mode_count);
            check_resp_ptr("framebuffer.modes_ptr", fb->modes, fb->mode_count * sizeof(void *));
            bool modes_ok = true, current_listed = false;
            for (uint64_t m = 0; m < fb->mode_count; m++) {
                struct limine_video_mode *vm = fb->modes[m];
                kprintf("K framebuffer.%lu.mode.%lu=%lux%lux%u pitch=%lu\n", i, m, vm->width,
                        vm->height, vm->bpp, vm->pitch);
                if (vm->memory_model != LIMINE_FRAMEBUFFER_RGB
                 || vm->pitch < vm->width * ((vm->bpp + 7) / 8)) {
                    modes_ok = false;
                }
                if (vm->width == fb->width && vm->height == fb->height && vm->bpp == fb->bpp) {
                    current_listed = true;
                }
            }
            check(modes_ok, "framebuffer.modes", "a mode has a bad memory model or pitch");
            fact("framebuffer.current_mode_listed", "%d", current_listed);
        }
#if TEST_DRAW
        // A pattern the harness can find on screen: proof the address, pitch
        // and masks describe what is actually displayed.
        if (fb->bpp == 32 && fb->width >= 64 && fb->height >= 64) {
            static const uint32_t colours[4] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
            for (int q = 0; q < 4; q++) {
                uint32_t c = colours[q];
                uint32_t px = (((c >> 16) & 0xff) >> (8 - fb->red_mask_size)) << fb->red_mask_shift
                            | (((c >> 8) & 0xff) >> (8 - fb->green_mask_size)) << fb->green_mask_shift
                            | ((c & 0xff) >> (8 - fb->blue_mask_size)) << fb->blue_mask_shift;
                uint64_t x0 = (q & 1) ? fb->width - 16 : 0;
                uint64_t y0 = (q & 2) ? fb->height - 16 : 0;
                for (uint64_t y = y0; y < y0 + 16; y++) {
                    volatile uint32_t *row = (volatile uint32_t *)((uint8_t *)fb->address + y * fb->pitch);
                    for (uint64_t x = x0; x < x0 + 16; x++) {
                        row[x] = px;
                    }
                }
            }
        }
#endif
    }
}

static void check_modules(void) {
    struct limine_module_response *r = module_request.response;
    fact("module.response", "%d", r != NULL);
    if (r == NULL) {
        return;
    }
    check_resp_ptr("module.response_ptr", r, sizeof(*r));
    fact("module.revision", "%lu", r->revision);
    fact("module.count", "%lu", r->module_count);
    check_resp_ptr("module.list_ptr", r->modules, r->module_count * sizeof(void *));
    bool exclusive = true, in_region = true;
    for (uint64_t i = 0; i < r->module_count; i++) {
        struct limine_file *f = r->modules[i];
        char prefix[24] = "module.";
        prefix[7] = '0' + (i / 10) % 10;
        prefix[8] = '0' + i % 10;
        prefix[9] = 0;
        report_file(prefix, f);
        uint64_t a0 = (uint64_t)(uintptr_t)f->address;
        uint64_t a1 = (a0 + f->size + 4095) & ~(uint64_t)4095;
        if (f->size == 0) {
            a1 = a0 + 4096;
        }
        for (uint64_t j = 0; j < r->module_count; j++) {
            struct limine_file *g = r->modules[j];
            uint64_t b0 = (uint64_t)(uintptr_t)g->address;
            uint64_t b1 = (b0 + g->size + 4095) & ~(uint64_t)4095;
            if (g->size == 0) {
                b1 = b0 + 4096;
            }
            if (j != i && b0 < a1 && a0 < b1) {
                exclusive = false;
                kprintf("C FAIL module.exclusive_pages: modules %lu and %lu share pages\n", i, j);
            }
        }
        if (memmap != NULL && have_hhdm && f->size != 0
         && !range_of_type(a0 - hhdm_offset, f->size, LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)) {
            in_region = false;
            kprintf("C FAIL module.memmap: module %lu is not in executable_and_modules memory\n", i);
        }
    }
    check(exclusive, "module.exclusive", "see above");
    check(in_region, "module.region", "see above");
}

static bool checksum_ok(const void *p, size_t len) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += b[i];
    }
    return sum == 0;
}

// What a firmware table pointer must be under this base revision: HHDM, or
// physical for the revisions that said so.
static bool table_ptr_is_phys(const char *which) {
    if (which[0] == 'r') {
        return base_rev == 3;
    }
    return base_rev == 3 || base_rev == 4;
}

static const void *table_access(const char *which, uint64_t p) {
    if (!have_hhdm) {
        return NULL;
    }
    uint64_t phys = table_ptr_is_phys(which) ? p : p - hhdm_offset;
    struct pt_walk w;
    if (!arch_walk(hhdm_offset + phys, &w)) {
        return NULL;
    }
    return (const void *)(uintptr_t)(hhdm_offset + phys);
}

static uint64_t rd_le(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    memcpy(&v, p, n);
    return v;
}

static const uint8_t *phys_access(uint64_t phys) {
    struct pt_walk w;
    if (!have_hhdm || !arch_walk(hhdm_offset + phys, &w)) {
        return NULL;
    }
    return (const uint8_t *)(uintptr_t)(hhdm_offset + phys);
}

// The ACPI BGRT (ACPI 6.6 5.2.23), and the size its BMP image says it is.
static void report_bgrt(uint64_t rsdp_phys) {
    const uint8_t *rsdp = phys_access(rsdp_phys);
    if (rsdp == NULL) {
        return;
    }
    bool xsdt = rsdp[15] >= 2;
    const uint8_t *root = phys_access(xsdt ? rd_le(rsdp + 24, 8) : rd_le(rsdp + 16, 4));
    if (root == NULL) {
        return;
    }
    size_t esize = xsdt ? 8 : 4;
    for (size_t off = 36; off + esize <= rd_le(root + 4, 4); off += esize) {
        const uint8_t *t = phys_access(rd_le(root + off, esize));
        if (t == NULL || memcmp(t, "BGRT", 4) != 0) {
            continue;
        }
        fact("bgrt.checksum_ok", "%d", checksum_ok(t, rd_le(t + 4, 4)));
        fact("bgrt.version", "%lu", rd_le(t + 36, 2));
        fact("bgrt.status", "%u", t[38]);
        fact("bgrt.offset", "%lu %lu", rd_le(t + 48, 4), rd_le(t + 52, 4));
        const uint8_t *bmp = phys_access(rd_le(t + 40, 8));
        if (bmp != NULL && bmp[0] == 'B' && bmp[1] == 'M' && rd_le(bmp + 14, 4) >= 40) {
            int32_t height = (int32_t)rd_le(bmp + 22, 4);
            fact("bgrt.image", "%lu %d", rd_le(bmp + 18, 4), height < 0 ? -height : height);
        }
        return;
    }
    fact("bgrt.status", "none");
}

static uint64_t check_firmware_tables(void) {
    uint64_t rsdp_phys = 0;
    struct limine_rsdp_response *rr = rsdp_request.response;
    fact("rsdp.response", "%d", rr != NULL);
    if (rr != NULL) {
        check_resp_ptr("rsdp.response_ptr", rr, sizeof(*rr));
        uint64_t p = (uint64_t)(uintptr_t)rr->address;
        fact("rsdp.address", "%lx", p);
        if (have_hhdm) {
            check(table_ptr_is_phys("rsdp") ? p < hhdm_offset : is_hhdm_ptr(p), "rsdp.address_kind",
                  "%lx under base revision %d", p, base_rev);
            rsdp_phys = table_ptr_is_phys("rsdp") ? p : p - hhdm_offset;
        }
        const uint8_t *rsdp = table_access("rsdp", p);
        fact("rsdp.readable", "%d", rsdp != NULL);
        if (rsdp != NULL) {
            check(memcmp(rsdp, "RSD PTR ", 8) == 0 && checksum_ok(rsdp, 20), "rsdp.valid",
                  "bad signature or checksum");
            fact("rsdp.revision", "%u", rsdp[15]);
            if (base_rev >= 4 && memmap != NULL) {
                int64_t t = region_type(rsdp_phys);
                check(t == LIMINE_MEMMAP_ACPI_RECLAIMABLE || t == LIMINE_MEMMAP_ACPI_NVS
                   || t == LIMINE_MEMMAP_RESERVED_MAPPED, "rsdp.memmap", "RSDP in %s memory",
                      memmap_type_name(t));
            }
        }
    }

    struct limine_smbios_response *sr = smbios_request.response;
    fact("smbios.response", "%d", sr != NULL);
    if (sr != NULL) {
        check_resp_ptr("smbios.response_ptr", sr, sizeof(*sr));
        fact("smbios.entry_32", "%lx", (uint64_t)(uintptr_t)sr->entry_32);
        fact("smbios.entry_64", "%lx", (uint64_t)(uintptr_t)sr->entry_64);
        check(sr->entry_32 != NULL || sr->entry_64 != NULL, "smbios.some_entry", "both NULL");
        if (sr->entry_32 != NULL) {
            const uint8_t *e = table_access("smbios", (uint64_t)(uintptr_t)sr->entry_32);
            if (e != NULL) {
                check(memcmp(e, "_SM_", 4) == 0 && checksum_ok(e, e[5]), "smbios.entry_32_valid",
                      "bad anchor or checksum");
            }
            if (have_hhdm) {
                uint64_t p = (uint64_t)(uintptr_t)sr->entry_32;
                check(table_ptr_is_phys("smbios") ? p < hhdm_offset : is_hhdm_ptr(p),
                      "smbios.entry_32_kind", "%lx under base revision %d", p, base_rev);
                if (base_rev >= 5 && memmap != NULL) {
                    check(region_type(p - hhdm_offset) == LIMINE_MEMMAP_RESERVED_MAPPED
                       || region_type(p - hhdm_offset) == LIMINE_MEMMAP_ACPI_RECLAIMABLE
                       || region_type(p - hhdm_offset) == LIMINE_MEMMAP_ACPI_NVS,
                          "smbios.entry_32_memmap", "in %s memory",
                          memmap_type_name(region_type(p - hhdm_offset)));
                }
            }
        }
        if (sr->entry_64 != NULL) {
            const uint8_t *e = table_access("smbios", (uint64_t)(uintptr_t)sr->entry_64);
            if (e != NULL) {
                check(memcmp(e, "_SM3_", 5) == 0 && checksum_ok(e, e[6]), "smbios.entry_64_valid",
                      "bad anchor or checksum");
            }
            if (have_hhdm) {
                uint64_t p = (uint64_t)(uintptr_t)sr->entry_64;
                check(table_ptr_is_phys("smbios") ? p < hhdm_offset : is_hhdm_ptr(p),
                      "smbios.entry_64_kind", "%lx under base revision %d", p, base_rev);
            }
        }
    }

    struct limine_efi_system_table_response *er = efi_system_table_request.response;
    fact("efi_system_table.response", "%d", er != NULL);
    if (er != NULL) {
        check_resp_ptr("efi_system_table.response_ptr", er, sizeof(*er));
        uint64_t p = (uint64_t)(uintptr_t)er->address;
        fact("efi_system_table.address", "%lx", p);
        const uint8_t *st = table_access("efi", p);
        if (st != NULL) {
            check(memcmp(st, "IBI SYST", 8) == 0, "efi_system_table.signature", "bad signature");
            fact("efi_system_table.revision", "%x", *(const uint32_t *)(st + 8));
        }
        if (have_hhdm) {
            check(table_ptr_is_phys("efi") ? p < hhdm_offset : is_hhdm_ptr(p),
                  "efi_system_table.address_kind", "%lx under base revision %d", p, base_rev);
            if (base_rev >= 5 && memmap != NULL) {
                check(region_type(p - hhdm_offset) == LIMINE_MEMMAP_RESERVED_MAPPED,
                      "efi_system_table.memmap", "in %s memory",
                      memmap_type_name(region_type(p - hhdm_offset)));
            }
        }
    }

    struct limine_efi_memmap_response *mr = efi_memmap_request.response;
    fact("efi_memmap.response", "%d", mr != NULL);
    if (mr != NULL) {
        check_resp_ptr("efi_memmap.response_ptr", mr, sizeof(*mr));
        check_resp_ptr("efi_memmap.memmap_ptr", mr->memmap, mr->memmap_size);
        fact("efi_memmap.size", "%lu", mr->memmap_size);
        fact("efi_memmap.desc_size", "%lu", mr->desc_size);
        fact("efi_memmap.desc_version", "%lu", mr->desc_version);
        check(mr->desc_size >= 40 && mr->memmap_size % mr->desc_size == 0, "efi_memmap.sizes",
              "size %lu desc_size %lu", mr->memmap_size, mr->desc_size);
        check(mr->desc_version == 1, "efi_memmap.version", "%lu", mr->desc_version);
    }
    if (rsdp_phys != 0) {
        report_bgrt(rsdp_phys);
    }
    return rsdp_phys;
}

static void check_dtb(void) {
    struct limine_dtb_response *r = dtb_request.response;
    fact("dtb.response", "%d", r != NULL);
    if (r == NULL) {
        return;
    }
    check_resp_ptr("dtb.response_ptr", r, sizeof(*r));
    const void *dtb = r->dtb_ptr;
    uint32_t size = fdt_totalsize(dtb);
    check_resp_ptr("dtb.blob_ptr", dtb, size);
    check(fdt_magic_ok(dtb), "dtb.magic", "bad magic");
    if (!fdt_magic_ok(dtb)) {
        return;
    }
    arch_dtb(dtb);
    fact("dtb.memory_nodes", "%d", fdt_count_root_nodes(dtb, "memory"));
    check(fdt_count_root_nodes(dtb, "memory") == 0, "dtb.no_memory_nodes", "memory nodes remain");
    int len;
    const char *marker = fdt_getprop_path(dtb, "/", "limine-test-marker", &len);
    fact_str("dtb.marker", marker != NULL ? marker : "(none)");
    const char *model = fdt_getprop_path(dtb, "/", "model", &len);
    fact_str("dtb.model", model != NULL ? model : "(none)");
}

static void check_misc_features(void) {
    struct limine_bootloader_info_response *bi = bootloader_info_request.response;
    fact("bootloader_info.response", "%d", bi != NULL);
    if (bi != NULL) {
        check_resp_ptr("bootloader_info.response_ptr", bi, sizeof(*bi));
        fact("bootloader_info.revision", "%lu", bi->revision);
        fact_str("bootloader_info.name", bi->name);
        fact_str("bootloader_info.version", bi->version);
        check(is_hhdm_ptr((uint64_t)(uintptr_t)bi->name) || !have_hhdm, "bootloader_info.name_ptr",
              "%p", bi->name);
    }

    struct limine_executable_cmdline_response *cl = cmdline_request.response;
    fact("cmdline.response", "%d", cl != NULL);
    if (cl != NULL) {
        check_resp_ptr("cmdline.response_ptr", cl, sizeof(*cl));
        check_resp_ptr("cmdline.string_ptr", cl->cmdline, strlen(cl->cmdline) + 1);
        fact_str("cmdline", cl->cmdline);
    }

    struct limine_executable_file_response *ef = executable_file_request.response;
    fact("executable_file.response", "%d", ef != NULL);
    if (ef != NULL) {
        check_resp_ptr("executable_file.response_ptr", ef, sizeof(*ef));
        report_file("executable_file", ef->executable_file);
        if (cl != NULL) {
            check(cl->cmdline == ef->executable_file->string, "cmdline.same_memory",
                  "cmdline %p, executable_file string %p", cl->cmdline,
                  ef->executable_file->string);
        }
    }

#if !TEST_REQUEST_OUTSIDE_MARKERS
    struct limine_firmware_type_response *ft = firmware_type_request.response;
    fact("firmware_type.response", "%d", ft != NULL);
    if (ft != NULL) {
        check_resp_ptr("firmware_type.response_ptr", ft, sizeof(*ft));
        fact("firmware_type", "%lu", ft->firmware_type);
    }
#endif

    struct limine_date_at_boot_response *db = date_at_boot_request.response;
    fact("date_at_boot.response", "%d", db != NULL);
    if (db != NULL) {
        check_resp_ptr("date_at_boot.response_ptr", db, sizeof(*db));
        fact("date_at_boot", "%ld", db->timestamp);
    }

    struct limine_bootloader_performance_response *pf = perf_request.response;
    fact("performance.response", "%d", pf != NULL);
    if (pf != NULL) {
        check_resp_ptr("performance.response_ptr", pf, sizeof(*pf));
        fact("performance.reset_usec", "%lu", pf->reset_usec);
        fact("performance.init_usec", "%lu", pf->init_usec);
        fact("performance.exec_usec", "%lu", pf->exec_usec);
        check(pf->reset_usec <= pf->init_usec && pf->init_usec <= pf->exec_usec,
              "performance.order", "reset %lu init %lu exec %lu", pf->reset_usec, pf->init_usec,
              pf->exec_usec);
    }

#if TEST_KEEP_IOMMU_REQUEST
    fact("keep_iommu.response", "%d", keep_iommu_request.response != NULL);
    if (keep_iommu_request.response != NULL) {
        check_resp_ptr("keep_iommu.response_ptr", keep_iommu_request.response,
                       sizeof(*keep_iommu_request.response));
    }
#endif

    struct limine_tsc_frequency_response *tf = tsc_frequency_request.response;
    fact("tsc_frequency.response", "%d", tf != NULL);
    if (tf != NULL) {
        check_resp_ptr("tsc_frequency.response_ptr", tf, sizeof(*tf));
        fact("tsc_frequency", "%lu", tf->frequency);
        uint64_t hw = arch_counter_hw_freq();
        fact("tsc_frequency.hardware", "%lu", hw);
        if (hw != 0) {
            check(tf->frequency == hw, "tsc_frequency.matches_hardware",
                  "reported %lu, hardware says %lu", tf->frequency, hw);
        }
    }

#if TEST_ENTROPY_COUNT >= 0
    struct limine_entropy_response *en = entropy_request.response;
    fact("entropy.response", "%d", en != NULL);
    if (en != NULL) {
        check_resp_ptr("entropy.response_ptr", en, sizeof(*en));
        fact("entropy.count", "%lu", en->value_count);
        if (en->value_count == 0) {
            check(en->values == NULL, "entropy.null_values", "values %p for count 0", en->values);
        } else {
            check_resp_ptr("entropy.values_ptr", en->values, en->value_count * 8);
            bool distinct = true, nonzero = false;
            for (uint64_t i = 0; i < en->value_count; i++) {
                if (en->values[i] != 0) {
                    nonzero = true;
                }
                for (uint64_t j = i + 1; j < en->value_count && j < 64; j++) {
                    if (en->values[i] == en->values[j]) {
                        distinct = false;
                    }
                }
            }
            check(nonzero && distinct, "entropy.values", "values look degenerate");
            fact("entropy.first", "%016lx", en->values[0]);
        }
    }
#endif

#if TEST_ENTRY_REQUEST
    fact("entry_point.response", "%d", entry_point_request.response != NULL);
    if (entry_point_request.response != NULL) {
        check_resp_ptr("entry_point.response_ptr", entry_point_request.response,
                       sizeof(*entry_point_request.response));
    }
#endif
}

static void check_flanterm(void) {
#if TEST_FLANTERM_REQUEST
    struct limine_flanterm_fb_init_params_response *r = flanterm_request.response;
    fact("flanterm.response", "%d", r != NULL);
    if (r == NULL) {
        return;
    }
    check_resp_ptr("flanterm.response_ptr", r, sizeof(*r));
    fact("flanterm.count", "%lu", r->entry_count);
    struct limine_framebuffer_response *fbr = framebuffer_request.response;
    if (fbr != NULL) {
        check(r->entry_count == fbr->framebuffer_count, "flanterm.count_matches",
              "%lu entries, %lu framebuffers", r->entry_count, fbr->framebuffer_count);
    }
    for (uint64_t i = 0; i < r->entry_count; i++) {
        struct limine_flanterm_fb_init_params *p = r->entries[i];
        check_resp_ptr("flanterm.entry_ptr", p, sizeof(*p));
        kprintf("K flanterm.%lu.canvas_size=%lu\n", i, p->canvas_size);
        kprintf("K flanterm.%lu.canvas=%d\n", i, p->canvas != NULL);
        if (p->canvas != NULL) {
            check_resp_ptr("flanterm.canvas_ptr", p->canvas, p->canvas_size);
            kprintf("K flanterm.%lu.canvas_hash=%016lx\n", i, fnv1a(p->canvas, p->canvas_size));
        }
        kprintf("K flanterm.%lu.ansi=", i);
        for (int c = 0; c < 8; c++) {
            kprintf("%06x%s", p->ansi_colours[c], c < 7 ? " " : "\n");
        }
        kprintf("K flanterm.%lu.ansi_bright=", i);
        for (int c = 0; c < 8; c++) {
            kprintf("%06x%s", p->ansi_bright_colours[c], c < 7 ? " " : "\n");
        }
        kprintf("K flanterm.%lu.default=%08x %08x %08x %08x\n", i, p->default_bg, p->default_fg,
                p->default_bg_bright, p->default_fg_bright);
        kprintf("K flanterm.%lu.font=%lux%lu spacing=%lu scale=%lux%lu margin=%lu rotation=%lu\n", i,
                p->font_width, p->font_height, p->font_spacing, p->font_scale_x, p->font_scale_y,
                p->margin, p->rotation);
        if (p->font != NULL) {
            uint64_t font_size = p->font_width * p->font_height * 256 / 8;
            check_resp_ptr("flanterm.font_ptr", p->font, font_size);
            kprintf("K flanterm.%lu.font_hash=%016lx\n", i, fnv1a(p->font, font_size));
        }
    }
#endif
}

static void check_tpm(void) {
    struct limine_tpm_event_log_response *r = tpm_event_log_request.response;
    fact("tpm_event_log.response", "%d", r != NULL);
    if (r == NULL) {
        return;
    }
    check_resp_ptr("tpm_event_log.response_ptr", r, sizeof(*r));
    fact("tpm_event_log.format", "%lu", r->format);
    fact("tpm_event_log.size", "%lu", r->size);
    if (r->size == 0) {
        check(r->address == NULL, "tpm_event_log.null", "address %p for size 0", r->address);
        return;
    }
    check_resp_ptr("tpm_event_log.address_ptr", r->address, r->size);
    if (r->format == LIMINE_TPM_EVENT_LOG_FORMAT_TCG_2) {
        tpm_dump_log(r->address, r->size);
    }
}

static void check_mp(void) {
#if TEST_MP_FLAGS >= 0
    struct limine_mp_response *r = mp_request.response;
    fact("mp.response", "%d", r != NULL);
    if (r == NULL) {
        return;
    }
    check_resp_ptr("mp.response_ptr", r, sizeof(*r));
    check_resp_ptr("mp.cpus_ptr", r->cpus, r->cpu_count * sizeof(void *));
    fact("mp.revision", "%lu", r->revision);
    fact("mp.cpu_count", "%lu", r->cpu_count);
    fact("mp.flags", "%lx", (uint64_t)r->flags);
#if defined (__x86_64__)
    fact("mp.bsp_id", "%x", r->bsp_lapic_id);
    uint64_t bsp_id = r->bsp_lapic_id;
#elif defined (__aarch64__)
    fact("mp.bsp_id", "%lx", r->bsp_mpidr);
    uint64_t bsp_id = r->bsp_mpidr;
#elif defined (__riscv)
    fact("mp.bsp_id", "%lx", r->bsp_hartid);
    uint64_t bsp_id = r->bsp_hartid;
#elif defined (__loongarch64)
    fact("mp.bsp_id", "%lx", r->bsp_phys_id);
    uint64_t bsp_id = r->bsp_phys_id;
#endif
    bool goto_null = true;
    int bsp_entries = 0;
    unsigned slot = 0;
    for (uint64_t i = 0; i < r->cpu_count; i++) {
        struct limine_mp_info *info = r->cpus[i];
        check_resp_ptr("mp.info_ptr", info, sizeof(*info));
#if defined (__x86_64__)
        uint64_t id = info->lapic_id;
#elif defined (__aarch64__)
        uint64_t id = info->mpidr;
#elif defined (__riscv)
        uint64_t id = info->hartid;
#elif defined (__loongarch64)
        uint64_t id = info->phys_id;
#endif
        kprintf("K mp.cpu.%lu=id %lx processor_id %lx\n", i, id, (uint64_t)info->processor_id);
        if (info->goto_address != NULL) {
            goto_null = false;
        }
        if (id == bsp_id) {
            bsp_entries++;
        }
    }
    check(goto_null, "mp.goto_address_null", "a goto_address was not NULL at entry");
    check(bsp_entries == 1, "mp.bsp_listed", "BSP listed %d times", bsp_entries);

    // Release every AP and see each come up in the state the protocol promises.
    arch_prepare_aps();
    for (uint64_t i = 0; i < r->cpu_count && slot < MAX_APS; i++) {
        struct limine_mp_info *info = r->cpus[i];
#if defined (__x86_64__)
        if (info->lapic_id == bsp_id) {
            continue;
        }
#elif defined (__aarch64__)
        if (info->mpidr == bsp_id) {
            continue;
        }
#elif defined (__riscv)
        if (info->hartid == bsp_id) {
            continue;
        }
#elif defined (__loongarch64)
        if (info->phys_id == bsp_id) {
            continue;
        }
#endif
        info->extra_argument = slot++;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);
    }
    uint64_t start = arch_counter();
    uint64_t freq = arch_counter_hw_freq();
    if (freq == 0) {
        freq = 1000000000;
    }
    while (__atomic_load_n(&aps_in, __ATOMIC_ACQUIRE) < slot) {
        if (arch_counter() - start > freq * 20) {
            break;
        }
        arch_cpu_relax();
    }
    fact("mp.aps_started", "%u", aps_in);
    check(aps_in == slot, "mp.all_aps_started", "%u of %u APs reached their goto_address", aps_in, slot);
    slot = 0;
    for (uint64_t i = 0; i < r->cpu_count && slot < aps_in; i++) {
        struct limine_mp_info *info = r->cpus[i];
        if (info->extra_argument >= MAX_APS || r->cpus[i]->goto_address == NULL) {
            continue;
        }
        arch_check_ap(base_rev, (unsigned)info->extra_argument, info);
        slot++;
    }
#endif
}
#endif

static void report_base_revision(void) {
#if TEST_BASE_REVISION >= 0
    uint64_t t0 = base_revision[0], t1 = base_revision[1], t2 = base_revision[2];
    fact("base_revision.tag", "%lx %lx %lu", t0, t1, t2);
    fact("base_revision.supported", "%d", LIMINE_BASE_REVISION_SUPPORTED(base_revision));
    bool loaded_valid = LIMINE_LOADED_BASE_REVISION_VALID(base_revision);
    fact("base_revision.loaded", "%ld", loaded_valid ? (int64_t)LIMINE_LOADED_BASE_REVISION(base_revision) : -1);
    if (LIMINE_BASE_REVISION_SUPPORTED(base_revision)) {
        base_rev = TEST_BASE_REVISION;
    } else if (loaded_valid) {
        base_rev = (int)LIMINE_LOADED_BASE_REVISION(base_revision);
    } else {
        base_rev = 0;
    }
#else
    base_rev = 0;
#endif
    fact("base_revision.effective", "%d", base_rev);
}

void kmain(void) {
    arch_init_output();
    kprintf("\nB LIMINE-TEST variant=%s arch=%s\n", TEST_VARIANT, arch_name());
    fact("entry", "%s", entry_id == ENTRY_REQUEST ? "request" : entry_id == ENTRY_ELF ? "elf" : "unknown");
    report_base_revision();
#if TEST_SYMBOLS
    report_symbols();
#endif

#if !TEST_MINIMAL
    struct limine_hhdm_response *hr = hhdm_request.response;
    fact("hhdm.response", "%d", hr != NULL);
    if (hr != NULL) {
        hhdm_offset = hr->offset;
        have_hhdm = true;
        fact("hhdm.offset", "%lx", hhdm_offset);
        check_resp_ptr("hhdm.response_ptr", hr, sizeof(*hr));
        check(hhdm_offset % 4096 == 0, "hhdm.alignment", "%lx", hhdm_offset);
    }
    check_memmap();
#endif

    uint64_t entry_point = (uint64_t)(uintptr_t)(TEST_ENTRY_REQUEST && !TEST_MINIMAL ? (void *)entry_req : (void *)0);
    arch_check_entry(base_rev, entry_point);

#if !TEST_MINIMAL
#if TEST_ENTRY_REQUEST
    check(entry_id == ENTRY_REQUEST, "entry_point.used", "entered through the ELF entry point");
#else
    check(entry_id == ENTRY_ELF, "entry_point.elf", "entered somewhere other than the ELF entry point");
#endif

#if TEST_PAGING_REV >= 0
    struct limine_paging_mode_response *pm = paging_mode_request.response;
    fact("paging_mode.response", "%d", pm != NULL);
    if (pm != NULL) {
        check_resp_ptr("paging_mode.response_ptr", pm, sizeof(*pm));
        fact("paging_mode", "%lu", pm->mode);
        check(pm->mode == arch_paging_mode(), "paging_mode.in_force",
              "response says %lu, hardware is in %lu", pm->mode, arch_paging_mode());
    }
#endif
    fact("paging_mode.hardware", "%lu", arch_paging_mode());

#if defined (__riscv)
    struct limine_riscv_bsp_hartid_response *bh = bsp_hartid_request.response;
    fact("riscv_bsp_hartid.response", "%d", bh != NULL);
    if (bh != NULL) {
        check_resp_ptr("riscv_bsp_hartid.response_ptr", bh, sizeof(*bh));
        fact("riscv_bsp_hartid", "%lu", bh->bsp_hartid);
    }
#endif

    check_dtb();
    check_kernel_mapping();
    check_hhdm_mappings();
    check_stack();
    check_misc_features();
    check_modules();
    uint64_t rsdp_phys = check_firmware_tables();
    check_flanterm();
    check_framebuffers();
    check_tpm();
    check_mp();
    arch_check_late(base_rev, rsdp_phys);
#else
    arch_check_late(base_rev, 0);
#endif

#if TEST_REQUEST_OUTSIDE_MARKERS
    fact("outside_request.response", "%d", outside_request.response != NULL);
#endif

    report_done();
    halt();
}
