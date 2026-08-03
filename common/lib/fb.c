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
#include <lib/bgrt.h>

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

#if defined (UEFI)
    if (!preserve_screen && *_fbs_count > 0) {
        bgrt_restore((*ret)[0].framebuffer_width, (*ret)[0].framebuffer_height);
    }
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
static bool fb_flush_aarch64(volatile void *base, size_t length) {
    clean_dcache_poc((uintptr_t)base, CHECKED_ADD((uintptr_t)base, length, return false));
    return true;
}
#elif defined (__riscv)
__attribute__((target("arch=+zicbom")))
static bool fb_flush_riscv(volatile void *base, size_t length) {
    const size_t cbom_block_size = riscv_cbom_block_size();
    uintptr_t start = ALIGN_DOWN((uintptr_t)base, cbom_block_size);
    uintptr_t end = ALIGN_UP(CHECKED_ADD((uintptr_t)base, length, return false), cbom_block_size, return false);
    for (uintptr_t ptr = start; ptr < end; ptr += cbom_block_size) {
        asm volatile("cbo.flush (%0)" :: "r"(ptr) : "memory");
    }
    asm volatile ("fence rw, rw" ::: "memory");
    return true;
}

#elif defined (__loongarch64)
// cacop's code[2:0] names a cache in the order CPUCFG 0x10 lists them, one leaf
// per present bit (manual section 4.2.3.1), so the numbering is a property of
// the part rather than of the architecture.
#define LOONGARCH_CACHE_CFG 0x10
#define LOONGARCH_L1_IU_PRESENT ((uint32_t)1 << 0)
#define LOONGARCH_L1_IU_UNIFY ((uint32_t)1 << 1)
#define LOONGARCH_L1_D_PRESENT ((uint32_t)1 << 2)
// Levels two and up repeat one layout every seven bits from bit 3.
#define LOONGARCH_LX_FIRST_BIT 3
#define LOONGARCH_LX_BITS 7
#define LOONGARCH_LX_IU_PRESENT ((uint32_t)1 << 0)
#define LOONGARCH_LX_IU_UNIFY ((uint32_t)1 << 1)
#define LOONGARCH_LX_IU_INCLUSIVE ((uint32_t)1 << 3)
#define LOONGARCH_LX_D_PRESENT ((uint32_t)1 << 4)
#define LOONGARCH_LX_D_INCLUSIVE ((uint32_t)1 << 6)
#define LOONGARCH_MAX_LEAVES 6

// Where a writeback lands is decided by the inclusion relations between levels,
// so maintaining one leaf does not reach memory by itself. An instruction cache
// holds no data and is never written back, and a leaf that contains every level
// below it makes maintaining those levels redundant.
static uint32_t loongarch_writeback_leaves(void) {
    uint32_t cfg = loongarch_cpucfg(LOONGARCH_CACHE_CFG);
    uint32_t mask = 0;
    unsigned leaf = 0;
    unsigned outermost = 0;
    bool outermost_inclusive = false;

    if (cfg & LOONGARCH_L1_IU_PRESENT) {
        if (cfg & LOONGARCH_L1_IU_UNIFY) {
            mask |= (uint32_t)1 << leaf;
            outermost = leaf;
        }
        leaf++;
    }

    if (cfg & LOONGARCH_L1_D_PRESENT) {
        mask |= (uint32_t)1 << leaf;
        outermost = leaf;
        leaf++;
    }

    for (uint32_t lx = cfg >> LOONGARCH_LX_FIRST_BIT;
         lx != 0 && leaf < LOONGARCH_MAX_LEAVES; lx >>= LOONGARCH_LX_BITS) {
        if (lx & LOONGARCH_LX_IU_PRESENT) {
            if (lx & LOONGARCH_LX_IU_UNIFY) {
                mask |= (uint32_t)1 << leaf;
                outermost = leaf;
                outermost_inclusive = (lx & LOONGARCH_LX_IU_INCLUSIVE) != 0;
            }
            leaf++;
        }

        if ((lx & LOONGARCH_LX_D_PRESENT) && leaf < LOONGARCH_MAX_LEAVES) {
            mask |= (uint32_t)1 << leaf;
            outermost = leaf;
            outermost_inclusive = (lx & LOONGARCH_LX_D_INCLUSIVE) != 0;
            leaf++;
        }
    }

    if (outermost_inclusive) {
        return (uint32_t)1 << outermost;
    }

    return mask;
}

// cacop takes the cache as an immediate, so each leaf needs its own loop.
#define LOONGARCH_WRITEBACK(code) \
    for (uintptr_t ptr = start; ptr < end; ptr += clsz) { \
        asm volatile ("cacop " code ", %0, 0" :: "r"(ptr) : "memory"); \
    }

static bool fb_flush_loongarch64(volatile void *base, size_t length) {
    static uint32_t leaves = 0;
    static bool probed = false;

    if (!probed) {
        leaves = loongarch_writeback_leaves();
        probed = true;
    }

    const size_t clsz = 64;
    uintptr_t start = ALIGN_DOWN((uintptr_t)base, clsz);
    uintptr_t end = ALIGN_UP(CHECKED_ADD((uintptr_t)base, length, return false), clsz, return false);

    // Hit-mode cacop probes the cache like a load and acts only on a hit, and the
    // manual gives no ordering between it and prior stores, so drain them first.
    asm volatile ("dbar 0" ::: "memory");

    for (unsigned leaf = 0; leaf < LOONGARCH_MAX_LEAVES; leaf++) {
        if (!(leaves & ((uint32_t)1 << leaf))) {
            continue;
        }

        switch (leaf) {
            case 0: {
                LOONGARCH_WRITEBACK("0x10");
                break;
            }
            case 1: {
                LOONGARCH_WRITEBACK("0x11");
                break;
            }
            case 2: {
                LOONGARCH_WRITEBACK("0x12");
                break;
            }
            case 3: {
                LOONGARCH_WRITEBACK("0x13");
                break;
            }
            case 4: {
                LOONGARCH_WRITEBACK("0x14");
                break;
            }
            case 5: {
                LOONGARCH_WRITEBACK("0x15");
                break;
            }
            default: {
                break;
            }
        }
    }

    asm volatile ("dbar 0" ::: "memory");
    return true;
}
#endif

bool fb_flush_reliable(void) {
    static bool probed = false;
    static bool reliable = true;

    if (!probed) {
#if defined (__riscv)
        reliable = riscv_check_isa_extension("zicbom", NULL, NULL);
#endif
        probed = true;
    }

    return reliable;
}

bool fb_flush(volatile void *base, size_t length) {
    typedef bool (*flush_fn)(volatile void *, size_t);
    static flush_fn fn = NULL;

    if (fn == NULL) {
#if defined (__aarch64__)
        fn = fb_flush_aarch64;
#elif defined (__riscv)
        if (riscv_check_isa_extension("zicbom", NULL, NULL)) {
            fn = fb_flush_riscv;
        }
#elif defined (__loongarch64)
        fn = fb_flush_loongarch64;
#endif
    }

    if (fn != NULL) {
        return fn(base, length);
    }

    // Coherent by construction, or with no way to get there.
    return fb_flush_reliable();
}

void fb_flush_cb(volatile void *base, size_t length) {
    (void)fb_flush(base, length);
}
