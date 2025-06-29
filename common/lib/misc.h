#ifndef LIB__MISC_H__
#define LIB__MISC_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <fs/file.h>
#include <lib/part.h>
#include <lib/libc.h>
#if defined (UEFI)
#  include <efi.h>
#  if defined (__riscv)
#    include <efi/protocol/riscv/efiboot.h>
#  endif
#endif

#if defined (UEFI)
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_RUNTIME_SERVICES *gRT;
extern EFI_HANDLE efi_image_handle;
extern EFI_MEMORY_DESCRIPTOR *efi_mmap;
extern UINTN efi_mmap_size, efi_desc_size, efi_mmap_key;
extern UINT32 efi_desc_ver;

extern bool efi_boot_services_exited;
bool efi_exit_boot_services(void);

bool is_efi_serial_present(void);
#endif

void *get_device_tree_blob(const char *config, size_t extra_size);

extern struct volume *boot_volume;

#if defined (BIOS)
extern bool stage3_loaded;
#endif

extern bool quiet, serial, editor_enabled, help_hidden, hash_mismatch_panic;

extern uint64_t usec_at_bootloader_entry;

bool parse_resolution(size_t *width, size_t *height, size_t *bpp, const char *buf);

void get_absolute_path(char *path_ptr, const char *path, const char *pwd);

uint32_t oct2bin(uint8_t *str, uint32_t max);
uint32_t hex2bin(uint8_t *str, uint32_t size);

uint64_t sqrt(uint64_t a_nInput);
size_t get_trailing_zeros(uint64_t val);

int digit_to_int(char c);
uint8_t bcd_to_int(uint8_t val);
uint8_t int_to_bcd(uint8_t val);

noreturn void panic(bool allow_menu, const char *fmt, ...);

int pit_sleep_and_quit_on_keypress(int seconds);

uint64_t strtoui(const char *s, const char **end, int base);

#define MIN(a, b) ({ \
    __auto_type MIN_a = (a); \
    __auto_type MIN_b = (b); \
    MIN_a > MIN_b ? MIN_b : MIN_a; \
})

#define MAX(a, b) ({ \
    __auto_type MAX_a = (a); \
    __auto_type MAX_b = (b); \
    MAX_a > MAX_b ? MAX_a : MAX_b; \
})

#define DIV_ROUNDUP(a, b) ({ \
    __auto_type DIV_ROUNDUP_a = (a); \
    __auto_type DIV_ROUNDUP_b = (b); \
    (DIV_ROUNDUP_a + (DIV_ROUNDUP_b - 1)) / DIV_ROUNDUP_b; \
})

#define ALIGN_UP(x, a) ({ \
    __auto_type ALIGN_UP_value = (x); \
    __auto_type ALIGN_UP_align = (a); \
    ALIGN_UP_value = DIV_ROUNDUP(ALIGN_UP_value, ALIGN_UP_align) * ALIGN_UP_align; \
    ALIGN_UP_value; \
})

#define ALIGN_DOWN(x, a) ({ \
    __auto_type ALIGN_DOWN_value = (x); \
    __auto_type ALIGN_DOWN_align = (a); \
    ALIGN_DOWN_value = (ALIGN_DOWN_value / ALIGN_DOWN_align) * ALIGN_DOWN_align; \
    ALIGN_DOWN_value; \
})

#define SIZEOF_ARRAY(array) (sizeof(array) / sizeof(array[0]))

typedef char symbol[];

noreturn void stage3_common(void);

#if defined (__x86_64__) || defined (__i386__)
noreturn void common_spinup(void *fnptr, int args, ...);
#elif defined (__aarch64__)
noreturn void enter_in_el1(uint64_t entry, uint64_t sp, uint64_t sctlr,
                           uint64_t mair, uint64_t tcr, uint64_t ttbr0,
                           uint64_t ttbr1, uint64_t target_x0);
#elif defined (__riscv)
noreturn void riscv_spinup(uint64_t entry, uint64_t sp, uint64_t satp, uint64_t direct_map_offset);
#if defined (UEFI)
RISCV_EFI_BOOT_PROTOCOL *get_riscv_boot_protocol(void);
#endif
#elif defined (__loongarch64)
noreturn void loongarch_spinup(uint64_t entry, uint64_t sp, uint64_t pgdl,
                               uint64_t pgdh, uint64_t direct_map_offset);
#else
#error Unknown architecture
#endif

#define no_unwind __attribute__((section(".no_unwind")))

#define MEM_RANGE_X 1
#define MEM_RANGE_W 2
#define MEM_RANGE_R 4

struct mem_range {
    uint64_t base;
    uint64_t length;
    uint64_t permissions;
};

#endif
