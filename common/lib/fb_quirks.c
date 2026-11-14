#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <flanterm_backends/fb.h>
#include <lib/fb_quirks.h>
#include <lib/fb.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/smbios.h>

// The SMBIOS strings that keys test (DSP0134 3.9.0 sections 7.1 to 7.3), by
// the codes the tables use.
#define FM 0 // BIOS Information: Vendor
#define FV 1 // BIOS Information: BIOS Version
#define FD 2 // BIOS Information: BIOS Release Date
#define SM 3 // System Information: Manufacturer
#define SP 4 // System Information: Product Name
#define SV 5 // System Information: Version
#define BM 6 // Baseboard Information: Manufacturer
#define BP 7 // Baseboard Information: Product
#define BS 8 // Baseboard Information: Serial Number
#define FIELD_COUNT 9

// Each field's structure type, and the offset of its string number there.
static const uint8_t field_locations[FIELD_COUNT][2] = {
    [FM] = { 0, 0x04 }, [FV] = { 0, 0x05 }, [FD] = { 0, 0x08 },
    [SM] = { 1, 0x04 }, [SP] = { 1, 0x05 }, [SV] = { 1, 0x06 },
    [BM] = { 2, 0x04 }, [BP] = { 2, 0x05 }, [BS] = { 2, 0x07 }
};

struct key {
    uint8_t field;
    bool exact;
    const char *text;
};

#define MAX_KEYS 4

// The field is the text, or holds it.
#define EQ(F, T) { (F), true, (T) }
#define HAS(F, T) { (F), false, (T) }

struct mac_framebuffer {
    struct key keys[MAX_KEYS];
    uint32_t base;
    uint16_t pitch;
    uint16_t width;
    uint16_t height;
    bool force_pitch;
};

// FM~"VENDOR" and SP~"MODEL", then the geometry.
#define MAC(VENDOR, MODEL, BASE, PITCH, WIDTH, HEIGHT, FORCE_PITCH) \
    { { HAS(FM, VENDOR), HAS(SP, MODEL) }, (BASE), (PITCH), (WIDTH), (HEIGHT), (FORCE_PITCH) }

// Early Intel Macs' framebuffer geometry. From Linux v7.3.4,
// drivers/firmware/efi/sysfb_efi.c.
static const struct mac_framebuffer mac_framebuffers[] = {
    MAC("Apple Computer, Inc.", "iMac4,1", 0x80010000, 5888, 1440, 900, false),
    MAC("Apple Computer, Inc.", "iMac5,1", 0x80010000, 6912, 1680, 1050, false),
    MAC("Apple Inc.", "iMac5,1", 0x80010000, 6912, 1680, 1050, false),
    MAC("Apple Computer, Inc.", "iMac6,1", 0x80010000, 8192, 1920, 1200, false),
    MAC("Apple Inc.", "iMac6,1", 0x80010000, 8192, 1920, 1200, false),
    MAC("Apple Inc.", "iMac7,1", 0x40010000, 6912, 1680, 1050, false),
    MAC("Apple Inc.", "iMac8,1", 0xc0060000, 8192, 1920, 1200, false),
    MAC("Apple Inc.", "iMac10,1", 0xc0010000, 8192, 1920, 1080, false),
    MAC("Apple Inc.", "iMac11,1", 0xc0010000, 10240, 2560, 1440, false),
    MAC("Apple Computer, Inc.", "Macmini1,1", 0x80000000, 8192, 1024, 768, false),
    MAC("Apple Inc.", "Macmini3,1", 0x40010000, 4096, 1024, 768, false),
    MAC("Apple Inc.", "Macmini4,1", 0xc0010000, 8192, 1920, 1200, false),
    MAC("Apple Computer, Inc.", "MacBook1,1", 0x80000000, 8192, 1280, 800, false),
    MAC("Apple Computer, Inc.", "MacBook2,1", 0x80000000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBook2,1", 0x80000000, 8192, 1280, 800, false),
    MAC("Apple Computer, Inc.", "MacBook3,1", 0x80000000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBook3,1", 0x80000000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBook4,1", 0x80000000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBook5,1", 0x80010000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBook6,1", 0x80010000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBook7,1", 0x80010000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBookAir1,1", 0x80000000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBookAir3,1", 0, 8192, 0, 0, true),
    MAC("Apple Computer, Inc.", "MacBookPro1,1", 0x80010000, 5888, 1440, 900, false),
    MAC("Apple Computer, Inc.", "MacBookPro2,1", 0, 0, 0, 0, false),
    MAC("Apple Computer, Inc.", "MacBookPro2,2", 0x80010000, 5888, 1440, 900, false),
    MAC("Apple Inc.", "MacBookPro2,1", 0, 0, 0, 0, false),
    MAC("Apple Computer, Inc.", "MacBookPro3,1", 0x80030000, 8192, 1440, 900, false),
    MAC("Apple Inc.", "MacBookPro3,1", 0x80030000, 8192, 1440, 900, false),
    MAC("Apple Inc.", "MacBookPro4,1", 0xc0060000, 8192, 1920, 1200, false),
    MAC("Apple Inc.", "MacBookPro5,1", 0xc0010000, 8192, 1440, 900, false),
    MAC("Apple Inc.", "MacBookPro5,2", 0xc0010000, 8192, 1920, 1200, false),
    MAC("Apple Inc.", "MacBookPro5,3", 0xd0010000, 8192, 1440, 900, false),
    MAC("Apple Inc.", "MacBookPro6,1", 0x90030000, 8192, 1920, 1200, false),
    MAC("Apple Inc.", "MacBookPro6,2", 0x90030000, 8192, 1680, 1050, false),
    MAC("Apple Inc.", "MacBookPro7,1", 0xc0010000, 8192, 1280, 800, false),
    MAC("Apple Inc.", "MacBookPro8,2", 0x90010000, 5888, 1440, 900, false)
};

struct portrait_panel {
    struct key keys[MAX_KEYS];
    // Only a mode reported as 1280x800, which the firmware turns in software
    // while the memory keeps the panel's own layout.
    bool fixed;
};

#if defined(UEFI)
// Panels laid out portrait whose modes GOP reports with width and height
// exchanged. From Linux v7.3.4, drivers/firmware/efi/sysfb_efi.c.
static const struct portrait_panel portrait_panels[] = {
    // Lenovo IdeaPad Miix 310-10ICR
    { { EQ(SM, "LENOVO"), EQ(SV, "MIIX 310-10ICR"), EQ(FV, "1HCN44WW") }, .fixed = false },
    // Lenovo IdeaPad Miix 320-10ICR
    { { EQ(SM, "LENOVO"), EQ(SV, "Lenovo MIIX 320-10ICR") }, .fixed = false },
    // Lenovo IdeaPad D330-10IGM
    { { EQ(SM, "LENOVO"), EQ(SV, "Lenovo ideapad D330-10IGM") }, .fixed = false },
    // Lenovo IdeaPad Duet 3 10IGL5, LTE variant included
    { { EQ(SM, "LENOVO"), HAS(SV, "IdeaPad Duet 3 10IGL5") }, .fixed = false },
    // Lenovo Yoga Book X91F / X91L
    { { EQ(SM, "LENOVO"), HAS(SP, "Lenovo YB1-X91") }, .fixed = false },
    // Valve Steam Deck (Jupiter)
    { { EQ(SM, "Valve"), EQ(SP, "Jupiter"), EQ(SV, "1") }, .fixed = true },
    // Valve Steam Deck (Galileo)
    { { EQ(SM, "Valve"), EQ(SP, "Galileo"), EQ(SV, "1") }, .fixed = true }
};
#endif

struct turned_panel {
    struct key keys[MAX_KEYS];
    uint16_t width;
    uint16_t height;
    int turn;
    // BIOS Release Dates one of which FD must be, or NULL for any.
    const char *const *dates;
};

// The panel's edge at the top of the casing, as the clockwise turn that what
// is drawn needs to look upright.
#define RIGHT_UP FLANTERM_FB_ROTATE_90
#define LEFT_UP FLANTERM_FB_ROTATE_270

#define DATES(...) ((const char *const []){ __VA_ARGS__, NULL })

// Panels mounted turned in their casing, by their size as the framebuffer
// lays them out. From Linux v7.3.4,
// drivers/gpu/drm/drm_panel_orientation_quirks.c.
static const struct turned_panel turned_panels[] = {
    // Acer One 10 (S1003)
    { { EQ(SM, "Acer"), EQ(SP, "One S1003") }, 800, 1280, RIGHT_UP, NULL },
    // Acer Switch V 10 (SW5-017)
    { { EQ(SM, "Acer"), EQ(SP, "SW5-017") }, 800, 1280, RIGHT_UP, NULL },
    // Anbernic Win600
    { { EQ(BM, "Anbernic"), EQ(SP, "Win600") }, 720, 1280, RIGHT_UP, NULL },
    // ASUS T100HA
    { { EQ(SM, "ASUSTeK COMPUTER INC."), EQ(SP, "T100HAN") }, 800, 1280, LEFT_UP, NULL },
    // ASUS T101HA
    { { EQ(SM, "ASUSTeK COMPUTER INC."), EQ(SP, "T101HA") }, 800, 1280, RIGHT_UP, NULL },
    // ASUS T103HAF
    { { EQ(SM, "ASUSTeK COMPUTER INC."), EQ(SP, "T103HAF") }, 800, 1280, RIGHT_UP, NULL },
    // AYANEO 2 and 2S
    { { EQ(SM, "AYANEO"), HAS(SP, "AYANEO 2") }, 1200, 1920, RIGHT_UP, NULL },
    // AYA NEO 2021
    { { EQ(SM, "AYADEVICE"), EQ(SP, "AYA NEO 2021") }, 800, 1280, RIGHT_UP, NULL },
    // AYANEO AIR
    { { EQ(SM, "AYANEO"), HAS(SP, "AIR") }, 1080, 1920, LEFT_UP, NULL },
    // AYANEO Flip DS, lower screen
    { { EQ(SM, "AYANEO"), EQ(SP, "FLIP DS") }, 640, 960, LEFT_UP, NULL },
    // AYANEO Flip KB and Flip DS, upper screen
    { { EQ(SM, "AYANEO"), HAS(SP, "FLIP") }, 1080, 1920, LEFT_UP, NULL },
    // AYA NEO Founder
    { { EQ(SM, "AYA NEO"), HAS(SP, "AYA NEO Founder") }, 800, 1280, RIGHT_UP, NULL },
    // AYANEO GEEK
    { { EQ(SM, "AYANEO"), HAS(SP, "GEEK") }, 800, 1280, RIGHT_UP, NULL },
    // AYANEO NEXT
    { { EQ(BM, "AYANEO"), HAS(BP, "NEXT") }, 800, 1280, RIGHT_UP, NULL },
    // AYANEO KUN
    { { EQ(BM, "AYANEO"), HAS(BP, "KUN") }, 1600, 2560, RIGHT_UP, NULL },
    // AYANEO SLIDE
    { { EQ(SM, "AYANEO"), HAS(SP, "SLIDE") }, 1080, 1920, LEFT_UP, NULL },
    // AYN Loki Max
    { { EQ(SM, "ayn"), EQ(SP, "Loki Max") }, 1080, 1920, LEFT_UP, NULL },
    // AYN Loki Zero
    { { EQ(SM, "ayn"), EQ(SP, "Loki Zero") }, 1080, 1920, LEFT_UP, NULL },
    // Chuwi HiBook (CWI514)
    {
        { HAS(BM, "Hampoo"), HAS(BP, "Cherry Trail CR"), HAS(FD, "05/07/2016") },
        1200, 1920, RIGHT_UP, NULL
    },
    // Chuwi Hi10 Pro (CWI529)
    { { EQ(BM, "Hampoo"), EQ(SP, "Hi10 pro tablet") }, 1200, 1920, RIGHT_UP, NULL },
    // Dynabook K50
    { { EQ(SM, "Dynabook Inc."), EQ(SP, "dynabook K50/FR") }, 800, 1280, LEFT_UP, NULL },
    // GPD MicroPC, early firmware
    {
        { EQ(SM, "Default string"), EQ(SP, "Default string"), EQ(BM, "Default string"),
          EQ(BP, "Default string") },
        720, 1280, RIGHT_UP, DATES("04/26/2019")
    },
    // GPD MicroPC, later firmware
    { { EQ(SM, "GPD"), EQ(SP, "MicroPC") }, 720, 1280, RIGHT_UP, NULL },
    // GPD Win Max
    { { EQ(SM, "GPD"), EQ(SP, "G1619-01") }, 800, 1280, RIGHT_UP, NULL },
    // GPD Pocket
    {
        { EQ(BM, "AMI Corporation"), EQ(BP, "Default string"), EQ(BS, "Default string"),
          EQ(SP, "Default string") },
        1200, 1920, RIGHT_UP, DATES("05/26/2017", "06/28/2017", "07/05/2017", "08/07/2017")
    },
    // GPD Pocket 2
    {
        { EQ(SM, "Default string"), EQ(SP, "Default string"), EQ(BM, "Default string"),
          EQ(BP, "Default string") },
        1200, 1920, RIGHT_UP, DATES("06/28/2018", "08/28/2018", "12/07/2018")
    },
    // GPD Win
    {
        { EQ(BM, "AMI Corporation"), EQ(BP, "Default string"), EQ(BS, "Default string"),
          EQ(SP, "Default string") },
        720, 1280, RIGHT_UP, DATES("10/25/2016", "11/18/2016", "12/23/2016", "12/26/2016",
                                   "02/21/2017", "03/20/2017", "05/25/2017")
    },
    // GPD Win 2, early firmware
    {
        { EQ(SM, "Default string"), EQ(SP, "Default string"), EQ(BM, "Default string"),
          EQ(BP, "Default string") },
        720, 1280, RIGHT_UP, DATES("12/07/2017", "05/24/2018", "06/29/2018")
    },
    // GPD Win 2, later firmware
    { { EQ(SM, "GPD"), EQ(SP, "WIN2") }, 720, 1280, RIGHT_UP, NULL },
    // GPD Win 3
    { { EQ(SM, "GPD"), EQ(SP, "G1618-03") }, 720, 1280, RIGHT_UP, NULL },
    // GPD Win Mini
    { { EQ(SM, "GPD"), EQ(SP, "G1617-01") }, 1080, 1920, RIGHT_UP, NULL },
    // I.T.Works TW891
    {
        { EQ(SM, "To be filled by O.E.M."), EQ(SP, "TW891"), EQ(BM, "To be filled by O.E.M."),
          EQ(BP, "TW891") },
        800, 1280, RIGHT_UP, DATES("10/16/2015")
    },
    // KD Kurio Smart C15200 2-in-1
    {
        { EQ(SM, "KD Interactive"), EQ(SP, "Kurio Smart"), EQ(BP, "KDM960BCP") },
        800, 1280, RIGHT_UP, NULL
    },
    // Lenovo IdeaPad Miix 310-10ICR
    { { EQ(SM, "LENOVO"), EQ(SP, "80SG"), EQ(SV, "MIIX 310-10ICR") }, 800, 1280, RIGHT_UP, NULL },
    // Lenovo IdeaPad Miix 320-10ICR
    {
        { EQ(SM, "LENOVO"), EQ(SP, "80XF"), EQ(SV, "Lenovo MIIX 320-10ICR") },
        800, 1280, RIGHT_UP, NULL
    },
    // Lenovo IdeaPad D330-10IGM, HD panel
    { { EQ(SM, "LENOVO"), EQ(SV, "Lenovo ideapad D330-10IGM") }, 800, 1280, RIGHT_UP, NULL },
    // Lenovo IdeaPad D330-10IGM, FHD panel
    { { EQ(SM, "LENOVO"), EQ(SV, "Lenovo ideapad D330-10IGM") }, 1200, 1920, RIGHT_UP, NULL },
    // Lenovo IdeaPad D330-10IGL, HD panel
    { { EQ(SM, "LENOVO"), EQ(SV, "Lenovo ideapad D330-10IGL") }, 800, 1280, RIGHT_UP, NULL },
    // Lenovo IdeaPad Duet 3 10IGL5
    { { EQ(SM, "LENOVO"), EQ(SV, "IdeaPad Duet 3 10IGL5") }, 1200, 1920, RIGHT_UP, NULL },
    // Lenovo Legion Go 8APU1
    { { EQ(SM, "LENOVO"), EQ(SV, "Legion Go 8APU1") }, 1600, 2560, LEFT_UP, NULL },
    // Lenovo Yoga Book X90F / X90L
    {
        { EQ(SM, "Intel Corporation"), EQ(SP, "CHERRYVIEW D1 PLATFORM"), EQ(SV, "YETI-11") },
        1200, 1920, RIGHT_UP, NULL
    },
    // Lenovo Yoga Book X91F / X91L
    { { HAS(SP, "Lenovo YB1-X91") }, 1200, 1920, RIGHT_UP, NULL },
    // Lenovo Yoga Tablet 2 830F / 830L
    {
        { HAS(SM, "Intel Corp."), HAS(SP, "VALLEYVIEW C0 PLATFORM"), HAS(BP, "BYT-T FFD8"),
          HAS(FV, "BLADE_21") },
        1200, 1920, RIGHT_UP, NULL
    },
    // Lenovo Yoga Tab 3 X90F
    { { HAS(SM, "Intel Corporation"), HAS(SV, "Blade3-10A-001") }, 1600, 2560, RIGHT_UP, NULL },
    // Nanote UMPC-01
    { { HAS(SM, "RWC CO.,LTD"), HAS(SP, "UMPC-01") }, 1200, 1920, RIGHT_UP, NULL },
    // OneGX1 Pro
    {
        { EQ(SM, "SYSTEM_MANUFACTURER"), EQ(SP, "SYSTEM_PRODUCT_NAME"), EQ(SV, "Default string") },
        1200, 1920, RIGHT_UP, DATES("12/17/2020")
    },
    // OneXPlayer
    {
        { EQ(SM, "ONE-NETBOOK TECHNOLOGY CO., LTD."), EQ(SP, "ONE XPLAYER") },
        1600, 2560, LEFT_UP, NULL
    },
    // OneXPlayer Mini (Intel)
    {
        { EQ(SM, "ONE-NETBOOK TECHNOLOGY CO., LTD."), EQ(SP, "ONE XPLAYER") },
        1200, 1920, LEFT_UP, NULL
    },
    // Orange Pi Neo
    { { EQ(SM, "OrangePi"), EQ(SP, "NEO-01") }, 1200, 1920, RIGHT_UP, NULL },
    // Samsung Galaxy Book 10.6
    {
        { EQ(SM, "SAMSUNG ELECTRONICS CO., LTD."), EQ(SP, "Galaxy Book 10.6") },
        1280, 1920, RIGHT_UP, NULL
    },
    // Valve Steam Deck (Jupiter)
    { { EQ(SM, "Valve"), EQ(SP, "Jupiter"), EQ(SV, "1") }, 800, 1280, RIGHT_UP, NULL },
    // Valve Steam Deck (Galileo)
    { { EQ(SM, "Valve"), EQ(SP, "Galileo"), EQ(SV, "1") }, 800, 1280, RIGHT_UP, NULL },
    // VIOS LTH17
    { { EQ(SM, "VIOS"), EQ(SP, "LTH17") }, 800, 1280, RIGHT_UP, NULL },
    // ZOTAC Gaming Zone
    { { EQ(SM, "ZOTAC"), EQ(BP, "G0A1W") }, 1080, 1920, LEFT_UP, NULL },
    // One-Netbook One Mix 2S
    {
        { EQ(SM, "Default string"), EQ(SP, "Default string"), EQ(BM, "Default string"),
          EQ(BP, "Default string") },
        1200, 1920, RIGHT_UP, DATES("05/21/2018", "10/26/2018", "03/04/2019")
    }
};

// String number n of a structure (DSP0134 3.9.0 section 6.1.3), or an empty
// one where there is no such string or it holds nothing but spaces.
static const char *structure_string(struct smbios_struct_header *hdr, size_t size, uint8_t n) {
    if (n == 0) {
        return "";
    }

    const char *s = (const char *)hdr + hdr->length;
    const char *end = (const char *)hdr + size;

    for (size_t i = 1; s < end && *s != '\0'; i++) {
        size_t len = strlen(s);
        if (i == n) {
            for (size_t j = 0; j < len; j++) {
                if (s[j] != ' ') {
                    return s;
                }
            }
            return "";
        }
        s += len + 1;
    }

    return "";
}

// Each field from the first structure of its type long enough to hold it, or
// NULL without one.
static void read_fields(const char *fields[FIELD_COUNT]) {
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        fields[i] = NULL;
    }

    struct smbios_walk walk;
    if (!smbios_walk_start(&walk)) {
        return;
    }

    struct smbios_struct_header *hdr;
    size_t size;
    while ((hdr = smbios_walk_next(&walk, &size)) != NULL) {
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            uint8_t type = field_locations[i][0];
            uint8_t offset = field_locations[i][1];
            if (fields[i] == NULL && hdr->type == type && hdr->length > offset) {
                fields[i] = structure_string(hdr, size, ((uint8_t *)hdr)[offset]);
            }
        }
    }
}

static bool contains(const char *s, const char *text) {
    size_t len = strlen(text);
    for (; *s != '\0'; s++) {
        if (strncmp(s, text, len) == 0) {
            return true;
        }
    }
    return false;
}

static bool key_holds(const struct key *key, const char *field) {
    if (field == NULL) {
        return false;
    }
    if (key->exact) {
        return strcmp(field, key->text) == 0;
    }
    return contains(field, key->text);
}

static bool keys_match(const struct key *keys, const char *const fields[FIELD_COUNT]) {
    for (size_t i = 0; i < MAX_KEYS && keys[i].text != NULL; i++) {
        if (!key_holds(&keys[i], fields[keys[i].field])) {
            return false;
        }
    }
    return true;
}

static const struct mac_framebuffer *find_mac(const char *const fields[FIELD_COUNT]) {
    for (size_t i = 0; i < SIZEOF_ARRAY(mac_framebuffers); i++) {
        const struct mac_framebuffer *mac = &mac_framebuffers[i];
        // An entry with no geometry leaves the next to decide.
        if (mac->base == 0 && mac->pitch == 0 && mac->width == 0 && mac->height == 0) {
            continue;
        }
        if (keys_match(mac->keys, fields)) {
            return mac;
        }
    }
    return NULL;
}

static void correct_mac(struct fb_info *mode, const struct mac_framebuffer *mac) {
    if (mode->framebuffer_width == 0) {
        mode->framebuffer_width = mac->width;
    }
    if (mode->framebuffer_height == 0) {
        mode->framebuffer_height = mac->height;
    }

    // A mode whose rows are wider than the forced pitch cannot have it, and
    // keeps the firmware's.
    uint64_t row = mode->framebuffer_width * ((mode->framebuffer_bpp + 7) / 8);
    if (mode->framebuffer_pitch == 0 || (mac->force_pitch && mac->pitch >= row)) {
        mode->framebuffer_pitch = mac->pitch;
    }
}

#if defined(UEFI)
static const struct portrait_panel *find_portrait(const char *const fields[FIELD_COUNT]) {
    for (size_t i = 0; i < SIZEOF_ARRAY(portrait_panels); i++) {
        if (keys_match(portrait_panels[i].keys, fields)) {
            return &portrait_panels[i];
        }
    }
    return NULL;
}
#endif

static void correct_portrait(struct fb_info *mode, const struct portrait_panel *portrait) {
    if (portrait->fixed) {
        // The panel's own layout: 800x1280, in rows of 832 pixels.
        if (mode->framebuffer_width == 1280 && mode->framebuffer_height == 800) {
            mode->framebuffer_width = 800;
            mode->framebuffer_height = 1280;
            mode->framebuffer_pitch = 3328;
        }
        return;
    }

    // Some firmware releases for these machines already report the mode as
    // it is laid out, portrait.
    if (mode->framebuffer_width > mode->framebuffer_height) {
        uint64_t width = mode->framebuffer_height;
        mode->framebuffer_height = mode->framebuffer_width;
        mode->framebuffer_width = width;
        // The pitch reported goes with the exchanged layout, so whatever
        // padding the real rows have is unknown.
        mode->framebuffer_pitch = width * mode->framebuffer_bpp / 8;
    }
}

static bool date_listed(const char *const *dates, const char *date) {
    if (date == NULL) {
        return false;
    }
    for (; *dates != NULL; dates++) {
        if (strcmp(*dates, date) == 0) {
            return true;
        }
    }
    return false;
}

static int upright_rotation(const struct fb_info *fb, const char *const fields[FIELD_COUNT]) {
    for (size_t i = 0; i < SIZEOF_ARRAY(turned_panels); i++) {
        const struct turned_panel *panel = &turned_panels[i];
        if (panel->width != fb->framebuffer_width || panel->height != fb->framebuffer_height
         || !keys_match(panel->keys, fields)) {
            continue;
        }
        if (panel->dates != NULL && !date_listed(panel->dates, fields[FD])) {
            continue;
        }
        return panel->turn;
    }
    return FLANTERM_FB_ROTATE_0;
}

void fb_quirks_apply(struct fb_info *fbs, size_t fbs_count) {
    if (fbs_count == 0) {
        return;
    }

    const char *fields[FIELD_COUNT];
    read_fields(fields);

    const struct mac_framebuffer *mac = find_mac(fields);
#if defined(UEFI)
    // Only GOP is known to report these modes exchanged.
    const struct portrait_panel *portrait = find_portrait(fields);
#else
    const struct portrait_panel *portrait = NULL;
#endif

    // The first framebuffer alone, the one the Linux protocol hands Linux, so
    // that Limine goes by these tables exactly where Linux does.
    struct fb_info *fb = &fbs[0];

    // Base 0 is no framebuffer for the geometry to fill in.
    if (mac != NULL && fb->framebuffer_addr != 0) {
        // Each mode as the framebuffer would be in it.
        correct_mac(fb, mac);
        for (size_t i = 0; i < fb->mode_count; i++) {
            correct_mac(&fb->mode_list[i], mac);
        }
    }

    if (portrait != NULL) {
        // Each mode as the framebuffer would be in it.
        correct_portrait(fb, portrait);
        for (size_t i = 0; i < fb->mode_count; i++) {
            correct_portrait(&fb->mode_list[i], portrait);
        }
    }

    fb->upright_rotation = upright_rotation(fb, fields);
}
