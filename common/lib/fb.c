#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/fb.h>
#include <lib/misc.h>
#include <drivers/vbe.h>
#include <drivers/gop.h>
#include <mm/pmm.h>
#include <mm/mtrr.h>
#include <mm/efi_pt.h>
#include <sys/cpu.h>

struct fb_info *fb_fbs;
size_t fb_fbs_count = 0;

void fb_init(struct fb_info **ret, size_t *_fbs_count,
             uint64_t target_width, uint64_t target_height, uint16_t target_bpp,
             bool preserve_screen, bool keep_wc) {
    if (quiet) {
        preserve_screen = true;
    }

#if defined (BIOS)
    *ret = ext_mem_alloc(sizeof(struct fb_info));
    if (init_vbe(*ret, target_width, target_height, target_bpp)) {
        *_fbs_count = 1;

        (*ret)->edid = get_edid_info();
        size_t mode_count;
        (*ret)->mode_list = vbe_get_mode_list(&mode_count);
        (*ret)->mode_count = mode_count;
    } else {
        *_fbs_count = 0;
        pmm_free(*ret, sizeof(struct fb_info));
    }
#elif defined (UEFI)
    init_gop(ret, _fbs_count, target_width, target_height, target_bpp);
#endif

    fb_fbs = *ret;
    fb_fbs_count = *_fbs_count;

    // Map the framebuffers as write-combining so the clear (and, when kept,
    // terminal rendering) is fast. keep_wc leaves it active for the caller.
    bool want_wc = keep_wc || !preserve_screen;

#if defined (__i386__) || defined (__x86_64__)
    if (want_wc) {
        for (size_t i = 0; i < *_fbs_count; i++) {
            uint64_t fb_size = CHECKED_MUL((uint64_t)(*ret)[i].framebuffer_pitch,
                                           (uint64_t)(*ret)[i].framebuffer_height, continue);
            if (fb_size == 0) {
                continue;
            }
#if defined (__x86_64__) && defined (UEFI)
            efi_pt_set_fb_wc((*ret)[i].framebuffer_addr, fb_size);
#else
            mtrr_wc_add_fb_range((*ret)[i].framebuffer_addr, fb_size);
#endif
        }
    }
#endif

    if (!preserve_screen) {
        for (size_t i = 0; i < *_fbs_count; i++) {
            fb_clear(&(*ret)[i]);
        }
    }

#if defined (__i386__) || defined (__x86_64__)
    if (want_wc && !keep_wc) {
#if defined (__x86_64__) && defined (UEFI)
        efi_pt_restore();
#else
        mtrr_restore();
#endif
    }
#else
    (void)want_wc;
#endif
}

void fb_clear(struct fb_info *fb) {
    for (size_t y = 0; y < fb->framebuffer_height; y++) {
        switch (fb->framebuffer_bpp) {
            case 32: {
                uint32_t *fbp = (void *)(uintptr_t)fb->framebuffer_addr;
                size_t row = (y * fb->framebuffer_pitch) / 4;
                for (size_t x = 0; x < fb->framebuffer_width; x++) {
                    fbp[row + x] = 0;
                }
                break;
            }
            case 16: {
                uint16_t *fbp = (void *)(uintptr_t)fb->framebuffer_addr;
                size_t row = (y * fb->framebuffer_pitch) / 2;
                for (size_t x = 0; x < fb->framebuffer_width; x++) {
                    fbp[row + x] = 0;
                }
                break;
            }
            default: {
                uint8_t *fbp = (void *)(uintptr_t)fb->framebuffer_addr;
                size_t row = y * fb->framebuffer_pitch;
                size_t row_bytes = fb->framebuffer_width * (fb->framebuffer_bpp / 8);
                for (size_t x = 0; x < row_bytes; x++) {
                    fbp[row + x] = 0;
                }
                break;
            }
        }
    }

    fb_flush((volatile void *)(uintptr_t)fb->framebuffer_addr,
             (size_t)fb->framebuffer_pitch * fb->framebuffer_height);
}

#if defined (__aarch64__)
static void fb_flush_aarch64(volatile void *base, size_t length) {
    clean_dcache_poc((uintptr_t)base, CHECKED_ADD((uintptr_t)base, length, return));
}
#elif defined (__riscv)
__attribute__((target("arch=+zicbom")))
static void fb_flush_riscv(volatile void *base, size_t length) {
    const size_t cbom_block_size = 0x40;
    uintptr_t start = ALIGN_DOWN((uintptr_t)base, cbom_block_size);
    uintptr_t end = ALIGN_UP(CHECKED_ADD((uintptr_t)base, length, return), cbom_block_size, return);
    for (uintptr_t ptr = start; ptr < end; ptr += cbom_block_size) {
        asm volatile("cbo.flush (%0)" :: "r"(ptr) : "memory");
    }
    asm volatile ("fence rw, rw" ::: "memory");
}

static void fb_flush_riscv_nozicbom(volatile void *base, size_t length) {
    (void)base;
    (void)length;

    // Without Zicbom, there is no portable instruction to flush dirty cache lines.
    // Read through a dedicated eviction buffer to create cache pressure and displace
    // dirty framebuffer lines. 128 KB covers typical RISC-V L1 D-caches (32-64 KB).
    static volatile uint8_t *eviction_buf = NULL;
    #define EVICTION_BUF_SIZE (128 * 1024)
    if (eviction_buf == NULL) {
        eviction_buf = ext_mem_alloc(EVICTION_BUF_SIZE);
    }

    volatile uint64_t *p = (volatile uint64_t *)eviction_buf;
    for (size_t i = 0; i < EVICTION_BUF_SIZE / sizeof(uint64_t); i += (64 / sizeof(uint64_t))) {
        (void)p[i];
    }
    asm volatile ("fence rw, rw" ::: "memory");
}
#elif defined (__loongarch64)
static void fb_flush_loongarch64(volatile void *base, size_t length) {
    // cacop Hit_Writeback_Inv_LEAF0 = 0x10 (D-cache L1 writeback+invalidate)
    const size_t clsz = 64;
    uintptr_t start = ALIGN_DOWN((uintptr_t)base, clsz);
    uintptr_t end = ALIGN_UP(CHECKED_ADD((uintptr_t)base, length, return), clsz, return);
    for (uintptr_t ptr = start; ptr < end; ptr += clsz) {
        asm volatile ("cacop 0x10, %0, 0" :: "r"(ptr) : "memory");
    }
}
#endif

void fb_flush(volatile void *base, size_t length) {
    typedef void (*flush_fn)(volatile void *, size_t);
    static flush_fn fn = NULL;

    if (fn == NULL) {
#if defined (__aarch64__)
        fn = fb_flush_aarch64;
#elif defined (__riscv)
        if (riscv_check_isa_extension("zicbom", NULL, NULL)) {
            fn = fb_flush_riscv;
        } else {
            fn = fb_flush_riscv_nozicbom;
        }
#elif defined (__loongarch64)
        fn = fb_flush_loongarch64;
#endif
    }

    if (fn != NULL) {
        fn(base, length);
    }
}
