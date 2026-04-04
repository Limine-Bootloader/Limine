#include <stddef.h>
#include <stdbool.h>
#include <lib/acpi.h>
#include <lib/config.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/getchar.h>
#include <mm/pmm.h>
#include <fs/file.h>
#include <lib/print.h>
#include <pxe/tftp.h>
#include <crypt/blake2b.h>
#include <sys/cpu.h>

#define CONFIG_B2SUM_SIGNATURE "++CONFIG_B2SUM_SIGNATURE++"
#define CONFIG_B2SUM_EMPTY "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

const char *config_b2sum = CONFIG_B2SUM_SIGNATURE CONFIG_B2SUM_EMPTY;

static bool config_get_entry_name(char *ret, size_t index, size_t limit);
static char *config_get_entry(size_t *size, size_t index);

#define SEPARATOR '\n'

bool config_ready = false;
no_unwind bool bad_config = false;

static char *config_addr;

#if defined (UEFI)

#define EFI_APP_PATH_LEN 128
static char efi_app_path[128] = {0};

static bool init_efi_app_path(size_t *len_out) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_DEVICE_PATH_PROTOCOL *path;
    CHAR16 *file_path, *p, *last_slash;

    EFI_GUID loaded_image_protocol_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    status = gBS->HandleProtocol(efi_image_handle, &loaded_image_protocol_guid,
                                 (void **)&loaded_image);
    if (status != 0) {
        return false;
    }

    path = loaded_image->FilePath;

    while (!(path->Type == END_DEVICE_PATH_TYPE && path->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
        if (path->Type == MEDIA_DEVICE_PATH && path->SubType == MEDIA_FILEPATH_DP) {
            goto found;
        }

        uint16_t node_length = *((uint16_t *)&path->Length[0]);
        if (node_length < 4) {
            return false;
        }
        path = (void *)path + node_length;
    }

    return false;

found:
    file_path = (CHAR16 *)((void *)path + 4);

    last_slash = NULL;
    for (p = file_path; *p; p++) {
        if (*p == L'\\') {
            last_slash = p;
        }
    }

    if (last_slash) {
        size_t len = (last_slash - file_path) + 1;
        if (len >= EFI_APP_PATH_LEN) {
            len = EFI_APP_PATH_LEN - 1;
        }

        for (size_t i = 0; i < len; i++) {
            efi_app_path[i] = (char)(file_path[i] & 0xff);
            if (efi_app_path[i] == '\\') {
                efi_app_path[i] = '/';
            }
        }
        efi_app_path[len] = 0;
        if (len_out != NULL) {
            *len_out = len;
        }
    } else {
        efi_app_path[0] = '/';
        efi_app_path[1] = 0;
        if (len_out != NULL) {
            *len_out = 1;
        }
    }

    return true;
}
#endif

int init_config_disk(struct volume *part) {
#if defined (UEFI)
    bool use_default_efi_search_path = false;

    size_t len;
    if (!init_efi_app_path(&len)) {
        use_default_efi_search_path = true;
    } else {
        if (len + sizeof("limine.conf") >= EFI_APP_PATH_LEN) {
            use_default_efi_search_path = true;
        } else {
            strcpy(efi_app_path + len, "limine.conf");
        }
    }
#endif

    struct file_handle *f;

    bool old_cif = case_insensitive_fopen;
    case_insensitive_fopen = true;
    if (
     false
#if defined (UEFI)
     || (f = fopen(part, use_default_efi_search_path ? "/EFI/BOOT/limine.conf" : efi_app_path)) != NULL
#endif
     || (f = fopen(part, "/boot/limine/limine.conf")) != NULL
     || (f = fopen(part, "/boot/limine.conf")) != NULL
     || (f = fopen(part, "/limine/limine.conf")) != NULL
     || (f = fopen(part, "/limine.conf")) != NULL
    ) {
        goto opened;
    }

    case_insensitive_fopen = old_cif;
    return -1;

opened:
    case_insensitive_fopen = old_cif;

    if (f->size > SIZE_MAX - 2) {
        panic(false, "Config file too large");
    }
    size_t config_size = f->size + 2;
    config_addr = ext_mem_alloc(config_size);

    fread(f, config_addr, 0, f->size);

    fclose(f);

    return init_config(config_size);
}

struct smbios_struct_header {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
} __attribute__((packed));

static size_t smbios_struct_size(struct smbios_struct_header *hdr, size_t remaining) {
    // Validate minimum structure header size
    if (remaining < sizeof(struct smbios_struct_header)) {
        return 0;
    }
    if (hdr->length < sizeof(struct smbios_struct_header)) {
        return 0;  // Invalid structure
    }
    if (hdr->length > remaining) {
        return 0;  // Structure header claims more than remaining
    }

    const char *string_data = (void *)((uintptr_t)hdr + hdr->length);
    size_t string_area_max = remaining - hdr->length;
    size_t i = 1;
    for (; i < string_area_max && (string_data[i - 1] != '\0' || string_data[i] != '\0'); i++);
    if (i >= string_area_max) {
        return 0;  // Unterminated string area
    }
    return hdr->length + i + 1;
}

bool init_config_smbios(void) {
    struct smbios_entry_point_32 *smbios_entry_32 = NULL;
    struct smbios_entry_point_64 *smbios_entry_64 = NULL;
    acpi_get_smbios((void **)&smbios_entry_32, (void **)&smbios_entry_64);
    if (smbios_entry_32 == NULL && smbios_entry_64 == NULL) {
        return false;
    }

    struct smbios_struct_header *hdr = NULL;
    size_t struct_count = 0;
    size_t table_length = 0;

    if (smbios_entry_64) {
        hdr = (void *)(uintptr_t) smbios_entry_64->table_address;
        table_length = smbios_entry_64->table_maximum_size;
    } else {
        hdr = (void *)(uintptr_t) smbios_entry_32->table_address;
        struct_count = smbios_entry_32->number_of_structures;
        table_length = smbios_entry_32->table_length;
    }

    if (hdr == NULL || table_length == 0) {
        return false;
    }

    size_t structure_bytes_processed = 0;
    for (size_t struct_num = 0; hdr && (!struct_count || struct_num < struct_count); struct_num++) {
        size_t remaining = table_length - structure_bytes_processed;
        if (remaining < sizeof(struct smbios_struct_header)) {
            return false;
        }

        if (hdr->type == 127)
            return false;

        size_t struct_size = smbios_struct_size(hdr, remaining);
        if (struct_size == 0) {
            return false;  // Invalid structure
        }

        if (hdr->type == 11 && hdr->length >= sizeof(struct smbios_struct_header)) {
            const char *string_data = (void *)((uintptr_t) hdr + hdr->length);
            size_t string_area_size = struct_size - hdr->length;

            size_t prefix_len = sizeof("limine:config:") - 1;
            if (string_area_size > prefix_len && !strncmp(string_data, "limine:config:", prefix_len)) {
                size_t total_len = strnlen(string_data, string_area_size);
                if (total_len <= prefix_len)
                    continue;
                size_t config_size = total_len - prefix_len + 2;
                config_addr = ext_mem_alloc(config_size);
                memcpy(config_addr, &string_data[prefix_len], config_size - 1);
                config_addr[config_size - 1] = '\0';
                return !init_config(config_size);
            }
        }

        structure_bytes_processed += struct_size;
        if (structure_bytes_processed >= table_length) {
            return false;
        }

        hdr = (void *)((uintptr_t) hdr + struct_size);
    }

    return false;
}

#define NOT_CHILD      (-1)
#define DIRECT_CHILD   0
#define INDIRECT_CHILD 1

static int is_child(char *buf, size_t limit,
                    size_t current_depth, size_t index) {
    if (!config_get_entry_name(buf, index, limit))
        return NOT_CHILD;
    if (strlen(buf) < current_depth + 1)
        return NOT_CHILD;
    for (size_t j = 0; j < current_depth; j++)
        if (buf[j] != '/')
            return NOT_CHILD;
    if (buf[current_depth] == '/')
        return INDIRECT_CHILD;
    return DIRECT_CHILD;
}

static bool is_directory(char *buf, size_t limit,
                         size_t current_depth, size_t index) {
    switch (is_child(buf, limit, current_depth + 1, index + 1)) {
        default:
        case NOT_CHILD:
            return false;
        case INDIRECT_CHILD:
            bad_config = true;
            panic(true, "config: Malformed config file. Parentless child.");
        case DIRECT_CHILD:
            return true;
    }
}

static struct menu_entry *create_menu_tree(struct menu_entry *parent,
                                           size_t current_depth, size_t index) {
    struct menu_entry *root = NULL, *prev = NULL;

    for (size_t i = index; ; i++) {
        static char name[64];

        switch (is_child(name, 64, current_depth, i)) {
            case NOT_CHILD:
                return root;
            case INDIRECT_CHILD:
                continue;
            case DIRECT_CHILD:
                break;
        }

        struct menu_entry *entry = ext_mem_alloc(sizeof(struct menu_entry));

        if (root == NULL)
            root = entry;

        config_get_entry_name(name, i, 64);

        bool default_expanded = name[current_depth] == '+';

        char *n = &name[current_depth + default_expanded];
        while (*n == ' ') {
            n++;
        }

        size_t n_len = strlen(n);
        if (n_len >= sizeof(entry->name)) {
            n_len = sizeof(entry->name) - 1;
        }
        memcpy(entry->name, n, n_len);
        entry->name[n_len] = 0;
        entry->parent = parent;

        size_t entry_size;
        char *config_entry = config_get_entry(&entry_size, i);
        entry->body = ext_mem_alloc(entry_size + 1);
        memcpy(entry->body, config_entry, entry_size);
        entry->body[entry_size] = 0;

        if (is_directory(name, 64, current_depth, i)) {
            entry->sub = create_menu_tree(entry, current_depth + 1, i + 1);
            entry->expanded = default_expanded;
        }

        char *comment = config_get_value(entry->body, 0, "COMMENT");
        if (comment != NULL) {
            entry->comment = strdup(comment);
        }

        if (prev != NULL)
            prev->next = entry;
        prev = entry;
    }
}

struct menu_entry *menu_tree = NULL;

struct macro {
    char name[1024];
    char value[2048];
    struct macro *next;
};

static struct macro *macros = NULL;

int init_config(size_t config_size) {
    config_b2sum += sizeof(CONFIG_B2SUM_SIGNATURE) - 1;

    if (memcmp((void *)config_b2sum, CONFIG_B2SUM_EMPTY, 128) != 0) {
        editor_enabled = false;

        uint8_t out_buf[BLAKE2B_OUT_BYTES];
        blake2b(out_buf, config_addr, config_size - 2);
        uint8_t hash_buf[BLAKE2B_OUT_BYTES];

        for (size_t i = 0; i < BLAKE2B_OUT_BYTES; i++) {
            int hi = digit_to_int(config_b2sum[i * 2]);
            int lo = digit_to_int(config_b2sum[i * 2 + 1]);
            if (hi == -1 || lo == -1) {
                panic(false, "!!! INVALID CHARACTER IN CONFIG CHECKSUM !!!");
            }
            hash_buf[i] = hi << 4 | lo;
        }

        if (memcmp(hash_buf, out_buf, BLAKE2B_OUT_BYTES) != 0) {
            panic(false, "!!! CHECKSUM MISMATCH FOR CONFIG FILE !!!");
        }
    }

    // add trailing newline if not present
    config_addr[config_size - 2] = '\n';

    size_t config_alloc_size = config_size;

    // remove windows carriage returns and spaces at the start and end of lines, if any
    for (size_t i = 0; i < config_size; i++) {
        size_t skip = 0;
        if (config_addr[i] == ' ' || config_addr[i] == '\t') {
            while (i + skip < config_size && (config_addr[i + skip] == ' ' || config_addr[i + skip] == '\t')) {
                skip++;
            }
            if (i + skip < config_size && config_addr[i + skip] == '\n') {
                goto skip_loop;
            }
            skip = 0;
        }
        while (i + skip < config_size
            && ((config_addr[i + skip] == '\r')
                || ((!i || config_addr[i - 1] == '\n') && (config_addr[i + skip] == ' ' || config_addr[i + skip] == '\t')))
        ) {
            skip++;
        }
skip_loop:
        if (skip) {
            for (size_t j = i; j < config_size - skip; j++)
                config_addr[j] = config_addr[j + skip];
            config_size -= skip;
        }
    }

    // Load macros
    struct macro *arch_macro = ext_mem_alloc(sizeof(struct macro));
    strcpy(arch_macro->name, "ARCH");
#if defined (__x86_64__)
    strcpy(arch_macro->value, "x86-64");
#elif defined (__i386__)
    {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 29))) {
        strcpy(arch_macro->value, "ia-32");
    } else {
        strcpy(arch_macro->value, "x86-64");
    }
    }
#elif defined (__aarch64__)
    strcpy(arch_macro->value, "aarch64");
#elif defined (__riscv)
    strcpy(arch_macro->value, "riscv64");
#elif defined (__loongarch64)
    strcpy(arch_macro->value, "loongarch64");
#else
#error "Unspecified architecture"
#endif
    arch_macro->next = macros;
    macros = arch_macro;

    struct macro *fw_type_macro = ext_mem_alloc(sizeof(struct macro));
    strcpy(fw_type_macro->name, "FW_TYPE");
#if defined (UEFI)
    strcpy(fw_type_macro->value, "UEFI");
#else
    strcpy(fw_type_macro->value, "BIOS");
#endif
    fw_type_macro->next = macros;
    macros = fw_type_macro;

    for (size_t i = 0; i < config_size;) {
        if ((config_size - i >= 3 && memcmp(config_addr + i, "\n${", 3) == 0)
         || (config_size - i >= 2 && i == 0 && memcmp(config_addr, "${", 2) == 0)) {
            struct macro *macro = ext_mem_alloc(sizeof(struct macro));

            i += i ? 3 : 2;
            size_t j;
            for (j = 0; config_addr[i] != '}' && config_addr[i] != '\n' && config_addr[i] != 0; j++, i++) {
                if (j >= sizeof(macro->name) - 1) {
                    bad_config = true;
                    panic(true, "config: Macro name too long (max %U)", (uint64_t)(sizeof(macro->name) - 1));
                }
                macro->name[j] = config_addr[i];
            }

            if (config_addr[i] == '\n' || config_addr[i] == 0 || config_addr[i+1] != '=') {
                pmm_free(macro, sizeof(struct macro));
                continue;
            }
            i += 2;

            macro->name[j] = 0;

            for (j = 0; config_addr[i] != '\n' && config_addr[i] != 0; j++, i++) {
                if (j >= sizeof(macro->value) - 1) {
                    bad_config = true;
                    panic(true, "config: Macro value too long (max %U)", (uint64_t)(sizeof(macro->value) - 1));
                }
                macro->value[j] = config_addr[i];
            }
            macro->value[j] = 0;

            macro->next = macros;
            macros = macro;

            continue;
        }

        i++;
    }

    // Expand macros
    if (macros != NULL) {
        // Check for overflow before multiplication
        if (config_size > SIZE_MAX / 4) {
            bad_config = true;
            panic(true, "config: Config file too large for macro expansion");
        }
        size_t new_config_size = config_size * 4;
        char *new_config = ext_mem_alloc(new_config_size);

        size_t i, in;
        for (i = 0, in = 0; i < config_size;) {
            if ((config_size - i >= 3 && memcmp(config_addr + i, "\n${", 3) == 0)
             || (config_size - i >= 2 && i == 0 && memcmp(config_addr, "${", 2) == 0)) {
                size_t orig_i = i;
                i += i ? 3 : 2;
                while (i < config_size && config_addr[i] != '}') {
                    i++;
                }
                if (i >= config_size) {
                    bad_config = true;
                    panic(true, "config: Malformed macro usage");
                }
                i++; // skip '}'
                if (i >= config_size || config_addr[i++] != '=') {
                    i = orig_i;
                    goto next;
                }
                while (config_addr[i] != '\n' && config_addr[i] != 0) {
                    i++;
                    if (i >= config_size) {
                        bad_config = true;
                        panic(true, "config: Malformed macro usage");
                    }
                }
                continue;
            }

next:
            if (config_size - i >= 2 && memcmp(config_addr + i, "${", 2) == 0) {
                char *macro_name = ext_mem_alloc(1024);
                i += 2;
                size_t j;
                for (j = 0; j < 1023 && config_addr[i] != '}' && config_addr[i] != '\n' && config_addr[i] != 0; j++, i++) {
                    macro_name[j] = config_addr[i];
                }
                if (config_addr[i] != '}') {
                    bad_config = true;
                    panic(true, "config: Malformed macro usage");
                }
                i++;
                macro_name[j] = 0;
                char *macro_value = "";
                struct macro *macro = macros;
                for (;;) {
                    if (macro == NULL) {
                        break;
                    }
                    if (strcmp(macro->name, macro_name) == 0) {
                        macro_value = macro->value;
                        break;
                    }
                    macro = macro->next;
                }
                pmm_free(macro_name, 1024);
                for (j = 0; macro_value[j] != 0; j++, in++) {
                    if (in >= new_config_size) {
                        goto overflow;
                    }
                    new_config[in] = macro_value[j];
                }
                continue;
            }

            if (in >= new_config_size) {
overflow:
                bad_config = true;
                panic(true, "config: Macro-induced buffer overflow");
            }
            new_config[in++] = config_addr[i++];
        }

        pmm_free(config_addr, config_alloc_size);

        config_addr = new_config;
        config_size = in;

        // Free macros
        struct macro *macro = macros;
        for (;;) {
            if (macro == NULL) {
                break;
            }
            struct macro *next = macro->next;
            pmm_free(macro, sizeof(struct macro));
            macro = next;
        }
        macros = NULL;
    }

    config_ready = true;

    menu_tree = create_menu_tree(NULL, 1, 0);

    size_t s;
    char *c = config_get_entry(&s, 0);
    if (c != NULL) {
        while (*c != '/' && c > config_addr) {
            c--;
        }
        if (*c == '/' && c > config_addr) {
            c[-1] = 0;
        }
    }

    return 0;
}

static bool config_get_entry_name(char *ret, size_t index, size_t limit) {
    if (!config_ready)
        return false;

    char *p = config_addr;

    for (size_t i = 0; i <= index; i++) {
        while (*p != '/') {
            if (!*p)
                return false;
            p++;
        }
        p++;
        if ((p - 1) != config_addr && *(p - 2) != '\n')
            i--;
    }

    p--;

    size_t i;
    for (i = 0; i < (limit - 1); i++) {
        if (p[i] == SEPARATOR)
            break;
        ret[i] = p[i];
    }

    ret[i] = 0;
    return true;
}

static char *config_get_entry(size_t *size, size_t index) {
    if (!config_ready)
        return NULL;

    char *ret;
    char *p = config_addr;

    for (size_t i = 0; i <= index; i++) {
        while (*p != '/') {
            if (!*p)
                return NULL;
            p++;
        }
        p++;
        if ((p - 1) != config_addr && *(p - 2) != '\n')
            i--;
    }

    do {
        p++;
    } while (*p != '\n' && *p != '\0');

    ret = p;

cont:
    while (*p != '/' && *p)
        p++;

    if (*p && *(p - 1) != '\n') {
        p++;
        goto cont;
    }

    *size = p - ret;

    return ret;
}

static const char *lastkey;

struct conf_tuple config_get_tuple(const char *config, size_t index,
                                   const char *key1, const char *key2) {
    // Static buffers for return values.
    // Callers must copy the result if they need persistence across calls.
    #define CONF_TUPLE_BUF_SIZE 4096
    static char value1_buf[CONF_TUPLE_BUF_SIZE];
    static char value2_buf[CONF_TUPLE_BUF_SIZE];

    struct conf_tuple conf_tuple;

    char *tmp = config_get_value(config, index, key1);
    if (tmp == NULL) {
        return (struct conf_tuple){0};
    }
    size_t len = strlen(tmp);
    if (len >= CONF_TUPLE_BUF_SIZE) {
        len = CONF_TUPLE_BUF_SIZE - 1;
    }
    memcpy(value1_buf, tmp, len);
    value1_buf[len] = '\0';
    conf_tuple.value1 = value1_buf;

    const char *lk1 = lastkey;

    tmp = config_get_value(lk1, 0, key2);
    if (tmp != NULL) {
        len = strlen(tmp);
        if (len >= CONF_TUPLE_BUF_SIZE) {
            len = CONF_TUPLE_BUF_SIZE - 1;
        }
        memcpy(value2_buf, tmp, len);
        value2_buf[len] = '\0';
        conf_tuple.value2 = value2_buf;
    } else {
        conf_tuple.value2 = NULL;
    }

    const char *lk2 = lastkey;

    const char *next_value1 = config_get_value(config, index + 1, key1);

    const char *lk3 = lastkey;

    if (conf_tuple.value2 != NULL && next_value1 != NULL) {
        if ((uintptr_t)lk2 > (uintptr_t)lk3) {
            conf_tuple.value2 = NULL;
        }
    }

    return conf_tuple;
}

char *config_get_value(const char *config, size_t index, const char *key) {
    // Static buffer for return values.
    // Callers must copy the result if they need persistence across calls.
    #define CONFIG_VALUE_BUF_SIZE 4096
    static char buf[CONFIG_VALUE_BUF_SIZE];

    if (!key || !config_ready)
        return NULL;

    if (config == NULL)
        config = config_addr;

    size_t key_len = strlen(key);

    for (size_t i = 0; config[i]; i++) {
        if (!strncasecmp(&config[i], key, key_len) && config[i + key_len] == ':') {
            if (i && config[i - 1] != SEPARATOR)
                continue;
            if (index--)
                continue;
            i += key_len + 1;
            while (config[i] == ' ' || config[i] == '\t') {
                i++;
            }
            size_t value_len;
            for (value_len = 0;
                 config[i + value_len] != SEPARATOR && config[i + value_len];
                 value_len++);
            if (value_len >= CONFIG_VALUE_BUF_SIZE) {
                value_len = CONFIG_VALUE_BUF_SIZE - 1;
            }
            memcpy(buf, config + i, value_len);
            buf[value_len] = '\0';
            lastkey = config + i;
            return buf;
        }
    }

    return NULL;
}
