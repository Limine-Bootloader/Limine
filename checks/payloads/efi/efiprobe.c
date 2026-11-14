// An EFI application the checks chainload or boot directly. It runs the space
// separated commands in its load options, or in \efiprobe.cmd on its own
// volume when it has none, and reports in the "K key=value" format.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <efi.h>

// mem.c's, which the compiler calls too.
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

static struct efi_system_table *st;
static struct efi_boot_services *bs;
static struct efi_runtime_services *rt;
static efi_handle self;
static struct efi_loaded_image_protocol *loaded;
#if !defined(__x86_64__) && !defined(__i386__)
static struct efi_serial_io_protocol *serial;
#endif

static size_t slen(const char *s) {
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

static bool sprefix(const char *s, const char *p) {
    while (*p != 0) {
        if (*s++ != *p++) {
            return false;
        }
    }
    return true;
}

static void putch(char c) {
#if defined(__x86_64__) || defined(__i386__)
    asm volatile ("outb %0, $0xe9" :: "a"(c));
#else
    // ConOut's terminal can drop what runs past its last column, so the
    // report goes to the serial port itself.
    if (serial != NULL) {
        char crlf[2] = { '\r', c };
        size_t size = c == '\n' ? 2 : 1;
        serial->write(serial, &size, c == '\n' ? crlf : crlf + 1);
        return;
    }
#endif
    uint16_t buf[3] = { (uint16_t)(uint8_t)c, 0, 0 };
    if (c == '\n') {
        buf[0] = '\r';
        buf[1] = '\n';
    }
    st->con_out->output_string(st->con_out, buf);
}

static void puts_(const char *s) {
    while (*s != 0) {
        putch(*s++);
    }
}

static void putnum(uint64_t v, unsigned base, int width) {
    char buf[24];
    int i = 0;
    do {
        buf[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v != 0);
    while (i < width) {
        buf[i++] = '0';
    }
    while (i > 0) {
        putch(buf[--i]);
    }
}

// %s, %u, %x (64-bit arguments), %w (UTF-16 string, escaped), %e (escaped
// char *).
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt != 0; fmt++) {
        if (*fmt != '%') {
            putch(*fmt);
            continue;
        }
        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        switch (*fmt) {
            case 's':
                puts_(va_arg(ap, const char *));
                break;
            case 'u':
                putnum(va_arg(ap, uint64_t), 10, width);
                break;
            case 'x':
                putnum(va_arg(ap, uint64_t), 16, width);
                break;
            case 'w': {
                const uint16_t *w = va_arg(ap, const uint16_t *);
                for (; w != NULL && *w != 0; w++) {
                    if (*w >= 0x20 && *w < 0x7f && *w != '\\') {
                        putch((char)*w);
                    } else {
                        puts_("\\u");
                        putnum(*w, 16, 4);
                    }
                }
                break;
            }
            case 'e': {
                const uint8_t *p = va_arg(ap, const uint8_t *);
                for (; *p != 0; p++) {
                    if (*p >= 0x20 && *p < 0x7f && *p != '\\') {
                        putch((char)*p);
                    } else {
                        puts_("\\x");
                        putnum(*p, 16, 2);
                    }
                }
                break;
            }
            default:
                putch('?');
                break;
        }
    }
    va_end(ap);
}

static void hexdump(const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        putnum(p[i], 16, 2);
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static uint64_t parse_hex(const char *s, const char **end) {
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (hexval(*s) >= 0) {
        v = v * 16 + hexval(*s++);
    }
    if (end != NULL) {
        *end = s;
    }
    return v;
}

static bool parse_guid(const char *s, struct efi_guid *g) {
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    if (slen(s) < 36) {
        return false;
    }
    const char *e;
    g->data1 = (uint32_t)parse_hex(s, &e);
    g->data2 = (uint16_t)parse_hex(s + 9, &e);
    g->data3 = (uint16_t)parse_hex(s + 14, &e);
    int idx[8] = { 19, 21, 24, 26, 28, 30, 32, 34 };
    for (int i = 0; i < 8; i++) {
        g->data4[i] = (uint8_t)(hexval(s[idx[i]]) * 16 + hexval(s[idx[i] + 1]));
    }
    return true;
}

static void put_guid(const struct efi_guid *g) {
    putnum(g->data1, 16, 8);
    putch('-');
    putnum(g->data2, 16, 4);
    putch('-');
    putnum(g->data3, 16, 4);
    putch('-');
    putnum(g->data4[0], 16, 2);
    putnum(g->data4[1], 16, 2);
    putch('-');
    for (int i = 2; i < 8; i++) {
        putnum(g->data4[i], 16, 2);
    }
}

static void *alloc(size_t size) {
    void *p = NULL;
    if (bs->allocate_pool(EFI_LOADER_DATA, size, &p) != EFI_SUCCESS) {
        return NULL;
    }
    return p;
}

static void to_utf16(uint16_t *dst, const char *src, size_t max) {
    size_t i = 0;
    for (; src[i] != 0 && i + 1 < max; i++) {
        dst[i] = (uint8_t)src[i];
    }
    dst[i] = 0;
}

static struct efi_device_path_to_text_protocol *dp_text(void) {
    static struct efi_device_path_to_text_protocol *p;
    if (p == NULL) {
        struct efi_guid g = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
        bs->locate_protocol(&g, NULL, (void **)&p);
    }
    return p;
}

static void put_devpath(struct efi_device_path_protocol *dp) {
    if (dp == NULL) {
        puts_("(null)");
        return;
    }
    struct efi_device_path_to_text_protocol *t = dp_text();
    if (t == NULL) {
        puts_("(no text protocol)");
        return;
    }
    uint16_t *s = t->convert_device_path_to_text(dp, false, false);
    out("%w", s);
    if (s != NULL) {
        bs->free_pool(s);
    }
}

static size_t devpath_size(struct efi_device_path_protocol *dp) {
    size_t total = 0;
    for (;;) {
        size_t len = dp->length[0] | (dp->length[1] << 8);
        total += len;
        if (dp->type == EFI_END_DEVICE_PATH_TYPE && dp->sub_type == EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE) {
            return total;
        }
        dp = (struct efi_device_path_protocol *)((uint8_t *)dp + len);
    }
}

// The device path of a file on this image's own volume.
static struct efi_device_path_protocol *file_devpath(const char *path) {
    struct efi_guid dpg = EFI_DEVICE_PATH_PROTOCOL_GUID;
    struct efi_device_path_protocol *dev = NULL;
    if (bs->handle_protocol(loaded->device_handle, &dpg, (void **)&dev) != EFI_SUCCESS) {
        return NULL;
    }
    size_t dev_size = devpath_size(dev) - 4;
    size_t plen = slen(path);
    size_t node = 4 + (plen + 1) * 2;
    uint8_t *buf = alloc(dev_size + node + 4);
    for (size_t i = 0; i < dev_size; i++) {
        buf[i] = ((uint8_t *)dev)[i];
    }
    struct efi_device_path_protocol *fp = (struct efi_device_path_protocol *)(buf + dev_size);
    fp->type = EFI_MEDIA_DEVICE_PATH;
    fp->sub_type = EFI_MEDIA_FILEPATH_DP;
    fp->length[0] = node & 0xff;
    fp->length[1] = node >> 8;
    uint16_t *name = (uint16_t *)(buf + dev_size + 4);
    to_utf16(name, path, plen + 1);
    struct efi_device_path_protocol *end = (void *)(buf + dev_size + node);
    end->type = EFI_END_DEVICE_PATH_TYPE;
    end->sub_type = EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end->length[0] = 4;
    end->length[1] = 0;
    return (struct efi_device_path_protocol *)buf;
}

static void cmd_info(void) {
    out("K options_size=%u\n", (uint64_t)loaded->load_options_size);
    if (loaded->load_options != NULL) {
        uint16_t *o = loaded->load_options;
        out("K options=%w\n", o);
        // The last code unit the size covers, to see the terminator is counted.
        size_t units = loaded->load_options_size / 2;
        out("K options_last_unit=%x\n", (uint64_t)(units > 0 ? o[units - 1] : 0xffff));
    } else {
        out("K options=(null)\n");
    }
    out("K file_path=");
    put_devpath(loaded->file_path);
    out("\nK device_path=");
    struct efi_guid dpg = EFI_DEVICE_PATH_PROTOCOL_GUID;
    struct efi_device_path_protocol *dev = NULL;
    if (loaded->device_handle != NULL) {
        bs->handle_protocol(loaded->device_handle, &dpg, (void **)&dev);
    }
    put_devpath(dev);
    out("\nK image_code_type=%u\n", (uint64_t)loaded->image_code_type);
    struct efi_guid lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    struct efi_loaded_image_protocol *parent = NULL;
    if (loaded->parent_handle != NULL
     && bs->handle_protocol(loaded->parent_handle, &lig, (void **)&parent) == EFI_SUCCESS) {
        out("K parent_file_path=");
        put_devpath(parent->file_path);
        out("\n");
    }
}

static void cmd_gop(void) {
    struct efi_guid g = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    struct efi_graphics_output_protocol *gop = NULL;
    if (bs->locate_protocol(&g, NULL, (void **)&gop) != EFI_SUCCESS || gop == NULL) {
        out("K gop=none\n");
        return;
    }
    out("K gop.mode=%ux%u\n", (uint64_t)gop->mode->info->horizontal_resolution,
        (uint64_t)gop->mode->info->vertical_resolution);
}

// A graphics output protocol over reserved memory, with modes in the formats
// QEMU's display devices never offer, portrait ones, a 1280x800 one of padded
// rows, and an EDID that prefers 800x600.
struct fake_mode {
    uint32_t width, height, format, pitch;
    struct efi_pixel_bitmask masks;
};

static const struct fake_mode fake_modes[] = {
    { 640, 480, EFI_PIXEL_BIT_MASK, 640, { 0xf800, 0x07e0, 0x001f, 0 } },
    { 800, 600, EFI_PIXEL_RED_GREEN_BLUE_RESERVED_8_BIT_PER_COLOR, 800, { 0, 0, 0, 0 } },
    { 1024, 768, EFI_PIXEL_BIT_MASK, 1024, { 0, 0, 0, 0 } },
    { 1280, 1024, EFI_PIXEL_BLT_ONLY, 1280, { 0, 0, 0, 0 } },
    { 640, 480, EFI_PIXEL_BLUE_GREEN_RED_RESERVED_8_BIT_PER_COLOR, 600, { 0, 0, 0, 0 } },
    { 320, 200, EFI_PIXEL_BIT_MASK, 320, { 0x7c00, 0x03e0, 0x001f, 0x8000 } },
    { 800, 1280, EFI_PIXEL_BLUE_GREEN_RED_RESERVED_8_BIT_PER_COLOR, 800, { 0, 0, 0, 0 } },
    { 720, 1280, EFI_PIXEL_BLUE_GREEN_RED_RESERVED_8_BIT_PER_COLOR, 720, { 0, 0, 0, 0 } },
    { 1280, 800, EFI_PIXEL_BLUE_GREEN_RED_RESERVED_8_BIT_PER_COLOR, 1344, { 0, 0, 0, 0 } }
};
#define FAKE_MODES (sizeof(fake_modes) / sizeof(fake_modes[0]))

struct fake_gop {
    struct efi_graphics_output_protocol gop;
    struct efi_graphics_output_protocol_mode mode;
    struct efi_graphics_output_mode_information info;
    struct efi_edid_active_protocol edid;
    bool started;
    bool no_display;
};

// Bit 0: QueryMode() says EFI_NOT_STARTED until SetMode() is called. Bit 1:
// SetMode() refuses mode 1, the EDID's. Bit 2: the framebuffer lies in boot
// services data, half a page into its pages. Bit 3: a GPU with no display
// attached: no EDID, a current mode of 0x0 with no framebuffer, and a
// SetMode() that never returns.
#define FAKEGOP_NOT_STARTED 1
#define FAKEGOP_MODE1_FAILS 2
#define FAKEGOP_BS_DATA 4
#define FAKEGOP_NO_DISPLAY 8
static uint64_t fakegop_flags;
static efi_physical_address fake_fb;
static uint8_t fake_edid[128];

static void fake_info(struct efi_graphics_output_mode_information *info, uint32_t n) {
    info->version = 0;
    info->horizontal_resolution = fake_modes[n].width;
    info->vertical_resolution = fake_modes[n].height;
    info->pixel_format = fake_modes[n].format;
    info->pixel_information = fake_modes[n].masks;
    info->pixels_per_scan_line = fake_modes[n].pitch;
}

static efi_status EFI_API fake_query_mode(struct efi_graphics_output_protocol *this, uint32_t n, size_t *size,
                                          struct efi_graphics_output_mode_information **info) {
    if ((fakegop_flags & FAKEGOP_NOT_STARTED) && !((struct fake_gop *)this)->started) {
        return EFI_NOT_STARTED;
    }
    if (n >= FAKE_MODES) {
        return EFI_INVALID_PARAMETER;
    }
    *info = alloc(sizeof(**info));
    fake_info(*info, n);
    if (((struct fake_gop *)this)->no_display && n == this->mode->mode) {
        (*info)->horizontal_resolution = 0;
        (*info)->vertical_resolution = 0;
    }
    *size = sizeof(**info);
    return EFI_SUCCESS;
}

static efi_status EFI_API fake_set_mode(struct efi_graphics_output_protocol *this, uint32_t n) {
    struct fake_gop *f = (struct fake_gop *)this;
    if (f->no_display) {
        for (;;) {
        }
    }
    if (n >= FAKE_MODES) {
        return EFI_UNSUPPORTED;
    }
    if ((fakegop_flags & FAKEGOP_MODE1_FAILS) && n == 1) {
        return EFI_DEVICE_ERROR;
    }
    f->started = true;
    f->mode.mode = n;
    fake_info(&f->info, n);
    f->mode.frame_buffer_size = (size_t)fake_modes[n].pitch * fake_modes[n].height * 4;
    return EFI_SUCCESS;
}

static efi_status EFI_API fake_blt(struct efi_graphics_output_protocol *this, struct efi_graphics_output_blt_pixel *buf,
                                   uint32_t op, size_t sx, size_t sy,
                                   size_t dx, size_t dy, size_t w, size_t h, size_t delta) {
    (void)this; (void)buf; (void)op; (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h; (void)delta;
    return EFI_UNSUPPORTED;
}

// fakegop:N[:F] installs N such protocols, all over the same memory, each on
// a handle of its own, the first carrying the EDID; F holds FAKEGOP_ flags.
static void cmd_fakegop(char *arg) {
    const char *end;
    uint64_t count = parse_hex(arg, &end);
    fakegop_flags = *end == ':' ? parse_hex(end + 1, NULL) : 0;
    if (fake_fb == 0) {
        size_t pages = (1280 * 1024 * 4 + 4095) / 4096;
        if (fakegop_flags & FAKEGOP_BS_DATA) {
            bs->allocate_pages(
                EFI_ALLOCATE_ANY_PAGES, EFI_BOOT_SERVICES_DATA, pages + 1, &fake_fb
            );
            fake_fb += 2048;
        } else {
            bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_RESERVED_MEMORY_TYPE, pages, &fake_fb);
        }
        static const uint8_t timing[18] = { 0x64, 0x19, 0x20, 0x00, 0x30, 0x58, 0x00, 0x20 };
        static const uint8_t header[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
        for (size_t i = 0; i < 8; i++) {
            fake_edid[i] = header[i];
        }
        fake_edid[18] = 1;
        fake_edid[19] = 3;
        for (size_t i = 0; i < 18; i++) {
            fake_edid[54 + i] = timing[i];
        }
        uint8_t sum = 0;
        for (size_t i = 0; i < 127; i++) {
            sum += fake_edid[i];
        }
        fake_edid[127] = (uint8_t)-sum;
    }
    struct efi_guid gg = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    struct efi_guid eg = EFI_EDID_ACTIVE_PROTOCOL_GUID;
    for (uint64_t i = 0; i < count; i++) {
        struct fake_gop *f = alloc(sizeof(*f));
        f->no_display = false;
        f->gop.query_mode = fake_query_mode;
        f->gop.set_mode = fake_set_mode;
        f->gop.blt = fake_blt;
        f->gop.mode = &f->mode;
        f->mode.max_mode = FAKE_MODES;
        f->mode.info = &f->info;
        f->mode.size_of_info = sizeof(f->info);
        f->mode.frame_buffer_base = fake_fb;
        fake_set_mode(&f->gop, 0);
        f->started = !(fakegop_flags & FAKEGOP_NOT_STARTED);
        if (fakegop_flags & FAKEGOP_NO_DISPLAY) {
            f->info.horizontal_resolution = 0;
            f->info.vertical_resolution = 0;
            f->mode.frame_buffer_base = 0;
            f->mode.frame_buffer_size = 0;
            f->no_display = true;
        }
        f->edid.size_of_edid = sizeof(fake_edid);
        f->edid.edid = fake_edid;
        efi_handle h = NULL;
        efi_status s = bs->install_protocol_interface(&h, &gg, EFI_NATIVE_INTERFACE, &f->gop);
        if (s == EFI_SUCCESS && i == 0 && !(fakegop_flags & FAKEGOP_NO_DISPLAY)) {
            s = bs->install_protocol_interface(&h, &eg, EFI_NATIVE_INTERFACE, &f->edid);
        }
        out("K fakegop.%u=%x\n", i, (uint64_t)s);
    }
    out("K fakegop.fb=%x\n", (uint64_t)fake_fb);
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void cmd_time(void) {
    struct efi_time t;
    if (rt->get_time(&t, NULL) != EFI_SUCCESS) {
        out("K time=error\n");
        return;
    }
    int64_t days = days_from_civil(t.year, t.month, t.day);
    uint64_t since_epoch = (uint64_t)(days * 86400 + t.hour * 3600 + t.minute * 60 + t.second);
    out("K time=%u-%2u-%2u %2u:%2u:%2u\n", (uint64_t)t.year, (uint64_t)t.month, (uint64_t)t.day,
        (uint64_t)t.hour, (uint64_t)t.minute, (uint64_t)t.second);
    out("K time.unix=%u\n", since_epoch);
    out("K time.timezone=%x\n", (uint64_t)(uint16_t)t.time_zone);
}

// The firmware's memory map, and where this image's stack is.
static void cmd_memmap(void) {
    uint64_t sp;
    sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    out("K memmap.frame=%x\n", sp);
    size_t size = 0, key, desc_size;
    uint32_t desc_ver;
    bs->get_memory_map(&size, NULL, &key, &desc_size, &desc_ver);
    size += 8 * desc_size;
    uint8_t *buf = NULL;
    if (bs->allocate_pool(EFI_LOADER_DATA, size, (void **)&buf) != EFI_SUCCESS) {
        out("K memmap.error=alloc\n");
        return;
    }
    if (bs->get_memory_map(&size, (struct efi_memory_descriptor *)buf, &key, &desc_size, &desc_ver) != EFI_SUCCESS) {
        out("K memmap.error=get\n");
        bs->free_pool(buf);
        return;
    }
    for (size_t off = 0; off + desc_size <= size; off += desc_size) {
        struct efi_memory_descriptor *d = (struct efi_memory_descriptor *)(buf + off);
        out("K memmap=%x %x %x %x\n", (uint64_t)d->type, (uint64_t)d->physical_start,
            (uint64_t)d->number_of_pages, (uint64_t)d->attribute);
    }
    bs->free_pool(buf);
}

// Block devices, in the order LocateHandle gives them.
static void cmd_blockio(void) {
    struct efi_guid bio = EFI_BLOCK_IO_PROTOCOL_GUID;
    struct efi_guid dpg = EFI_DEVICE_PATH_PROTOCOL_GUID;
    size_t size = 0;
    bs->locate_handle(EFI_BY_PROTOCOL, &bio, NULL, &size, NULL);
    efi_handle *handles = NULL;
    if (size == 0 || bs->allocate_pool(EFI_LOADER_DATA, size, (void **)&handles) != EFI_SUCCESS
     || bs->locate_handle(EFI_BY_PROTOCOL, &bio, NULL, &size, handles) != EFI_SUCCESS) {
        out("K blockio.error=1\n");
        return;
    }
    for (size_t i = 0; i < size / sizeof(efi_handle); i++) {
        struct efi_block_io_protocol *b = NULL;
        struct efi_device_path_protocol *dp = NULL;
        bs->handle_protocol(handles[i], &bio, (void **)&b);
        bs->handle_protocol(handles[i], &dpg, (void **)&dp);
        out("K blockio=%u %u %u %x ", (uint64_t)b->media->logical_partition, (uint64_t)b->media->read_only,
            (uint64_t)b->media->block_size, (uint64_t)b->media->last_block);
        put_devpath(dp);
        out("\n");
    }
    bs->free_pool(handles);
}

static void cmd_tables(void) {
    for (size_t i = 0; i < st->number_of_table_entries; i++) {
        out("K table.%u=", (uint64_t)i);
        put_guid(&st->configuration_table[i].vendor_guid);
        out("\n");
    }
}

// name@guid
static bool split_var(char *arg, uint16_t *name, size_t max, struct efi_guid *guid, char **rest) {
    char *at = arg;
    while (*at != 0 && *at != '@') {
        at++;
    }
    if (*at != '@') {
        return false;
    }
    *at = 0;
    to_utf16(name, arg, max);
    if (!parse_guid(at + 1, guid)) {
        return false;
    }
    *rest = at + 37;
    if (**rest == '@') {
        (*rest)++;
    }
    return true;
}

static void dump_var(const uint16_t *name, struct efi_guid *guid) {
    static uint8_t buf[8192];
    size_t size = sizeof(buf);
    uint32_t attrs = 0;
    efi_status s = rt->get_variable((uint16_t *)name, guid, &attrs, &size, buf);
    out("K var.%w=", name);
    if (s != EFI_SUCCESS) {
        out("missing\n");
        return;
    }
    out("attrs %x size %u hex ", (uint64_t)attrs, (uint64_t)size);
    hexdump(buf, size);
    out("\n");
}

static void cmd_var(char *arg) {
    uint16_t name[128];
    struct efi_guid guid;
    char *rest;
    if (!split_var(arg, name, 128, &guid, &rest)) {
        out("K error=bad var argument\n");
        return;
    }
    dump_var(name, &guid);
}

// Every variable of one vendor.
static void cmd_vendor_vars(char *arg) {
    struct efi_guid want;
    if (!parse_guid(arg, &want)) {
        out("K error=bad guid\n");
        return;
    }
    static uint16_t name[512];
    name[0] = 0;
    struct efi_guid guid;
    for (;;) {
        size_t size = sizeof(name);
        if (rt->get_next_variable_name(&size, name, &guid) != EFI_SUCCESS) {
            break;
        }
        bool match = true;
        for (size_t i = 0; i < sizeof(struct efi_guid); i++) {
            if (((uint8_t *)&guid)[i] != ((uint8_t *)&want)[i]) {
                match = false;
            }
        }
        if (match) {
            dump_var(name, &guid);
        }
    }
}

// name@guid@attrs@hexbytes, or name@guid@attrs@"string" (UTF-16, terminated).
static void cmd_setvar(char *arg) {
    uint16_t name[128];
    struct efi_guid guid;
    char *rest;
    if (!split_var(arg, name, 128, &guid, &rest)) {
        out("K error=bad setvar argument\n");
        return;
    }
    const char *e;
    uint32_t attrs = (uint32_t)parse_hex(rest, &e);
    if (*e == '@') {
        e++;
    }
    static uint8_t data[4096];
    size_t n = 0;
    if (*e == '"') {
        e++;
        while (*e != 0 && *e != '"' && n + 4 < sizeof(data)) {
            // '_' stands for a space, which the command syntax cannot carry.
            char c = *e == '_' ? ' ' : *e;
            data[n++] = (uint8_t)c;
            data[n++] = 0;
            e++;
        }
        data[n++] = 0;
        data[n++] = 0;
    } else {
        while (hexval(e[0]) >= 0 && hexval(e[1]) >= 0 && n < sizeof(data)) {
            data[n++] = (uint8_t)(hexval(e[0]) * 16 + hexval(e[1]));
            e += 2;
        }
    }
    efi_status s = rt->set_variable(name, &guid, attrs, n, n ? data : NULL);
    out("K setvar.%w=%x\n", name, (uint64_t)s);
}

// XXXX@Description@\path: a Boot#### option for a file on this volume,
// appended to BootOrder.
static void cmd_addboot(char *arg) {
    const char *e;
    uint16_t num = (uint16_t)parse_hex(arg, &e);
    if (*e != '@') {
        out("K error=bad addboot\n");
        return;
    }
    char *desc = (char *)e + 1;
    char *path = desc;
    while (*path != 0 && *path != '@') {
        path++;
    }
    if (*path != '@') {
        out("K error=bad addboot\n");
        return;
    }
    *path++ = 0;
    for (char *p = desc; *p != 0; p++) {
        if (*p == '_') {
            *p = ' ';
        }
    }
    struct efi_device_path_protocol *dp = file_devpath(path);
    if (dp == NULL) {
        out("K error=no device path\n");
        return;
    }
    size_t dpsize = devpath_size(dp);
    size_t dlen = slen(desc);
    size_t total = 4 + 2 + (dlen + 1) * 2 + dpsize;
    uint8_t *opt = alloc(total);
    uint32_t attrs = EFI_LOAD_OPTION_ACTIVE;
    for (int i = 0; i < 4; i++) {
        opt[i] = (attrs >> (8 * i)) & 0xff;
    }
    opt[4] = dpsize & 0xff;
    opt[5] = dpsize >> 8;
    to_utf16((uint16_t *)(opt + 6), desc, dlen + 1);
    for (size_t i = 0; i < dpsize; i++) {
        opt[6 + (dlen + 1) * 2 + i] = ((uint8_t *)dp)[i];
    }
    struct efi_guid gv = EFI_GLOBAL_VARIABLE;
    uint16_t name[9] = { 'B', 'o', 'o', 't', 0, 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        name[4 + i] = "0123456789ABCDEF"[(num >> (12 - 4 * i)) & 0xf];
    }
    uint32_t nv = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS
                | EFI_VARIABLE_RUNTIME_ACCESS;
    efi_status s = rt->set_variable(name, &gv, nv, total, opt);
    out("K addboot=%x\n", (uint64_t)s);
    static uint16_t order[256];
    size_t size = sizeof(order) - 2;
    if (rt->get_variable(L"BootOrder", &gv, NULL, &size, order) != EFI_SUCCESS) {
        size = 0;
    }
    order[size / 2] = num;
    s = rt->set_variable(L"BootOrder", &gv, nv, size + 2, order);
    out("K bootorder=%x\n", (uint64_t)s);
}

// \path[@options]: load and start an image from this volume, and report how
// it came back.
static uint64_t fnv1a(const uint8_t *p, uint64_t len) {
    uint64_t h = 0xcbf29ce484222325;
    for (uint64_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 0x100000001b3;
    }
    return h;
}

// A CC measurement protocol over no hardware, as Limine finds on Intel TDX
// and AMD SEV: it maps PCRs to MRs as TDX does (PCR 0 to the MRTD, 1 and 7 to
// RTMR[0], 2 to 6 to RTMR[1], 8 to 15 to RTMR[2]), and reports each call.
// Flags: 1 has MapPcrToMrIndex() fail, 2 HashLogExtendEvent(), 4
// GetEventLog(); 8 has HashLogExtendEvent() report the event log full.
static uint32_t fakecc_flags;

static efi_status EFI_API fakecc_capability(struct efi_cc_measurement_protocol *this,
                                            struct efi_cc_boot_service_capability *cap) {
    (void)this;
    cap->structure_version = (struct efi_cc_version){ 1, 0 };
    cap->protocol_version = (struct efi_cc_version){ 1, 0 };
    cap->hash_algorithm_bitmap = EFI_CC_BOOT_HASH_ALG_SHA384;
    cap->supported_event_logs = EFI_CC_EVENT_LOG_FORMAT_TCG_2;
    cap->cc_type = (struct efi_cc_type){ EFI_CC_TYPE_INTEL_TDX, 0 };
    return EFI_SUCCESS;
}

static efi_status EFI_API fakecc_event_log(struct efi_cc_measurement_protocol *this, efi_cc_event_log_format format,
                                           efi_physical_address *location, efi_physical_address *last,
                                           bool *truncated) {
    (void)this;
    out("K cc.event_log=%x\n", (uint64_t)format);
    if (fakecc_flags & 4) {
        return EFI_DEVICE_ERROR;
    }
    *location = 0;
    *last = 0;
    *truncated = false;
    return EFI_SUCCESS;
}

static efi_status EFI_API fakecc_extend(struct efi_cc_measurement_protocol *this, uint64_t flags,
                                        efi_physical_address data, uint64_t len, struct efi_cc_event *event) {
    (void)this;
    out("K cc.extend=%u %x %x %x %u %e\n", (uint64_t)event->header.mr_index, (uint64_t)event->header.event_type,
        (uint64_t)flags, fnv1a((const uint8_t *)(uintptr_t)data, len), (uint64_t)len, (const char *)event->event);
    return (fakecc_flags & 2) ? EFI_DEVICE_ERROR : (fakecc_flags & 8) ? EFI_VOLUME_FULL : EFI_SUCCESS;
}

static efi_status EFI_API fakecc_map(struct efi_cc_measurement_protocol *this, efi_tcg_pcrindex pcr, efi_cc_mr_index *mr) {
    (void)this;
    out("K cc.map=%u\n", (uint64_t)pcr);
    if ((fakecc_flags & 1) || pcr > 15) {
        return EFI_INVALID_PARAMETER;
    }
    *mr = pcr == 0 ? 0 : pcr == 1 || pcr == 7 ? 1 : pcr <= 6 ? 2 : 3;
    return EFI_SUCCESS;
}

static struct efi_cc_measurement_protocol fakecc = { fakecc_capability, fakecc_event_log, fakecc_extend, fakecc_map };
static bool fakecc_pending;

// Installed by chain: once the image is loaded, for firmware that measures
// the images it loads not to measure that one through it.
static void cmd_fakecc(char *arg) {
    fakecc_flags = (uint32_t)parse_hex(arg, NULL);
    fakecc_pending = true;
}

static void install_fakecc(void) {
    struct efi_guid g = EFI_CC_MEASUREMENT_PROTOCOL_GUID;
    efi_handle h = NULL;
    out("K fakecc=%x\n", (uint64_t)bs->install_protocol_interface(&h, &g, EFI_NATIVE_INTERFACE, &fakecc));
}

// A TCG2 protocol over no TPM that claims one, reporting each call. Flags as
// the CC protocol's, but for 1, which it has no use for.
static uint32_t faketcg2_flags;

static efi_status EFI_API faketcg2_capability(struct efi_tcg2_protocol *this, struct efi_tcg2_boot_service_capability *cap) {
    (void)this;
    cap->structure_version = (struct efi_tcg2_version){ 1, 1 };
    cap->protocol_version = (struct efi_tcg2_version){ 1, 1 };
    cap->hash_algorithm_bitmap = EFI_TCG2_BOOT_HASH_ALG_SHA256;
    cap->supported_event_logs = EFI_TCG2_EVENT_LOG_FORMAT_TCG_2;
    cap->tpm_present_flag = true;
    cap->number_of_pcr_banks = 1;
    cap->active_pcr_banks = EFI_TCG2_BOOT_HASH_ALG_SHA256;
    return EFI_SUCCESS;
}

static efi_status EFI_API faketcg2_event_log(struct efi_tcg2_protocol *this, efi_tcg2_event_log_format format,
                                             efi_physical_address *location, efi_physical_address *last,
                                             bool *truncated) {
    (void)this;
    out("K tcg2.event_log=%x\n", (uint64_t)format);
    if (faketcg2_flags & 4) {
        return EFI_DEVICE_ERROR;
    }
    *location = 0;
    *last = 0;
    *truncated = (faketcg2_flags & 8) != 0;
    return EFI_SUCCESS;
}

static efi_status EFI_API faketcg2_extend(struct efi_tcg2_protocol *this, uint64_t flags, efi_physical_address data,
                                          uint64_t len, struct efi_tcg2_event *event) {
    (void)this;
    out("K tcg2.extend=%u %x %x %x %u %e\n", (uint64_t)event->header.pcr_index, (uint64_t)event->header.event_type,
        (uint64_t)flags, fnv1a((const uint8_t *)(uintptr_t)data, len), (uint64_t)len, (const char *)event->event);
    return (faketcg2_flags & 2) ? EFI_DEVICE_ERROR : (faketcg2_flags & 8) ? EFI_VOLUME_FULL : EFI_SUCCESS;
}

static struct efi_tcg2_protocol faketcg2 = { faketcg2_capability, faketcg2_event_log, faketcg2_extend, NULL, NULL, NULL,
                                      NULL };
static bool faketcg2_pending;

// Installed by chain, as the CC protocol is.
static void cmd_faketcg2(char *arg) {
    faketcg2_flags = (uint32_t)parse_hex(arg, NULL);
    faketcg2_pending = true;
}

static void install_faketcg2(void) {
    struct efi_guid g = EFI_TCG2_PROTOCOL_GUID;
    efi_handle h = NULL;
    out("K faketcg2=%x\n", (uint64_t)bs->install_protocol_interface(&h, &g, EFI_NATIVE_INTERFACE, &faketcg2));
}

static void cmd_chain(char *arg) {
    char *opts = arg;
    while (*opts != 0 && *opts != '@') {
        opts++;
    }
    bool have_opts = *opts == '@';
    if (have_opts) {
        *opts++ = 0;
    }
    struct efi_device_path_protocol *dp = file_devpath(arg);
    efi_handle h = NULL;
    efi_status s = bs->load_image(false, self, dp, NULL, 0, &h);
    out("K chain.load=%x\n", (uint64_t)s);
    if (s != EFI_SUCCESS) {
        return;
    }
    if (fakecc_pending) {
        install_fakecc();
    }
    if (faketcg2_pending) {
        install_faketcg2();
    }
    if (have_opts) {
        struct efi_guid lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        struct efi_loaded_image_protocol *li = NULL;
        bs->handle_protocol(h, &lig, (void **)&li);
        size_t n = slen(opts);
        uint16_t *w = alloc((n + 1) * 2);
        for (size_t i = 0; i < n; i++) {
            w[i] = opts[i] == '_' ? ' ' : (uint8_t)opts[i];
        }
        w[n] = 0;
        li->load_options = w;
        li->load_options_size = (n + 1) * 2;
    }
    size_t exit_size = 0;
    uint16_t *exit_data = NULL;
    out("B CHAIN-RETURN-PENDING\n");
    s = bs->start_image(h, &exit_size, &exit_data);
    out("\nK chain.status=%x\n", (uint64_t)s);
    out("K chain.exit_data_size=%u\n", (uint64_t)exit_size);
    out("K chain.exit_data=%w\n", exit_size > 0 ? exit_data : (uint16_t *)L"");
}

static efi_status cmd_exit(char *arg) {
    const char *e;
    size_t status = (size_t)parse_hex(arg, &e);
    if (sizeof(size_t) == 8 && (uint64_t)status >> 31 == 1) {
        // Written as a 32-bit error code: give it a 64-bit status's error bit.
        status = (size_t)(((uint64_t)status & 0x7fffffff) | ((uint64_t)1 << 63));
    }
    if (*e == '@') {
        e++;
        size_t n = slen(e);
        uint16_t *data;
        bs->allocate_pool(EFI_BOOT_SERVICES_DATA, (n + 1) * 2, (void **)&data);
        for (size_t i = 0; i < n; i++) {
            data[i] = e[i] == '_' ? ' ' : (uint8_t)e[i];
        }
        data[n] = 0;
        out("E DONE\n");
        bs->exit(self, status, (n + 1) * 2, data);
    }
    out("E DONE\n");
    return status;
}

// A file from the root of this image's volume, in pool memory of the given
// type; NULL if it cannot be read.
static void *read_file(const uint16_t *name, uint32_t type, size_t *size) {
    struct efi_guid sfsg = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    struct efi_simple_file_system_protocol *sfs = NULL;
    struct efi_file_protocol *root = NULL, *f = NULL;
    if (loaded->device_handle == NULL
     || bs->handle_protocol(loaded->device_handle, &sfsg, (void **)&sfs) != EFI_SUCCESS
     || sfs->open_volume(sfs, &root) != EFI_SUCCESS) {
        return NULL;
    }
    if (root->open(root, &f, (uint16_t *)name, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        root->close(root);
        return NULL;
    }
    void *buf = NULL;
    // All ones seeks to the end (UEFI 2.11 section 13.5.11).
    uint64_t end = 0;
    if (f->set_position(f, UINT64_MAX) == EFI_SUCCESS && f->get_position(f, &end) == EFI_SUCCESS
     && f->set_position(f, 0) == EFI_SUCCESS
     && bs->allocate_pool(type, (size_t)end, &buf) == EFI_SUCCESS) {
        *size = (size_t)end;
        if (f->read(f, size, buf) != EFI_SUCCESS) {
            bs->free_pool(buf);
            buf = NULL;
        }
    }
    f->close(f);
    root->close(root);
    return buf;
}

// pointers:N reports, for N seconds, each state the simple pointers give.
static void cmd_pointers(char *arg) {
    uint64_t seconds = parse_hex(arg, NULL);
    struct efi_guid g = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    size_t count = 0;
    efi_handle *handles = NULL;
    if (bs->locate_handle_buffer(EFI_BY_PROTOCOL, &g, NULL, &count, &handles) != EFI_SUCCESS) {
        count = 0;
    }
    out("K pointers.count=%u\n", (uint64_t)count);
    struct efi_simple_pointer_protocol *p[8];
    size_t n = 0;
    for (size_t i = 0; i < count && n < 8; i++) {
        if (bs->handle_protocol(handles[i], &g, (void **)&p[n]) == EFI_SUCCESS) {
            out("K pointers.%u.resolution=%u %u %u\n", (uint64_t)n, p[n]->mode->resolution_x,
                p[n]->mode->resolution_y, p[n]->mode->resolution_z);
            p[n]->reset(p[n], false);
            n++;
        }
    }
    out("B POINTERS-READY\n");
    for (uint64_t ms = 0; ms < seconds * 1000; ms += 10) {
        for (size_t i = 0; i < n; i++) {
            struct efi_simple_pointer_state st;
            if (p[i]->get_state(p[i], &st) == EFI_SUCCESS) {
                out("K pointer.%u=%x %x %x %u %u\n", (uint64_t)i, (uint64_t)(int64_t)st.relative_movement_x,
                    (uint64_t)(int64_t)st.relative_movement_y, (uint64_t)(int64_t)st.relative_movement_z,
                    (uint64_t)st.left_button, (uint64_t)st.right_button);
            }
        }
        bs->stall(10000);
    }
}

// uninstall:GUID takes the protocol off every handle it is on.
static void cmd_uninstall(char *arg) {
    struct efi_guid g;
    if (!parse_guid(arg, &g)) {
        out("K error=bad guid %e\n", arg);
        return;
    }
    size_t count = 0;
    efi_handle *handles = NULL;
    if (bs->locate_handle_buffer(EFI_BY_PROTOCOL, &g, NULL, &count, &handles) != EFI_SUCCESS) {
        out("K uninstall.count=0\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        void *iface = NULL;
        bs->handle_protocol(handles[i], &g, &iface);
        efi_status s = bs->uninstall_protocol_interface(handles[i], &g, iface);
        out("K uninstall.%u=%x\n", (uint64_t)i, (uint64_t)s);
    }
    out("K uninstall.count=%u\n", (uint64_t)count);
    bs->free_pool(handles);
}

// dtb:NAME installs the device tree in that file, at the root of this image's
// volume, as the firmware's device tree configuration table.
static void cmd_dtb(char *arg) {
    uint16_t name[256];
    to_utf16(name, arg, 256);
    size_t size = 0;
    void *blob = read_file(name, EFI_ACPI_RECLAIM_MEMORY, &size);
    if (blob == NULL) {
        out("K dtb=unreadable\n");
        return;
    }
    struct efi_guid g = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
    out("K dtb=%x\n", (uint64_t)bs->install_configuration_table(&g, blob));
}

// A disk whose Block I/O claims a transfer length granularity of 512 blocks
// yet fails reads over a limit with EFI_OUT_OF_RESOURCES, as some firmware
// does. It reports each read it fails.
static uint8_t *fakedisk_data;
static uint64_t fakedisk_limit;
static struct efi_block_io_media fakedisk_media;

static efi_status EFI_API fakedisk_reset(struct efi_block_io_protocol *this, bool extended) {
    (void)this;
    (void)extended;
    return EFI_SUCCESS;
}

static efi_status EFI_API fakedisk_read(
    struct efi_block_io_protocol *this, uint32_t media_id, efi_lba lba, size_t size, void *buf
) {
    (void)this;
    if (media_id != fakedisk_media.media_id) {
        return EFI_MEDIA_CHANGED;
    }
    if (size % fakedisk_media.block_size != 0) {
        return EFI_BAD_BUFFER_SIZE;
    }
    if (buf == NULL || ((uintptr_t)buf & (fakedisk_media.io_align - 1)) != 0
     || lba > fakedisk_media.last_block
     || size / fakedisk_media.block_size > fakedisk_media.last_block + 1 - lba) {
        return EFI_INVALID_PARAMETER;
    }
    if (size > fakedisk_limit) {
        out("K fakedisk.refused=%x\n", (uint64_t)size);
        return EFI_OUT_OF_RESOURCES;
    }
    memcpy(buf, fakedisk_data + lba * fakedisk_media.block_size, size);
    return EFI_SUCCESS;
}

static efi_status EFI_API fakedisk_write(
    struct efi_block_io_protocol *this, uint32_t media_id, efi_lba lba, size_t size, void *buf
) {
    (void)this;
    (void)media_id;
    (void)lba;
    (void)size;
    (void)buf;
    return EFI_WRITE_PROTECTED;
}

static efi_status EFI_API fakedisk_flush(struct efi_block_io_protocol *this) {
    (void)this;
    return EFI_SUCCESS;
}

static struct efi_block_io_protocol fakedisk = {
    EFI_BLOCK_IO_PROTOCOL_REVISION3, &fakedisk_media, fakedisk_reset, fakedisk_read,
    fakedisk_write, fakedisk_flush
};

// fakedisk:LIMIT:NAME installs it over the image in file NAME, at the root of
// this image's volume, failing reads over LIMIT bytes.
static void cmd_fakedisk(char *arg) {
    const char *end;
    fakedisk_limit = parse_hex(arg, &end);
    uint16_t name[256];
    to_utf16(name, *end == ':' ? end + 1 : "", 256);
    size_t size = 0;
    fakedisk_data = read_file(name, EFI_LOADER_DATA, &size);
    if (fakedisk_data == NULL || size < 512) {
        out("K fakedisk=unreadable\n");
        return;
    }
    fakedisk_media.media_present = true;
    fakedisk_media.read_only = true;
    fakedisk_media.block_size = 512;
    fakedisk_media.io_align = 4;
    fakedisk_media.last_block = size / 512 - 1;
    fakedisk_media.logical_blocks_per_physical_block = 1;
    fakedisk_media.optimal_transfer_length_granularity = 512;
    struct efi_guid g = EFI_BLOCK_IO_PROTOCOL_GUID;
    efi_handle h = NULL;
    efi_status s = bs->install_protocol_interface(&h, &g, EFI_NATIVE_INTERFACE, &fakedisk);
    out("K fakedisk=%x\n", (uint64_t)s);
}

#if defined(__x86_64__) || defined(__i386__)
static const uint8_t *acpi_table(const char *sig) {
    struct efi_guid acpi20 = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
    const uint8_t *rsdp = NULL;
    for (size_t i = 0; i < st->number_of_table_entries; i++) {
        if (memcmp(&st->configuration_table[i].vendor_guid, &acpi20, sizeof(acpi20)) == 0) {
            rsdp = st->configuration_table[i].vendor_table;
        }
    }
    if (rsdp == NULL) {
        return NULL;
    }
    uint64_t xsdt;
    memcpy(&xsdt, rsdp + 24, 8);
    uint32_t len;
    memcpy(&len, (const uint8_t *)(uintptr_t)xsdt + 4, 4);
    for (uint32_t off = 36; off + 8 <= len; off += 8) {
        uint64_t p;
        memcpy(&p, (const uint8_t *)(uintptr_t)xsdt + off, 8);
        if (memcmp((const void *)(uintptr_t)p, sig, 4) == 0) {
            return (const uint8_t *)(uintptr_t)p;
        }
    }
    return NULL;
}

static uint32_t vtd_command(volatile uint8_t *regs, uint32_t bit) {
    // Intel VT-d 5.20 section 11.4.4.1: the one-shot bits of the status are
    // not written back, and the command is done when its status bit says so.
    uint32_t status = *(volatile uint32_t *)(regs + 0x1c) & 0x96ffffff;
    *(volatile uint32_t *)(regs + 0x18) = status | bit;
    while ((*(volatile uint32_t *)(regs + 0x1c) & bit) == 0) {
    }
    return *(volatile uint32_t *)(regs + 0x1c);
}

// DMA remapping on in each VT-d unit, every device passed through, as
// firmware with DMA protection leaves it: every bus's root entry names one
// context table, whose every entry is a pass-through one (Intel VT-d 5.20
// sections 9.1 and 9.3). Queued invalidation goes on too where the unit has
// it, on a queue of 256 empty entries (Intel VT-d 5.20 section 6.5.2).
// Interrupt remapping does not: with it on, QEMU's firmware stops getting its
// interrupts.
static void cmd_vtdon(void) {
    const uint8_t *dmar = acpi_table("DMAR");
    efi_physical_address tables = 0xffffffff;
    if (dmar == NULL || bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_RESERVED_MEMORY_TYPE, 3, &tables) != EFI_SUCCESS) {
        out("K vtd=unavailable\n");
        return;
    }
    memset((void *)(uintptr_t)tables, 0, 3 * 4096);
    volatile uint64_t *root = (volatile uint64_t *)(uintptr_t)tables, *ctx = root + 512;
    uint32_t len;
    memcpy(&len, dmar + 4, 4);
    for (uint32_t off = 48; off + 16 <= len;) {
        uint16_t type, size;
        uint64_t base;
        memcpy(&type, dmar + off, 2);
        memcpy(&size, dmar + off + 2, 2);
        memcpy(&base, dmar + off + 8, 8);
        if (size < 4) {
            break;
        }
        off += size;
        if (type != 0) {
            continue;
        }
        volatile uint8_t *regs = (volatile uint8_t *)(uintptr_t)base;
        uint64_t cap = *(volatile uint64_t *)(regs + 0x08), ecap = *(volatile uint64_t *)(regs + 0x10);
        // The largest address width the unit supports, as pass-through wants.
        uint64_t aw = (cap >> 11) & 1 ? 3 : (cap >> 10) & 1 ? 2 : 1;
        for (int i = 0; i < 256; i++) {
            root[2 * i] = (tables + 4096) | 1;
            root[2 * i + 1] = 0;
            ctx[2 * i] = (2 << 2) | 1;
            ctx[2 * i + 1] = aw | (1 << 8);
        }
        *(volatile uint64_t *)(regs + 0x20) = tables;
        vtd_command(regs, 1u << 30);
        // Global context-cache and IOTLB invalidation.
        *(volatile uint64_t *)(regs + 0x28) = (1ull << 63) | (1ull << 61);
        while (*(volatile uint64_t *)(regs + 0x28) >> 63) {
        }
        volatile uint64_t *iotlb = (volatile uint64_t *)(regs + ((ecap >> 8) & 0x3ff) * 16 + 8);
        *iotlb = (1ull << 63) | (1ull << 60);
        while (*iotlb >> 63) {
        }
        uint32_t status = vtd_command(regs, 1u << 31);
        if (ecap & (1 << 1)) {
            *(volatile uint64_t *)(regs + 0x88) = 0;
            *(volatile uint64_t *)(regs + 0x90) = tables + 2 * 4096;
            status = vtd_command(regs, 1u << 26);
        }
        out("K vtd=%x\n", (uint64_t)status);
    }
}

// AMD-Vi on in each IOMMU, with its command buffer and event log, every
// device passed through by a device table of no valid entries (AMD IOMMU
// 3.11 sections 2.2.2 and 3.4).
static void cmd_amdvion(void) {
    const uint8_t *ivrs = acpi_table("IVRS");
    // The device table for all 65536 device IDs, then the buffer and the log.
    efi_physical_address tables = 0xffffffff;
    if (ivrs == NULL || bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_RESERVED_MEMORY_TYPE, 514, &tables) != EFI_SUCCESS) {
        out("K amdvi=unavailable\n");
        return;
    }
    memset((void *)(uintptr_t)tables, 0, 514 * 4096);
    uint32_t len;
    memcpy(&len, ivrs + 4, 4);
    for (uint32_t off = 48; off + 16 <= len;) {
        uint8_t type = ivrs[off];
        uint16_t size;
        uint64_t base;
        memcpy(&size, ivrs + off + 2, 2);
        memcpy(&base, ivrs + off + 8, 8);
        if (size < 4) {
            break;
        }
        off += size;
        if (type != 0x10 && type != 0x11 && type != 0x40) {
            continue;
        }
        volatile uint8_t *regs = (volatile uint8_t *)(uintptr_t)base;
        *(volatile uint64_t *)(regs + 0x00) = tables | 0x1ff;
        // 256 entries each, the least there is.
        *(volatile uint64_t *)(regs + 0x08) = (tables + 512 * 4096) | (8ull << 56);
        *(volatile uint64_t *)(regs + 0x10) = (tables + 513 * 4096) | (8ull << 56);
        // IommuEn, EventLogEn, CmdBufEn.
        *(volatile uint64_t *)(regs + 0x18) |= (1 << 0) | (1 << 2) | (1 << 12);
        out("K amdvi=%x %x\n", *(volatile uint64_t *)(regs + 0x18), *(volatile uint64_t *)(regs + 0x2020));
    }
}

static void cmd_msr(char *arg) {
    uint32_t msr = (uint32_t)parse_hex(arg, NULL);
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    out("K msr.%x=%x\n", (uint64_t)msr, ((uint64_t)hi << 32) | lo);
}

static void acpi_checksum(uint8_t *table) {
    uint32_t len;
    memcpy(&len, table + 4, 4);
    uint8_t sum = 0;
    table[9] = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum += table[i];
    }
    table[9] = (uint8_t)-sum;
}

// The MPS INTI flags (hex) of every local APIC and local x2APIC NMI structure
// of the MADT set to these: madtnmi:FLAGS. ACPI 6.6 sections 5.2.12.7 and
// 5.2.12.13.
static void cmd_madtnmi(char *arg) {
    uint16_t flags = (uint16_t)parse_hex(arg, NULL);
    uint8_t *madt = (uint8_t *)acpi_table("APIC");
    if (madt == NULL) {
        out("K madtnmi=unavailable\n");
        return;
    }
    uint32_t len;
    memcpy(&len, madt + 4, 4);
    uint64_t count = 0;
    for (uint32_t off = 44; off + 2 <= len && madt[off + 1] >= 2; off += madt[off + 1]) {
        if (madt[off] == 4 && madt[off + 1] >= 6) {
            memcpy(madt + off + 3, &flags, 2);
            count++;
        } else if (madt[off] == 0xa && madt[off + 1] >= 12) {
            memcpy(madt + off + 2, &flags, 2);
            count++;
        }
    }
    acpi_checksum(madt);
    out("K madtnmi=%u\n", count);
}

// The first remapping unit of the DMAR, or of the IVRS, moved to memory that
// reads as a unit left on which never acts on being told to stop: VT-d with
// translation, interrupt remapping and queued invalidation on, its queue a
// page of its own (Intel VT-d 5.20 sections 11.4.4.2 and 11.4.7), or AMD-Vi
// with its command buffer and event log running (AMD IOMMU 3.11 sections
// 3.4.1 and 3.4.16): fakeiommu:vtd or fakeiommu:amdvi.
static void cmd_fakeiommu(char *arg) {
    bool vtd = sprefix(arg, "vtd");
    uint8_t *table = (uint8_t *)acpi_table(vtd ? "DMAR" : "IVRS");
    efi_physical_address regs = 0xffffffff;
    efi_status status = EFI_NOT_FOUND;
    if (table != NULL) {
        status = bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_RESERVED_MEMORY_TYPE, 4, &regs);
    }
    if (status != EFI_SUCCESS) {
        out("K fakeiommu=unavailable\n");
        return;
    }
    uint8_t *r = (uint8_t *)(uintptr_t)regs;
    memset(r, 0, 4 * 4096);
    if (vtd) {
        uint32_t gsts = (uint32_t)1 << 31 | (uint32_t)1 << 26 | (uint32_t)1 << 25;
        uint64_t iqa = regs + 3 * 4096;
        memcpy(r + 0x1c, &gsts, 4);
        memcpy(r + 0x90, &iqa, 8);
    } else {
        uint64_t control = (1 << 0) | (1 << 2) | (1 << 12);
        uint64_t status = (1 << 3) | (1 << 4);
        memcpy(r + 0x18, &control, 8);
        memcpy(r + 0x2020, &status, 8);
    }
    // The base address field of the unit's structure, the first of the table.
    memcpy(table + 48 + 8, &regs, 8);
    acpi_checksum(table);
    out("K fakeiommu=%x\n", (uint64_t)regs);
}

// The first remapping unit of the DMAR, or of the IVRS, moved to the
// configuration space of the last bus the MCFG covers, its base that of bus 0
// (PCI Firmware 3.3 section 4.1.2): with nothing on that bus, every read there
// gives all ones. deadiommu:vtd or deadiommu:amdvi.
static void cmd_deadiommu(char *arg) {
    uint8_t *table = (uint8_t *)acpi_table(sprefix(arg, "vtd") ? "DMAR" : "IVRS");
    const uint8_t *mcfg = acpi_table("MCFG");
    if (table == NULL || mcfg == NULL) {
        out("K deadiommu=unavailable\n");
        return;
    }
    uint64_t base;
    memcpy(&base, mcfg + 44, 8);
    base += (uint64_t)mcfg[55] << 20;
    memcpy(table + 48 + 8, &base, 8);
    acpi_checksum(table);
    out("K deadiommu=%x\n", base);
}
#endif

#if !defined(__x86_64__) && !defined(__i386__)
// SetVirtualAddressMap() replaced by code that returns EFI_UNSUPPORTED, in
// runtime services memory so that it outlives boot services.
static void cmd_svamfail(void) {
#if defined(__aarch64__)
    static const uint32_t code[] = { 0xd2800060, 0xf2f00000, 0xd65f03c0 };
#elif defined(__riscv)
    static const uint32_t code[] = { 0xfff00513, 0x03f51513, 0x00350513, 0x00008067 };
#elif defined(__loongarch64)
    static const uint32_t code[] = { 0x03200004, 0x03800c84, 0x4c000020 };
#endif
    efi_physical_address page = 0;
    efi_status s = bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_RUNTIME_SERVICES_CODE, 1, &page);
    if (s != EFI_SUCCESS) {
        out("K svamfail=%x\n", (uint64_t)s);
        return;
    }
    uint32_t *dst = (uint32_t *)(uintptr_t)page;
    for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
        dst[i] = code[i];
    }
#if defined(__aarch64__)
    asm volatile ("dc cvau, %0\n\tdsb ish\n\tic ivau, %0\n\tdsb ish\n\tisb" :: "r"(dst) : "memory");
#elif defined(__riscv)
    asm volatile ("fence.i" ::: "memory");
#elif defined(__loongarch64)
    asm volatile ("ibar 0" ::: "memory");
#endif
    rt->set_virtual_address_map = (efi_set_virtual_address_map)(uintptr_t)dst;
    rt->hdr.crc32 = 0;
    uint32_t crc = 0;
    bs->calculate_crc32(rt, rt->hdr.header_size, &crc);
    rt->hdr.crc32 = crc;
    out("K svamfail=0\n");
}
#endif

static efi_status EFI_API failing_watchdog(size_t timeout, uint64_t code, size_t size,
                                           uint16_t *data) {
    (void)timeout;
    (void)code;
    (void)size;
    (void)data;
    return EFI_DEVICE_ERROR;
}

// SetWatchdogTimer() replaced by one that always fails; the probe stays
// loaded while the image it chains to runs.
static void cmd_wdfail(void) {
    bs->set_watchdog_timer = failing_watchdog;
    bs->hdr.crc32 = 0;
    uint32_t crc = 0;
    bs->calculate_crc32(bs, bs->hdr.header_size, &crc);
    bs->hdr.crc32 = crc;
    out("K wdfail=0\n");
}

static efi_exit_boot_services real_exit_boot_services;
static uint64_t ebs_failures, ebs_fragment_pages;

static efi_status EFI_API failing_exit_boot_services(efi_handle image, size_t key) {
    if (ebs_failures == 0) {
        return real_exit_boot_services(image, key);
    }
    ebs_failures--;
    for (; ebs_fragment_pages > 0; ebs_fragment_pages--) {
        efi_physical_address page = 0;
        bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, ebs_fragment_pages % 2 ? EFI_LOADER_DATA : EFI_BOOT_SERVICES_DATA,
                          1, &page);
    }
    return EFI_INVALID_PARAMETER;
}

// ExitBootServices() failing n (hex) times as for a stale map key, the first
// time after allocating p (hex) single pages of alternating types, which grow
// the map: ebsfail:n[:p].
static void cmd_ebsfail(char *arg) {
    const char *end;
    ebs_failures = parse_hex(arg, &end);
    ebs_fragment_pages = *end == ':' ? parse_hex(end + 1, NULL) : 0;
    real_exit_boot_services = bs->exit_boot_services;
    bs->exit_boot_services = failing_exit_boot_services;
    bs->hdr.crc32 = 0;
    uint32_t crc = 0;
    bs->calculate_crc32(bs, bs->hdr.header_size, &crc);
    bs->hdr.crc32 = crc;
    out("K ebsfail=0\n");
}

// An OS loader's own memory type, UEFI 2.11 section 7.2.1, which no firmware
// allocation shares.
#define MEMATTR_TYPE 0x80000000
#define MEMATTR_PAGES 64

static efi_get_memory_map real_get_memory_map;
static uint64_t memattr_attribute;

static efi_status EFI_API memattr_get_memory_map(
    size_t *size, struct efi_memory_descriptor *map, size_t *key, size_t *desc_size,
    uint32_t *desc_version
) {
    efi_status s = real_get_memory_map(size, map, key, desc_size, desc_version);
    if (s != EFI_SUCCESS) {
        return s;
    }
    for (size_t off = 0; off + *desc_size <= *size; off += *desc_size) {
        struct efi_memory_descriptor *d = (struct efi_memory_descriptor *)((uint8_t *)map + off);
        if (d->type == MEMATTR_TYPE) {
            d->type = EFI_CONVENTIONAL_MEMORY;
            d->attribute = memattr_attribute;
        }
    }
    return s;
}

// Pages the probe holds that GetMemoryMap() reports as conventional memory
// with these attributes (hex): memattr:ATTRIBUTES.
static void cmd_memattr(char *arg) {
    memattr_attribute = parse_hex(arg, NULL);
    efi_physical_address base = 0xffffffff;
    efi_status s = bs->allocate_pages(
        EFI_ALLOCATE_MAX_ADDRESS, (uint32_t)MEMATTR_TYPE, MEMATTR_PAGES, &base
    );
    if (s != EFI_SUCCESS) {
        out("K memattr=%x\n", (uint64_t)s);
        return;
    }
    real_get_memory_map = bs->get_memory_map;
    bs->get_memory_map = memattr_get_memory_map;
    bs->hdr.crc32 = 0;
    uint32_t crc = 0;
    bs->calculate_crc32(bs, bs->hdr.header_size, &crc);
    bs->hdr.crc32 = crc;
    out("K memattr=%x %x\n", (uint64_t)base, (uint64_t)MEMATTR_PAGES * 4096);
}

// n (hex) single pages, of types that stay apart once boot services are gone
// and so leave a memory map of as many entries: fragment:n.
static void cmd_fragment(char *arg) {
    uint64_t pages = parse_hex(arg, NULL);
    efi_status s = EFI_SUCCESS;
    for (uint64_t i = 0; i < pages && s == EFI_SUCCESS; i++) {
        uint32_t type = i % 2 == 0 ? EFI_LOADER_DATA : EFI_ACPI_RECLAIM_MEMORY;
        efi_physical_address page = 0;
        s = bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, type, 1, &page);
    }
    out("K fragment=%x\n", (uint64_t)s);
}

// A Linux random seed table of n (hex) bytes, as an earlier stage would
// leave one.
static void cmd_seedtable(char *arg) {
    uint32_t n = (uint32_t)parse_hex(arg, NULL);
    uint8_t *table = NULL;
    efi_status s = bs->allocate_pool(EFI_ACPI_RECLAIM_MEMORY, 4 + n, (void **)&table);
    if (s != EFI_SUCCESS) {
        out("K seedtable=%x\n", (uint64_t)s);
        return;
    }
    for (int i = 0; i < 4; i++) {
        table[i] = (uint8_t)(n >> (8 * i));
    }
    for (uint32_t i = 0; i < n; i++) {
        table[4 + i] = (uint8_t)(0xa5 ^ i);
    }
    struct efi_guid g = { 0x1ce1e5bc, 0x7ceb, 0x42f2, { 0x81, 0xe5, 0x8a, 0xad, 0xf1, 0x80, 0xf5, 0x7b } };
    out("K seedtable=%x\n", (uint64_t)bs->install_configuration_table(&g, table));
}

// An SMBIOS 3.0 entry point in place of the firmware's, giving a structure
// table at ADDR of SIZE bytes (both hex, as ADDR:SIZE).
static void cmd_smbios3(char *arg) {
    const char *rest;
    uint64_t addr = parse_hex(arg, &rest);
    uint32_t size = *rest == ':' ? (uint32_t)parse_hex(rest + 1, NULL) : 0;
    uint8_t *ep = NULL;
    efi_status s = bs->allocate_pool(EFI_RUNTIME_SERVICES_DATA, 24, (void **)&ep);
    if (s != EFI_SUCCESS) {
        out("K smbios3=%x\n", (uint64_t)s);
        return;
    }
    // DSP0134 3.9.0 section 5.2.2: the anchor, the checksum, the length,
    // version 3.0, entry point revision 1, then the table's size and address.
    memset(ep, 0, 24);
    memcpy(ep, "_SM3_", 5);
    ep[6] = 24;
    ep[7] = 3;
    ep[10] = 1;
    for (int i = 0; i < 4; i++) {
        ep[12 + i] = (uint8_t)(size >> (8 * i));
    }
    for (int i = 0; i < 8; i++) {
        ep[16 + i] = (uint8_t)(addr >> (8 * i));
    }
    uint8_t sum = 0;
    for (int i = 0; i < 24; i++) {
        sum += ep[i];
    }
    ep[5] = (uint8_t)-sum;
    struct efi_guid g = EFI_SMBIOS3_TABLE_GUID;
    out("K smbios3=%x\n", (uint64_t)bs->install_configuration_table(&g, ep));
}

#if defined(__riscv)
// Translation off, as some RISC-V firmware runs: the firmware's identity map
// leaves every address the same without it.
static void cmd_satpoff(void) {
    asm volatile ("csrw satp, zero\n\tsfence.vma" ::: "memory");
    out("K satpoff=0\n");
}
#endif

static char cmdbuf[4096];

static size_t read_cmd_file(void) {
    struct efi_guid sfsg = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    struct efi_simple_file_system_protocol *sfs = NULL;
    if (loaded->device_handle == NULL
     || bs->handle_protocol(loaded->device_handle, &sfsg, (void **)&sfs) != EFI_SUCCESS) {
        return 0;
    }
    struct efi_file_protocol *root = NULL, *f = NULL;
    if (sfs->open_volume(sfs, &root) != EFI_SUCCESS) {
        return 0;
    }
    if (root->open(root, &f, L"efiprobe.cmd", EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        return 0;
    }
    size_t size = sizeof(cmdbuf) - 1;
    f->read(f, &size, cmdbuf);
    f->close(f);
    root->close(root);
    cmdbuf[size] = 0;
    return size;
}

efi_status efi_main(efi_handle image, struct efi_system_table *system_table) {
    st = system_table;
    bs = st->boot_services;
    rt = st->runtime_services;
    self = image;
    struct efi_guid lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    bs->handle_protocol(image, &lig, (void **)&loaded);
#if !defined(__x86_64__) && !defined(__i386__)
    struct efi_guid sg = EFI_SERIAL_IO_PROTOCOL_GUID;
    if (bs->locate_protocol(&sg, NULL, (void **)&serial) != EFI_SUCCESS) {
        serial = NULL;
    }
#endif

    size_t n = 0;
    if (loaded->load_options != NULL && loaded->load_options_size >= 2) {
        uint16_t *o = loaded->load_options;
        for (size_t i = 0; i < loaded->load_options_size / 2 && o[i] != 0 && n + 1 < sizeof(cmdbuf); i++) {
            cmdbuf[n++] = o[i] < 0x80 ? (char)o[i] : '?';
        }
        cmdbuf[n] = 0;
    }
    if (n == 0) {
        n = read_cmd_file();
    }

    out("\nB EFIPROBE\n");
    char *p = cmdbuf;
    while (*p != 0) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        char *tok = p;
        while (*p != 0 && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
            p++;
        }
        if (*p != 0) {
            *p++ = 0;
        }
        if (sprefix(tok, "name:")) {
            out("K name=%e\n", tok + 5);
        } else if (sprefix(tok, "info")) {
            cmd_info();
        } else if (sprefix(tok, "fakegop:")) {
            cmd_fakegop(tok + 8);
        } else if (sprefix(tok, "gop")) {
            cmd_gop();
        } else if (sprefix(tok, "time")) {
            cmd_time();
        } else if (sprefix(tok, "tables")) {
            cmd_tables();
        } else if (sprefix(tok, "memmap")) {
            cmd_memmap();
        } else if (sprefix(tok, "blockio")) {
            cmd_blockio();
        } else if (sprefix(tok, "var:")) {
            cmd_var(tok + 4);
        } else if (sprefix(tok, "vendor:")) {
            cmd_vendor_vars(tok + 7);
        } else if (sprefix(tok, "setvar:")) {
            cmd_setvar(tok + 7);
        } else if (sprefix(tok, "addboot:")) {
            cmd_addboot(tok + 8);
        } else if (sprefix(tok, "pointers:")) {
            cmd_pointers(tok + 9);
        } else if (sprefix(tok, "uninstall:")) {
            cmd_uninstall(tok + 10);
        } else if (sprefix(tok, "dtb:")) {
            cmd_dtb(tok + 4);
        } else if (sprefix(tok, "seedtable:")) {
            cmd_seedtable(tok + 10);
        } else if (sprefix(tok, "memattr:")) {
            cmd_memattr(tok + 8);
        } else if (sprefix(tok, "fragment:")) {
            cmd_fragment(tok + 9);
        } else if (sprefix(tok, "smbios3:")) {
            cmd_smbios3(tok + 8);
        } else if (sprefix(tok, "wdfail")) {
            cmd_wdfail();
        } else if (sprefix(tok, "ebsfail:")) {
            cmd_ebsfail(tok + 8);
        } else if (sprefix(tok, "fakecc:")) {
            cmd_fakecc(tok + 7);
        } else if (sprefix(tok, "faketcg2:")) {
            cmd_faketcg2(tok + 9);
        } else if (sprefix(tok, "fakedisk:")) {
            cmd_fakedisk(tok + 9);
#if defined(__riscv)
        } else if (sprefix(tok, "satpoff")) {
            cmd_satpoff();
#endif
#if defined(__x86_64__) || defined(__i386__)
        } else if (sprefix(tok, "msr:")) {
            cmd_msr(tok + 4);
        } else if (sprefix(tok, "vtdon")) {
            cmd_vtdon();
        } else if (sprefix(tok, "amdvion")) {
            cmd_amdvion();
        } else if (sprefix(tok, "madtnmi:")) {
            cmd_madtnmi(tok + 8);
        } else if (sprefix(tok, "fakeiommu:")) {
            cmd_fakeiommu(tok + 10);
        } else if (sprefix(tok, "deadiommu:")) {
            cmd_deadiommu(tok + 10);
#else
        } else if (sprefix(tok, "svamfail")) {
            cmd_svamfail();
#endif
        } else if (sprefix(tok, "chain:")) {
            cmd_chain(tok + 6);
        } else if (sprefix(tok, "stall:")) {
            bs->stall((size_t)parse_hex(tok + 6, NULL) * 1000);
        } else if (sprefix(tok, "exit:")) {
            return cmd_exit(tok + 5);
        } else if (sprefix(tok, "reset:")) {
            out("E DONE\n");
            uint32_t t = EFI_RESET_WARM;
            if (sprefix(tok + 6, "cold")) {
                t = EFI_RESET_COLD;
            } else if (sprefix(tok + 6, "shutdown")) {
                t = EFI_RESET_SHUTDOWN;
            }
            rt->reset_system(t, EFI_SUCCESS, 0, NULL);
        } else if (sprefix(tok, "hang")) {
            out("E DONE\n");
            for (;;) {
                bs->stall(1000000);
            }
        } else {
            out("K error=unknown command %e\n", tok);
        }
    }
    out("E DONE\n");
    return EFI_SUCCESS;
}
