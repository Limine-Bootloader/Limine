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

static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;
static EFI_RUNTIME_SERVICES *rt;
static EFI_HANDLE self;
static EFI_LOADED_IMAGE_PROTOCOL *loaded;
#if !defined (__x86_64__) && !defined (__i386__)
static EFI_SERIAL_IO_PROTOCOL *serial;
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
#if defined (__x86_64__) || defined (__i386__)
    asm volatile ("outb %0, $0xe9" :: "a"(c));
#else
    // ConOut's terminal can drop what runs past its last column, so the
    // report goes to the serial port itself.
    if (serial != NULL) {
        char crlf[2] = { '\r', c };
        EFI_UINTN size = c == '\n' ? 2 : 1;
        serial->Write(serial, &size, c == '\n' ? crlf : crlf + 1);
        return;
    }
#endif
    EFI_CHAR16 buf[3] = { (EFI_CHAR16)(uint8_t)c, 0, 0 };
    if (c == '\n') {
        buf[0] = '\r';
        buf[1] = '\n';
    }
    st->ConOut->OutputString(st->ConOut, buf);
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
                const EFI_CHAR16 *w = va_arg(ap, const EFI_CHAR16 *);
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

static bool parse_guid(const char *s, EFI_GUID *g) {
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    if (slen(s) < 36) {
        return false;
    }
    const char *e;
    g->Data1 = (uint32_t)parse_hex(s, &e);
    g->Data2 = (uint16_t)parse_hex(s + 9, &e);
    g->Data3 = (uint16_t)parse_hex(s + 14, &e);
    int idx[8] = { 19, 21, 24, 26, 28, 30, 32, 34 };
    for (int i = 0; i < 8; i++) {
        g->Data4[i] = (uint8_t)(hexval(s[idx[i]]) * 16 + hexval(s[idx[i] + 1]));
    }
    return true;
}

static void put_guid(const EFI_GUID *g) {
    putnum(g->Data1, 16, 8);
    putch('-');
    putnum(g->Data2, 16, 4);
    putch('-');
    putnum(g->Data3, 16, 4);
    putch('-');
    putnum(g->Data4[0], 16, 2);
    putnum(g->Data4[1], 16, 2);
    putch('-');
    for (int i = 2; i < 8; i++) {
        putnum(g->Data4[i], 16, 2);
    }
}

static void *alloc(size_t size) {
    void *p = NULL;
    if (bs->AllocatePool(EfiLoaderData, size, &p) != EFI_SUCCESS) {
        return NULL;
    }
    return p;
}

static void to_utf16(EFI_CHAR16 *dst, const char *src, size_t max) {
    size_t i = 0;
    for (; src[i] != 0 && i + 1 < max; i++) {
        dst[i] = (uint8_t)src[i];
    }
    dst[i] = 0;
}

static EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dp_text(void) {
    static EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *p;
    if (p == NULL) {
        EFI_GUID g = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
        bs->LocateProtocol(&g, NULL, (void **)&p);
    }
    return p;
}

static void put_devpath(EFI_DEVICE_PATH_PROTOCOL *dp) {
    if (dp == NULL) {
        puts_("(null)");
        return;
    }
    EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *t = dp_text();
    if (t == NULL) {
        puts_("(no text protocol)");
        return;
    }
    EFI_CHAR16 *s = t->ConvertDevicePathToText(dp, false, false);
    out("%w", s);
    if (s != NULL) {
        bs->FreePool(s);
    }
}

static size_t devpath_size(EFI_DEVICE_PATH_PROTOCOL *dp) {
    size_t total = 0;
    for (;;) {
        size_t len = dp->Length[0] | (dp->Length[1] << 8);
        total += len;
        if (dp->Type == EFI_END_DEVICE_PATH_TYPE && dp->SubType == EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE) {
            return total;
        }
        dp = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)dp + len);
    }
}

// The device path of a file on this image's own volume.
static EFI_DEVICE_PATH_PROTOCOL *file_devpath(const char *path) {
    EFI_GUID dpg = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dev = NULL;
    if (bs->HandleProtocol(loaded->DeviceHandle, &dpg, (void **)&dev) != EFI_SUCCESS) {
        return NULL;
    }
    size_t dev_size = devpath_size(dev) - 4;
    size_t plen = slen(path);
    size_t node = 4 + (plen + 1) * 2;
    uint8_t *buf = alloc(dev_size + node + 4);
    for (size_t i = 0; i < dev_size; i++) {
        buf[i] = ((uint8_t *)dev)[i];
    }
    EFI_DEVICE_PATH_PROTOCOL *fp = (EFI_DEVICE_PATH_PROTOCOL *)(buf + dev_size);
    fp->Type = EFI_MEDIA_DEVICE_PATH;
    fp->SubType = EFI_MEDIA_FILEPATH_DP;
    fp->Length[0] = node & 0xff;
    fp->Length[1] = node >> 8;
    EFI_CHAR16 *name = (EFI_CHAR16 *)(buf + dev_size + 4);
    to_utf16(name, path, plen + 1);
    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL *)(buf + dev_size + node);
    end->Type = EFI_END_DEVICE_PATH_TYPE;
    end->SubType = EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end->Length[0] = 4;
    end->Length[1] = 0;
    return (EFI_DEVICE_PATH_PROTOCOL *)buf;
}

static void cmd_info(void) {
    out("K options_size=%u\n", (uint64_t)loaded->LoadOptionsSize);
    if (loaded->LoadOptions != NULL) {
        EFI_CHAR16 *o = loaded->LoadOptions;
        out("K options=%w\n", o);
        // The last code unit the size covers, to see the terminator is counted.
        size_t units = loaded->LoadOptionsSize / 2;
        out("K options_last_unit=%x\n", (uint64_t)(units > 0 ? o[units - 1] : 0xffff));
    } else {
        out("K options=(null)\n");
    }
    out("K file_path=");
    put_devpath(loaded->FilePath);
    out("\nK device_path=");
    EFI_GUID dpg = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dev = NULL;
    if (loaded->DeviceHandle != NULL) {
        bs->HandleProtocol(loaded->DeviceHandle, &dpg, (void **)&dev);
    }
    put_devpath(dev);
    out("\nK image_code_type=%u\n", (uint64_t)loaded->ImageCodeType);
    EFI_GUID lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *parent = NULL;
    if (loaded->ParentHandle != NULL
     && bs->HandleProtocol(loaded->ParentHandle, &lig, (void **)&parent) == EFI_SUCCESS) {
        out("K parent_file_path=");
        put_devpath(parent->FilePath);
        out("\n");
    }
}

static void cmd_gop(void) {
    EFI_GUID g = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (bs->LocateProtocol(&g, NULL, (void **)&gop) != EFI_SUCCESS || gop == NULL) {
        out("K gop=none\n");
        return;
    }
    out("K gop.mode=%ux%u\n", (uint64_t)gop->Mode->Info->HorizontalResolution,
        (uint64_t)gop->Mode->Info->VerticalResolution);
}

// A graphics output protocol over reserved memory, with modes in the formats
// QEMU's display devices never offer, and an EDID that prefers 800x600.
struct fake_mode {
    uint32_t width, height, format, pitch;
    EFI_PIXEL_BITMASK masks;
};

static const struct fake_mode fake_modes[] = {
    { 640, 480, EfiPixelBitMask, 640, { 0xf800, 0x07e0, 0x001f, 0 } },
    { 800, 600, EfiPixelRedGreenBlueReserved8BitPerColor, 800, { 0, 0, 0, 0 } },
    { 1024, 768, EfiPixelBitMask, 1024, { 0, 0, 0, 0 } },
    { 1280, 1024, EfiPixelBltOnly, 1280, { 0, 0, 0, 0 } },
    { 640, 480, EfiPixelBlueGreenRedReserved8BitPerColor, 600, { 0, 0, 0, 0 } },
    { 320, 200, EfiPixelBitMask, 320, { 0x7c00, 0x03e0, 0x001f, 0x8000 } },
};
#define FAKE_MODES (sizeof(fake_modes) / sizeof(fake_modes[0]))

struct fake_gop {
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
    EFI_EDID_ACTIVE_PROTOCOL edid;
    bool started;
};

// Bit 0: QueryMode() says EFI_NOT_STARTED until SetMode() is called. Bit 1:
// SetMode() refuses mode 1, the EDID's. Bit 2: the framebuffer lies in boot
// services data, half a page into its pages.
#define FAKEGOP_NOT_STARTED 1
#define FAKEGOP_MODE1_FAILS 2
#define FAKEGOP_BS_DATA 4
static uint64_t fakegop_flags;
static EFI_PHYSICAL_ADDRESS fake_fb;
static uint8_t fake_edid[128];

static void fake_info(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info, uint32_t n) {
    info->Version = 0;
    info->HorizontalResolution = fake_modes[n].width;
    info->VerticalResolution = fake_modes[n].height;
    info->PixelFormat = fake_modes[n].format;
    info->PixelInformation = fake_modes[n].masks;
    info->PixelsPerScanLine = fake_modes[n].pitch;
}

static EFI_STATUS EFI_API fake_query_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *this, EFI_UINT32 n, EFI_UINTN *size,
                                          EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info) {
    if ((fakegop_flags & FAKEGOP_NOT_STARTED) && !((struct fake_gop *)this)->started) {
        return EFI_NOT_STARTED;
    }
    if (n >= FAKE_MODES) {
        return EFI_INVALID_PARAMETER;
    }
    *info = alloc(sizeof(**info));
    fake_info(*info, n);
    *size = sizeof(**info);
    return EFI_SUCCESS;
}

static EFI_STATUS EFI_API fake_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *this, EFI_UINT32 n) {
    struct fake_gop *f = (struct fake_gop *)this;
    if (n >= FAKE_MODES) {
        return EFI_UNSUPPORTED;
    }
    if ((fakegop_flags & FAKEGOP_MODE1_FAILS) && n == 1) {
        return EFI_DEVICE_ERROR;
    }
    f->started = true;
    f->mode.Mode = n;
    fake_info(&f->info, n);
    f->mode.FrameBufferSize = (EFI_UINTN)fake_modes[n].pitch * fake_modes[n].height * 4;
    return EFI_SUCCESS;
}

static EFI_STATUS EFI_API fake_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *this, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf,
                                   EFI_GRAPHICS_OUTPUT_BLT_OPERATION op, EFI_UINTN sx, EFI_UINTN sy,
                                   EFI_UINTN dx, EFI_UINTN dy, EFI_UINTN w, EFI_UINTN h, EFI_UINTN delta) {
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
            bs->AllocatePages(EfiAllocateAnyPages, EfiBootServicesData, pages + 1, &fake_fb);
            fake_fb += 2048;
        } else {
            bs->AllocatePages(EfiAllocateAnyPages, EfiReservedMemoryType, pages, &fake_fb);
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
    EFI_GUID gg = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GUID eg = EFI_EDID_ACTIVE_PROTOCOL_GUID;
    for (uint64_t i = 0; i < count; i++) {
        struct fake_gop *f = alloc(sizeof(*f));
        f->gop.QueryMode = fake_query_mode;
        f->gop.SetMode = fake_set_mode;
        f->gop.Blt = fake_blt;
        f->gop.Mode = &f->mode;
        f->mode.MaxMode = FAKE_MODES;
        f->mode.Info = &f->info;
        f->mode.SizeOfInfo = sizeof(f->info);
        f->mode.FrameBufferBase = fake_fb;
        fake_set_mode(&f->gop, 0);
        f->started = !(fakegop_flags & FAKEGOP_NOT_STARTED);
        f->edid.SizeOfEdid = sizeof(fake_edid);
        f->edid.Edid = fake_edid;
        EFI_HANDLE h = NULL;
        EFI_STATUS s = bs->InstallProtocolInterface(&h, &gg, EFI_NATIVE_INTERFACE, &f->gop);
        if (s == EFI_SUCCESS && i == 0) {
            s = bs->InstallProtocolInterface(&h, &eg, EFI_NATIVE_INTERFACE, &f->edid);
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
    EFI_TIME t;
    if (rt->GetTime(&t, NULL) != EFI_SUCCESS) {
        out("K time=error\n");
        return;
    }
    int64_t days = days_from_civil(t.Year, t.Month, t.Day);
    uint64_t since_epoch = (uint64_t)(days * 86400 + t.Hour * 3600 + t.Minute * 60 + t.Second);
    out("K time=%u-%2u-%2u %2u:%2u:%2u\n", (uint64_t)t.Year, (uint64_t)t.Month, (uint64_t)t.Day,
        (uint64_t)t.Hour, (uint64_t)t.Minute, (uint64_t)t.Second);
    out("K time.unix=%u\n", since_epoch);
    out("K time.timezone=%x\n", (uint64_t)(uint16_t)t.TimeZone);
}

// The firmware's memory map, and where this image's stack is.
static void cmd_memmap(void) {
    uint64_t sp;
    sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    out("K memmap.frame=%x\n", sp);
    EFI_UINTN size = 0, key, desc_size;
    EFI_UINT32 desc_ver;
    bs->GetMemoryMap(&size, NULL, &key, &desc_size, &desc_ver);
    size += 8 * desc_size;
    uint8_t *buf = NULL;
    if (bs->AllocatePool(EfiLoaderData, size, (void **)&buf) != EFI_SUCCESS) {
        out("K memmap.error=alloc\n");
        return;
    }
    if (bs->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)buf, &key, &desc_size, &desc_ver) != EFI_SUCCESS) {
        out("K memmap.error=get\n");
        bs->FreePool(buf);
        return;
    }
    for (EFI_UINTN off = 0; off + desc_size <= size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(buf + off);
        out("K memmap=%x %x %x %x\n", (uint64_t)d->Type, (uint64_t)d->PhysicalStart,
            (uint64_t)d->NumberOfPages, (uint64_t)d->Attribute);
    }
    bs->FreePool(buf);
}

// Block devices, in the order LocateHandle gives them.
static void cmd_blockio(void) {
    EFI_GUID bio = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID dpg = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_UINTN size = 0;
    bs->LocateHandle(EfiByProtocol, &bio, NULL, &size, NULL);
    EFI_HANDLE *handles = NULL;
    if (size == 0 || bs->AllocatePool(EfiLoaderData, size, (void **)&handles) != EFI_SUCCESS
     || bs->LocateHandle(EfiByProtocol, &bio, NULL, &size, handles) != EFI_SUCCESS) {
        out("K blockio.error=1\n");
        return;
    }
    for (EFI_UINTN i = 0; i < size / sizeof(EFI_HANDLE); i++) {
        EFI_BLOCK_IO_PROTOCOL *b = NULL;
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        bs->HandleProtocol(handles[i], &bio, (void **)&b);
        bs->HandleProtocol(handles[i], &dpg, (void **)&dp);
        out("K blockio=%u %u %u %x ", (uint64_t)b->Media->LogicalPartition, (uint64_t)b->Media->ReadOnly,
            (uint64_t)b->Media->BlockSize, (uint64_t)b->Media->LastBlock);
        put_devpath(dp);
        out("\n");
    }
    bs->FreePool(handles);
}

static void cmd_tables(void) {
    for (EFI_UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        out("K table.%u=", (uint64_t)i);
        put_guid(&st->ConfigurationTable[i].VendorGuid);
        out("\n");
    }
}

// name@guid
static bool split_var(char *arg, EFI_CHAR16 *name, size_t max, EFI_GUID *guid, char **rest) {
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

static void dump_var(const EFI_CHAR16 *name, EFI_GUID *guid) {
    static uint8_t buf[8192];
    EFI_UINTN size = sizeof(buf);
    EFI_UINT32 attrs = 0;
    EFI_STATUS s = rt->GetVariable((EFI_CHAR16 *)name, guid, &attrs, &size, buf);
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
    EFI_CHAR16 name[128];
    EFI_GUID guid;
    char *rest;
    if (!split_var(arg, name, 128, &guid, &rest)) {
        out("K error=bad var argument\n");
        return;
    }
    dump_var(name, &guid);
}

// Every variable of one vendor.
static void cmd_vendor_vars(char *arg) {
    EFI_GUID want;
    if (!parse_guid(arg, &want)) {
        out("K error=bad guid\n");
        return;
    }
    static EFI_CHAR16 name[512];
    name[0] = 0;
    EFI_GUID guid;
    for (;;) {
        EFI_UINTN size = sizeof(name);
        if (rt->GetNextVariableName(&size, name, &guid) != EFI_SUCCESS) {
            break;
        }
        bool match = true;
        for (size_t i = 0; i < sizeof(EFI_GUID); i++) {
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
    EFI_CHAR16 name[128];
    EFI_GUID guid;
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
    EFI_STATUS s = rt->SetVariable(name, &guid, attrs, n, n ? data : NULL);
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
    EFI_DEVICE_PATH_PROTOCOL *dp = file_devpath(path);
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
    to_utf16((EFI_CHAR16 *)(opt + 6), desc, dlen + 1);
    for (size_t i = 0; i < dpsize; i++) {
        opt[6 + (dlen + 1) * 2 + i] = ((uint8_t *)dp)[i];
    }
    EFI_GUID gv = EFI_GLOBAL_VARIABLE;
    EFI_CHAR16 name[9] = { 'B', 'o', 'o', 't', 0, 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        name[4 + i] = "0123456789ABCDEF"[(num >> (12 - 4 * i)) & 0xf];
    }
    uint32_t nv = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS
                | EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS s = rt->SetVariable(name, &gv, nv, total, opt);
    out("K addboot=%x\n", (uint64_t)s);
    static uint16_t order[256];
    EFI_UINTN size = sizeof(order) - 2;
    if (rt->GetVariable(L"BootOrder", &gv, NULL, &size, order) != EFI_SUCCESS) {
        size = 0;
    }
    order[size / 2] = num;
    s = rt->SetVariable(L"BootOrder", &gv, nv, size + 2, order);
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

static EFI_STATUS EFI_API fakecc_capability(EFI_CC_MEASUREMENT_PROTOCOL *this,
                                            EFI_CC_BOOT_SERVICE_CAPABILITY *cap) {
    (void)this;
    cap->StructureVersion = (EFI_CC_VERSION){ 1, 0 };
    cap->ProtocolVersion = (EFI_CC_VERSION){ 1, 0 };
    cap->HashAlgorithmBitmap = EFI_CC_BOOT_HASH_ALG_SHA384;
    cap->SupportedEventLogs = EFI_CC_EVENT_LOG_FORMAT_TCG_2;
    cap->CcType = (EFI_CC_TYPE){ EFI_CC_TYPE_INTEL_TDX, 0 };
    return EFI_SUCCESS;
}

static EFI_STATUS EFI_API fakecc_event_log(EFI_CC_MEASUREMENT_PROTOCOL *this, EFI_CC_EVENT_LOG_FORMAT format,
                                           EFI_PHYSICAL_ADDRESS *location, EFI_PHYSICAL_ADDRESS *last,
                                           EFI_BOOLEAN *truncated) {
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

static EFI_STATUS EFI_API fakecc_extend(EFI_CC_MEASUREMENT_PROTOCOL *this, EFI_UINT64 flags,
                                        EFI_PHYSICAL_ADDRESS data, EFI_UINT64 len, EFI_CC_EVENT *event) {
    (void)this;
    out("K cc.extend=%u %x %x %x %u %e\n", (uint64_t)event->Header.MrIndex, (uint64_t)event->Header.EventType,
        (uint64_t)flags, fnv1a((const uint8_t *)(uintptr_t)data, len), (uint64_t)len, (const char *)event->Event);
    return (fakecc_flags & 2) ? EFI_DEVICE_ERROR : (fakecc_flags & 8) ? EFI_VOLUME_FULL : EFI_SUCCESS;
}

static EFI_STATUS EFI_API fakecc_map(EFI_CC_MEASUREMENT_PROTOCOL *this, EFI_TCG_PCRINDEX pcr, EFI_CC_MR_INDEX *mr) {
    (void)this;
    out("K cc.map=%u\n", (uint64_t)pcr);
    if ((fakecc_flags & 1) || pcr > 15) {
        return EFI_INVALID_PARAMETER;
    }
    *mr = pcr == 0 ? 0 : pcr == 1 || pcr == 7 ? 1 : pcr <= 6 ? 2 : 3;
    return EFI_SUCCESS;
}

static EFI_CC_MEASUREMENT_PROTOCOL fakecc = { fakecc_capability, fakecc_event_log, fakecc_extend, fakecc_map };
static bool fakecc_pending;

// Installed by chain: once the image is loaded, for firmware that measures
// the images it loads not to measure that one through it.
static void cmd_fakecc(char *arg) {
    fakecc_flags = (uint32_t)parse_hex(arg, NULL);
    fakecc_pending = true;
}

static void install_fakecc(void) {
    EFI_GUID g = EFI_CC_MEASUREMENT_PROTOCOL_GUID;
    EFI_HANDLE h = NULL;
    out("K fakecc=%x\n", (uint64_t)bs->InstallProtocolInterface(&h, &g, EFI_NATIVE_INTERFACE, &fakecc));
}

// A TCG2 protocol over no TPM that claims one, reporting each call. Flags as
// the CC protocol's, but for 1, which it has no use for.
static uint32_t faketcg2_flags;

static EFI_STATUS EFI_API faketcg2_capability(EFI_TCG2_PROTOCOL *this, EFI_TCG2_BOOT_SERVICE_CAPABILITY *cap) {
    (void)this;
    cap->StructureVersion = (EFI_TCG2_VERSION){ 1, 1 };
    cap->ProtocolVersion = (EFI_TCG2_VERSION){ 1, 1 };
    cap->HashAlgorithmBitmap = EFI_TCG2_BOOT_HASH_ALG_SHA256;
    cap->SupportedEventLogs = EFI_TCG2_EVENT_LOG_FORMAT_TCG_2;
    cap->TPMPresentFlag = true;
    cap->NumberOfPcrBanks = 1;
    cap->ActivePcrBanks = EFI_TCG2_BOOT_HASH_ALG_SHA256;
    return EFI_SUCCESS;
}

static EFI_STATUS EFI_API faketcg2_event_log(EFI_TCG2_PROTOCOL *this, EFI_TCG2_EVENT_LOG_FORMAT format,
                                             EFI_PHYSICAL_ADDRESS *location, EFI_PHYSICAL_ADDRESS *last,
                                             EFI_BOOLEAN *truncated) {
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

static EFI_STATUS EFI_API faketcg2_extend(EFI_TCG2_PROTOCOL *this, EFI_UINT64 flags, EFI_PHYSICAL_ADDRESS data,
                                          EFI_UINT64 len, EFI_TCG2_EVENT *event) {
    (void)this;
    out("K tcg2.extend=%u %x %x %x %u %e\n", (uint64_t)event->Header.PCRIndex, (uint64_t)event->Header.EventType,
        (uint64_t)flags, fnv1a((const uint8_t *)(uintptr_t)data, len), (uint64_t)len, (const char *)event->Event);
    return (faketcg2_flags & 2) ? EFI_DEVICE_ERROR : (faketcg2_flags & 8) ? EFI_VOLUME_FULL : EFI_SUCCESS;
}

static EFI_TCG2_PROTOCOL faketcg2 = { faketcg2_capability, faketcg2_event_log, faketcg2_extend, NULL, NULL, NULL,
                                      NULL };
static bool faketcg2_pending;

// Installed by chain, as the CC protocol is.
static void cmd_faketcg2(char *arg) {
    faketcg2_flags = (uint32_t)parse_hex(arg, NULL);
    faketcg2_pending = true;
}

static void install_faketcg2(void) {
    EFI_GUID g = EFI_TCG2_PROTOCOL_GUID;
    EFI_HANDLE h = NULL;
    out("K faketcg2=%x\n", (uint64_t)bs->InstallProtocolInterface(&h, &g, EFI_NATIVE_INTERFACE, &faketcg2));
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
    EFI_DEVICE_PATH_PROTOCOL *dp = file_devpath(arg);
    EFI_HANDLE h = NULL;
    EFI_STATUS s = bs->LoadImage(false, self, dp, NULL, 0, &h);
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
        EFI_GUID lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
        bs->HandleProtocol(h, &lig, (void **)&li);
        size_t n = slen(opts);
        EFI_CHAR16 *w = alloc((n + 1) * 2);
        for (size_t i = 0; i < n; i++) {
            w[i] = opts[i] == '_' ? ' ' : (uint8_t)opts[i];
        }
        w[n] = 0;
        li->LoadOptions = w;
        li->LoadOptionsSize = (n + 1) * 2;
    }
    EFI_UINTN exit_size = 0;
    EFI_CHAR16 *exit_data = NULL;
    out("B CHAIN-RETURN-PENDING\n");
    s = bs->StartImage(h, &exit_size, &exit_data);
    out("\nK chain.status=%x\n", (uint64_t)s);
    out("K chain.exit_data_size=%u\n", (uint64_t)exit_size);
    out("K chain.exit_data=%w\n", exit_size > 0 ? exit_data : (EFI_CHAR16 *)L"");
}

static EFI_STATUS cmd_exit(char *arg) {
    const char *e;
    EFI_UINTN status = (EFI_UINTN)parse_hex(arg, &e);
    if (sizeof(EFI_UINTN) == 8 && (uint64_t)status >> 31 == 1) {
        // Written as a 32-bit error code: give it a 64-bit status's error bit.
        status = (EFI_UINTN)(((uint64_t)status & 0x7fffffff) | ((uint64_t)1 << 63));
    }
    if (*e == '@') {
        e++;
        size_t n = slen(e);
        EFI_CHAR16 *data;
        bs->AllocatePool(EfiBootServicesData, (n + 1) * 2, (void **)&data);
        for (size_t i = 0; i < n; i++) {
            data[i] = e[i] == '_' ? ' ' : (uint8_t)e[i];
        }
        data[n] = 0;
        out("E DONE\n");
        bs->Exit(self, status, (n + 1) * 2, data);
    }
    out("E DONE\n");
    return status;
}

// A file from the root of this image's volume, in pool memory of the given
// type; NULL if it cannot be read.
static void *read_file(const EFI_CHAR16 *name, EFI_MEMORY_TYPE type, EFI_UINTN *size) {
    EFI_GUID sfsg = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *f = NULL;
    if (loaded->DeviceHandle == NULL
     || bs->HandleProtocol(loaded->DeviceHandle, &sfsg, (void **)&sfs) != EFI_SUCCESS
     || sfs->OpenVolume(sfs, &root) != EFI_SUCCESS) {
        return NULL;
    }
    if (root->Open(root, &f, (EFI_CHAR16 *)name, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        root->Close(root);
        return NULL;
    }
    void *buf = NULL;
    EFI_UINTN cap = 1 << 20;
    if (bs->AllocatePool(type, cap, &buf) == EFI_SUCCESS) {
        *size = cap;
        if (f->Read(f, size, buf) != EFI_SUCCESS) {
            bs->FreePool(buf);
            buf = NULL;
        }
    }
    f->Close(f);
    root->Close(root);
    return buf;
}

// pointers:N reports, for N seconds, each state the simple pointers give.
static void cmd_pointers(char *arg) {
    uint64_t seconds = parse_hex(arg, NULL);
    EFI_GUID g = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    if (bs->LocateHandleBuffer(EfiByProtocol, &g, NULL, &count, &handles) != EFI_SUCCESS) {
        count = 0;
    }
    out("K pointers.count=%u\n", (uint64_t)count);
    EFI_SIMPLE_POINTER_PROTOCOL *p[8];
    size_t n = 0;
    for (EFI_UINTN i = 0; i < count && n < 8; i++) {
        if (bs->HandleProtocol(handles[i], &g, (void **)&p[n]) == EFI_SUCCESS) {
            out("K pointers.%u.resolution=%u %u %u\n", (uint64_t)n, p[n]->Mode->ResolutionX,
                p[n]->Mode->ResolutionY, p[n]->Mode->ResolutionZ);
            p[n]->Reset(p[n], false);
            n++;
        }
    }
    out("B POINTERS-READY\n");
    for (uint64_t ms = 0; ms < seconds * 1000; ms += 10) {
        for (size_t i = 0; i < n; i++) {
            EFI_SIMPLE_POINTER_STATE st;
            if (p[i]->GetState(p[i], &st) == EFI_SUCCESS) {
                out("K pointer.%u=%x %x %x %u %u\n", (uint64_t)i, (uint64_t)(int64_t)st.RelativeMovementX,
                    (uint64_t)(int64_t)st.RelativeMovementY, (uint64_t)(int64_t)st.RelativeMovementZ,
                    (uint64_t)st.LeftButton, (uint64_t)st.RightButton);
            }
        }
        bs->Stall(10000);
    }
}

// uninstall:GUID takes the protocol off every handle it is on.
static void cmd_uninstall(char *arg) {
    EFI_GUID g;
    if (!parse_guid(arg, &g)) {
        out("K error=bad guid %e\n", arg);
        return;
    }
    EFI_UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    if (bs->LocateHandleBuffer(EfiByProtocol, &g, NULL, &count, &handles) != EFI_SUCCESS) {
        out("K uninstall.count=0\n");
        return;
    }
    for (EFI_UINTN i = 0; i < count; i++) {
        void *iface = NULL;
        bs->HandleProtocol(handles[i], &g, &iface);
        EFI_STATUS s = bs->UninstallProtocolInterface(handles[i], &g, iface);
        out("K uninstall.%u=%x\n", (uint64_t)i, (uint64_t)s);
    }
    out("K uninstall.count=%u\n", (uint64_t)count);
    bs->FreePool(handles);
}

// dtb:NAME installs the device tree in that file, at the root of this image's
// volume, as the firmware's device tree configuration table.
static void cmd_dtb(char *arg) {
    EFI_CHAR16 name[256];
    to_utf16(name, arg, 256);
    EFI_UINTN size = 0;
    void *blob = read_file(name, EfiACPIReclaimMemory, &size);
    if (blob == NULL) {
        out("K dtb=unreadable\n");
        return;
    }
    EFI_GUID g = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
    out("K dtb=%x\n", (uint64_t)bs->InstallConfigurationTable(&g, blob));
}

#if defined (__x86_64__) || defined (__i386__)
static const uint8_t *acpi_table(const char *sig) {
    EFI_GUID acpi20 = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
    const uint8_t *rsdp = NULL;
    for (EFI_UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (memcmp(&st->ConfigurationTable[i].VendorGuid, &acpi20, sizeof(acpi20)) == 0) {
            rsdp = st->ConfigurationTable[i].VendorTable;
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
    EFI_PHYSICAL_ADDRESS tables = 0xffffffff;
    if (dmar == NULL || bs->AllocatePages(EfiAllocateMaxAddress, EfiReservedMemoryType, 3, &tables) != EFI_SUCCESS) {
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
    EFI_PHYSICAL_ADDRESS tables = 0xffffffff;
    if (ivrs == NULL || bs->AllocatePages(EfiAllocateMaxAddress, EfiReservedMemoryType, 514, &tables) != EFI_SUCCESS) {
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
#endif

#if !defined (__x86_64__) && !defined (__i386__)
// SetVirtualAddressMap() replaced by code that returns EFI_UNSUPPORTED, in
// runtime services memory so that it outlives boot services.
static void cmd_svamfail(void) {
#if defined (__aarch64__)
    static const uint32_t code[] = { 0xd2800060, 0xf2f00000, 0xd65f03c0 };
#elif defined (__riscv)
    static const uint32_t code[] = { 0xfff00513, 0x03f51513, 0x00350513, 0x00008067 };
#elif defined (__loongarch64)
    static const uint32_t code[] = { 0x03200004, 0x03800c84, 0x4c000020 };
#endif
    EFI_PHYSICAL_ADDRESS page = 0;
    EFI_STATUS s = bs->AllocatePages(EfiAllocateAnyPages, EfiRuntimeServicesCode, 1, &page);
    if (s != EFI_SUCCESS) {
        out("K svamfail=%x\n", (uint64_t)s);
        return;
    }
    uint32_t *dst = (uint32_t *)(uintptr_t)page;
    for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
        dst[i] = code[i];
    }
#if defined (__aarch64__)
    asm volatile ("dc cvau, %0\n\tdsb ish\n\tic ivau, %0\n\tdsb ish\n\tisb" :: "r"(dst) : "memory");
#elif defined (__riscv)
    asm volatile ("fence.i" ::: "memory");
#elif defined (__loongarch64)
    asm volatile ("ibar 0" ::: "memory");
#endif
    rt->SetVirtualAddressMap = (EFI_SET_VIRTUAL_ADDRESS_MAP)(uintptr_t)dst;
    rt->Hdr.CRC32 = 0;
    EFI_UINT32 crc = 0;
    bs->CalculateCrc32(rt, rt->Hdr.HeaderSize, &crc);
    rt->Hdr.CRC32 = crc;
    out("K svamfail=0\n");
}
#endif

static EFI_STATUS EFI_API failing_watchdog(EFI_UINTN timeout, EFI_UINT64 code, EFI_UINTN size,
                                           EFI_CHAR16 *data) {
    (void)timeout;
    (void)code;
    (void)size;
    (void)data;
    return EFI_DEVICE_ERROR;
}

// SetWatchdogTimer() replaced by one that always fails; the probe stays
// loaded while the image it chains to runs.
static void cmd_wdfail(void) {
    bs->SetWatchdogTimer = failing_watchdog;
    bs->Hdr.CRC32 = 0;
    EFI_UINT32 crc = 0;
    bs->CalculateCrc32(bs, bs->Hdr.HeaderSize, &crc);
    bs->Hdr.CRC32 = crc;
    out("K wdfail=0\n");
}

static EFI_EXIT_BOOT_SERVICES real_exit_boot_services;
static uint64_t ebs_failures, ebs_fragment_pages;

static EFI_STATUS EFI_API failing_exit_boot_services(EFI_HANDLE image, EFI_UINTN key) {
    if (ebs_failures == 0) {
        return real_exit_boot_services(image, key);
    }
    ebs_failures--;
    for (; ebs_fragment_pages > 0; ebs_fragment_pages--) {
        EFI_PHYSICAL_ADDRESS page = 0;
        bs->AllocatePages(EfiAllocateAnyPages, ebs_fragment_pages % 2 ? EfiLoaderData : EfiBootServicesData,
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
    real_exit_boot_services = bs->ExitBootServices;
    bs->ExitBootServices = failing_exit_boot_services;
    bs->Hdr.CRC32 = 0;
    EFI_UINT32 crc = 0;
    bs->CalculateCrc32(bs, bs->Hdr.HeaderSize, &crc);
    bs->Hdr.CRC32 = crc;
    out("K ebsfail=0\n");
}

// A Linux random seed table of n (hex) bytes, as an earlier stage would
// leave one.
static void cmd_seedtable(char *arg) {
    uint32_t n = (uint32_t)parse_hex(arg, NULL);
    uint8_t *table = NULL;
    EFI_STATUS s = bs->AllocatePool(EfiACPIReclaimMemory, 4 + n, (void **)&table);
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
    EFI_GUID g = { 0x1ce1e5bc, 0x7ceb, 0x42f2, { 0x81, 0xe5, 0x8a, 0xad, 0xf1, 0x80, 0xf5, 0x7b } };
    out("K seedtable=%x\n", (uint64_t)bs->InstallConfigurationTable(&g, table));
}

// An SMBIOS 3.0 entry point in place of the firmware's, giving a structure
// table at ADDR of SIZE bytes (both hex, as ADDR:SIZE).
static void cmd_smbios3(char *arg) {
    const char *rest;
    uint64_t addr = parse_hex(arg, &rest);
    uint32_t size = *rest == ':' ? (uint32_t)parse_hex(rest + 1, NULL) : 0;
    uint8_t *ep = NULL;
    EFI_STATUS s = bs->AllocatePool(EfiRuntimeServicesData, 24, (void **)&ep);
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
    EFI_GUID g = EFI_SMBIOS3_TABLE_GUID;
    out("K smbios3=%x\n", (uint64_t)bs->InstallConfigurationTable(&g, ep));
}

#if defined (__riscv)
// Translation off, as some RISC-V firmware runs: the firmware's identity map
// leaves every address the same without it.
static void cmd_satpoff(void) {
    asm volatile ("csrw satp, zero\n\tsfence.vma" ::: "memory");
    out("K satpoff=0\n");
}
#endif

static char cmdbuf[4096];

static size_t read_cmd_file(void) {
    EFI_GUID sfsg = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    if (loaded->DeviceHandle == NULL
     || bs->HandleProtocol(loaded->DeviceHandle, &sfsg, (void **)&sfs) != EFI_SUCCESS) {
        return 0;
    }
    EFI_FILE_PROTOCOL *root = NULL, *f = NULL;
    if (sfs->OpenVolume(sfs, &root) != EFI_SUCCESS) {
        return 0;
    }
    if (root->Open(root, &f, L"efiprobe.cmd", EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        return 0;
    }
    EFI_UINTN size = sizeof(cmdbuf) - 1;
    f->Read(f, &size, cmdbuf);
    f->Close(f);
    root->Close(root);
    cmdbuf[size] = 0;
    return size;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    st = system_table;
    bs = st->BootServices;
    rt = st->RuntimeServices;
    self = image;
    EFI_GUID lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    bs->HandleProtocol(image, &lig, (void **)&loaded);
#if !defined (__x86_64__) && !defined (__i386__)
    EFI_GUID sg = EFI_SERIAL_IO_PROTOCOL_GUID;
    if (bs->LocateProtocol(&sg, NULL, (void **)&serial) != EFI_SUCCESS) {
        serial = NULL;
    }
#endif

    size_t n = 0;
    if (loaded->LoadOptions != NULL && loaded->LoadOptionsSize >= 2) {
        EFI_CHAR16 *o = loaded->LoadOptions;
        for (size_t i = 0; i < loaded->LoadOptionsSize / 2 && o[i] != 0 && n + 1 < sizeof(cmdbuf); i++) {
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
#if defined (__riscv)
        } else if (sprefix(tok, "satpoff")) {
            cmd_satpoff();
#endif
#if defined (__x86_64__) || defined (__i386__)
        } else if (sprefix(tok, "msr:")) {
            cmd_msr(tok + 4);
        } else if (sprefix(tok, "vtdon")) {
            cmd_vtdon();
        } else if (sprefix(tok, "amdvion")) {
            cmd_amdvion();
#else
        } else if (sprefix(tok, "svamfail")) {
            cmd_svamfail();
#endif
        } else if (sprefix(tok, "chain:")) {
            cmd_chain(tok + 6);
        } else if (sprefix(tok, "stall:")) {
            bs->Stall((EFI_UINTN)parse_hex(tok + 6, NULL) * 1000);
        } else if (sprefix(tok, "exit:")) {
            return cmd_exit(tok + 5);
        } else if (sprefix(tok, "reset:")) {
            out("E DONE\n");
            EFI_RESET_TYPE t = EfiResetWarm;
            if (sprefix(tok + 6, "cold")) {
                t = EfiResetCold;
            } else if (sprefix(tok + 6, "shutdown")) {
                t = EfiResetShutdown;
            }
            rt->ResetSystem(t, EFI_SUCCESS, 0, NULL);
        } else if (sprefix(tok, "hang")) {
            out("E DONE\n");
            for (;;) {
                bs->Stall(1000000);
            }
        } else {
            out("K error=unknown command %e\n", tok);
        }
    }
    out("E DONE\n");
    return EFI_SUCCESS;
}
