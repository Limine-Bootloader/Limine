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
#include <crypt/hash.h>
#include <lib/tpm.h>
#include <lib/utf.h>
#include <sys/cpu.h>

#define CONFIG_B2SUM_SIGNATURE "++CONFIG_B2SUM_SIGNATURE++"
#define CONFIG_B2SUM_EMPTY \
    "0000000000000000000000000000000000000000000000000000000000000000" \
    "0000000000000000000000000000000000000000000000000000000000000000" \
    "0000000000000000000000000000000000000000000000000000000000000000" \
    "0000000000000000000000000000000000000000000000000000000000000000"

const char *config_b2sum = CONFIG_B2SUM_SIGNATURE CONFIG_B2SUM_EMPTY;

#define SEPARATOR '\n'

bool config_ready = false;
no_unwind bool bad_config = false;

static char *config_addr;

#if defined (UEFI)
// Snapshot of the on-disk config bytes, kept across the in-place mutations
// in init_config so the menu can measure them once measured_boot is known.
static char *config_raw_addr;
static size_t config_raw_size;

const char *config_get_raw(size_t *size_out) {
    *size_out = config_raw_size;
    return config_raw_addr;
}
#endif

#if defined (UEFI)

#define EFI_APP_PATH_LEN 128
static char efi_app_path[128] = {0};

static bool init_efi_app_path(size_t *len_out) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_DEVICE_PATH_PROTOCOL *path;
    EFI_CHAR16 path_units[EFI_APP_PATH_LEN];
    size_t len = 0, dir_len = 0;

    EFI_GUID loaded_image_protocol_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    status = gBS->HandleProtocol(efi_image_handle, &loaded_image_protocol_guid,
                                 (void **)&loaded_image);
    if (status != 0) {
        return false;
    }

    // NULL for an image loaded from a buffer without a device path.
    path = loaded_image->FilePath;
    if (path == NULL) {
        return false;
    }

    while (!(path->Type == EFI_END_DEVICE_PATH_TYPE && path->SubType == EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
        if (path->Type == EFI_MEDIA_DEVICE_PATH && path->SubType == EFI_MEDIA_FILEPATH_DP) {
            goto found;
        }

        uint16_t node_length = EfiDevicePathNodeLength(path);
        if (node_length < 4) {
            return false;
        }
        path = (void *)path + node_length;
    }

    return false;

found:
    // A file path may span several consecutive EFI_MEDIA_FILEPATH_DP nodes;
    // walk them all, bounding each scan by the node length, and keep the
    // directory.
    while (path->Type == EFI_MEDIA_DEVICE_PATH && path->SubType == EFI_MEDIA_FILEPATH_DP) {
        uint16_t node_length = EfiDevicePathNodeLength(path);
        if (node_length < 4) {
            return false;
        }

        // Nodes may start at any byte boundary (UEFI 2.11 section 10.3.1).
        uint8_t *node_path = (uint8_t *)path + 4;
        size_t node_chars = (node_length - 4) / 2;

        // A node boundary separates path components (UEFI 2.11 section
        // 10.3.5.4).
        bool separator = len != 0;

        for (size_t i = 0; i < node_chars; i++) {
            EFI_CHAR16 c = node_path[i * 2] | (node_path[i * 2 + 1] << 8);
            if (c == 0) {
                break;
            }
            if (c == L'\\' || c == L'/') {
                separator = true;
                continue;
            }
            if (len + separator >= EFI_APP_PATH_LEN - 1) {
                // A directory cut short would be another directory, so only
                // the file name may be.
                if (separator) {
                    return false;
                }
                continue;
            }
            if (separator) {
                path_units[len++] = L'/';
                dir_len = len;
                separator = false;
            }
            path_units[len++] = c;
        }

        path = (void *)path + node_length;
    }

    if (dir_len == 0) {
        efi_app_path[0] = '/';
        efi_app_path[1] = 0;
        dir_len = 1;
    } else if (!utf16_to_utf8(efi_app_path, EFI_APP_PATH_LEN, path_units, dir_len, &dir_len)) {
        return false;
    }

    if (len_out != NULL) {
        *len_out = dir_len;
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

        // SMBIOS 3.9 section 7.49 has End-of-Table used like an Inactive
        // structure, the whole table staying indexable, so stop at it only
        // when there is no structure count to go by.
        if (hdr->type == 127 && struct_count == 0) {
            return false;
        }

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
                if (total_len > prefix_len) {
                    size_t config_size = total_len - prefix_len + 2;
                    config_addr = ext_mem_alloc(config_size);
                    memcpy(config_addr, &string_data[prefix_len], config_size - 1);
                    config_addr[config_size - 1] = '\0';
                    return !init_config(config_size);
                }
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

#define MAX_MENU_NESTING 64

static struct menu_entry *create_menu_tree(void) {
    struct menu_entry *root = NULL;
    struct menu_entry *last_at_depth[MAX_MENU_NESTING + 2] = {0};

    char *p = config_addr;
    if (p == NULL) {
        return NULL;
    }

    while (*p != '\0') {
        if (*p == '/' && (p == config_addr || *(p - 1) == '\n')) {
            break;
        }
        p++;
    }

    if (*p == '\0') {
        return NULL;
    }

    if (p > config_addr) {
        *(p - 1) = '\0';
    }

    while (*p != '\0') {
        size_t depth = 0;
        while (*p == '/') {
            depth++;
            p++;
        }

        if (depth > MAX_MENU_NESTING) {
            bad_config = true;
            panic(true, "config: Menu nesting too deep (max %u)", MAX_MENU_NESTING);
        }

        if (depth > 1 && last_at_depth[depth - 1] == NULL) {
            bad_config = true;
            panic(true, "config: Malformed config file. Parentless child.");
        }

        bool default_expanded = (*p == '+');
        if (default_expanded) {
            p++;
        }

        while (*p == ' ' || *p == '\t') {
            p++;
        }

        char *name = p;
        while (*p != '\n' && *p != '\0') {
            p++;
        }
        if (*p == '\n') {
            *p = '\0';
            p++;
        }

        char *body = "";
        if (*p != '/') {
            body = p;
            while (*p != '\0') {
                if (*p == '/' && *(p - 1) == '\n') {
                    break;
                }
                p++;
            }

            if (p > body && *(p - 1) == '\n') {
                *(p - 1) = '\0';
            }
        }

        struct menu_entry *entry = ext_mem_alloc(sizeof(struct menu_entry));
        entry->name = name;
        entry->body = body;
        entry->parent = last_at_depth[depth - 1];
        entry->expanded = default_expanded;

        if (entry->parent != NULL) {
            if (entry->parent->sub == NULL) {
                entry->parent->sub = entry;
            }
        }

        if (last_at_depth[depth] != NULL && last_at_depth[depth]->parent == entry->parent) {
            last_at_depth[depth]->next = entry;
        } else if (depth == 1 && root == NULL) {
            root = entry;
        }

        last_at_depth[depth] = entry;
        for (size_t d = depth + 1; d <= MAX_MENU_NESTING; d++) {
            last_at_depth[d] = NULL;
        }
    }

    return root;
}

struct menu_entry *menu_tree = NULL;

struct macro {
    char name[1024];
    char value[2048];
    struct macro *next;
};

static struct macro *macros = NULL;

// Returns the size of the config with its macro references expanded, and
// stores the expansion in out unless out is NULL.
static size_t expand_macros(char *out, size_t config_size) {
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
            i++;
            if (i >= config_size || config_addr[i++] != '=') {
                i = orig_i;
                goto next;
            }
            // The config ends in a NUL, which stops this before its end.
            while (config_addr[i] != '\n' && config_addr[i] != 0) {
                i++;
            }
            continue;
        }

next:
        if (config_size - i >= 2 && memcmp(config_addr + i, "${", 2) == 0) {
            char macro_name[1024];
            i += 2;
            size_t j;
            // The config ends in a NUL, which stops this before its end.
            for (j = 0; j < sizeof(macro_name) - 1 && config_addr[i] != '}' && config_addr[i] != '\n' && config_addr[i] != 0; j++, i++) {
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
            size_t value_len = strlen(macro_value);
            if (out != NULL) {
                memcpy(out + in, macro_value, value_len);
            }
            in = CHECKED_ADD(in, value_len, goto too_large);
            continue;
        }

        if (out != NULL) {
            out[in] = config_addr[i];
        }
        in = CHECKED_ADD(in, 1, goto too_large);
        i++;
    }

    return in;

too_large:
    bad_config = true;
    panic(true, "config: Config file too large for macro expansion");
}

static bool has_macro_references(const char *config, size_t config_size) {
    const char *p = config;
    size_t rem = config_size;
    while ((p = memchr(p, '$', rem)) != NULL) {
        rem = config_size - (p - config);
        if (rem >= 2 && p[1] == '{') {
            return true;
        }
        p++;
        rem--;
    }

    return false;
}

int init_config(size_t config_size) {
    config_b2sum += sizeof(CONFIG_B2SUM_SIGNATURE) - 1;

    if (hash_enrolled_empty(config_b2sum)) {
        secure_boot_active = false;
    } else {
        editor_enabled = false;

        struct hash_reference ref;
        uint8_t out_buf[HASH_MAX_BYTES];

        if (!hash_reference_from_enrolled(&ref, config_b2sum)) {
            panic(false, "!!! INVALID CHARACTER IN CONFIG CHECKSUM !!!");
        }

        hash_buffer(ref.type, out_buf, ref.digest_size, config_addr, config_size - 2);

        if (memcmp(ref.digest, out_buf, ref.digest_size) != 0) {
            panic(false, "!!! CHECKSUM MISMATCH FOR CONFIG FILE !!!");
        }
    }

#if defined (UEFI)
    // Snapshot the raw bytes; the menu measures them once measured_boot
    // is final.
    config_raw_size = config_size - 2;
    config_raw_addr = ext_mem_alloc(config_raw_size);
    memcpy(config_raw_addr, config_addr, config_raw_size);
#endif

    // add trailing newline if not present
    config_addr[config_size - 2] = '\n';
    config_addr[config_size - 1] = '\0';

    size_t config_alloc_size = config_size;

    size_t write = 0;
    bool at_line_start = true;
    for (size_t read = 0; read < config_size - 1; read++) {
        char c = config_addr[read];

        if (c == '\r') {
            continue;
        }

        if (at_line_start && (c == ' ' || c == '\t')) {
            continue;
        }

        if (c == '\n') {
            while (write > 0 && (config_addr[write - 1] == ' ' || config_addr[write - 1] == '\t')) {
                write--;
            }
            at_line_start = true;
        } else {
            at_line_start = false;
        }

        config_addr[write++] = c;
    }

    while (write > 0 && (config_addr[write - 1] == ' ' || config_addr[write - 1] == '\t')) {
        write--;
    }
    config_addr[write++] = '\0';
    config_size = write;

    if (has_macro_references(config_addr, config_size)) {
        // Load macros
        struct macro *arch_macro = ext_mem_alloc(sizeof(struct macro));
        strcpy(arch_macro->name, "ARCH");
        strcpy(arch_macro->value, current_arch());
        arch_macro->next = macros;
        macros = arch_macro;

        struct macro *fw_type_macro = ext_mem_alloc(sizeof(struct macro));
        strcpy(fw_type_macro->name, "FW_TYPE");
        strcpy(fw_type_macro->value, current_firmware());
        fw_type_macro->next = macros;
        macros = fw_type_macro;

        struct macro *loader_arch_macro = ext_mem_alloc(sizeof(struct macro));
        strcpy(loader_arch_macro->name, "LOADER_ARCH");
        strcpy(loader_arch_macro->value, loader_arch());
        loader_arch_macro->next = macros;
        macros = loader_arch_macro;

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
        size_t new_config_size = expand_macros(NULL, config_size);
        char *new_config = ext_mem_alloc(new_config_size);
        expand_macros(new_config, config_size);

        pmm_free(config_addr, config_alloc_size);

        config_addr = new_config;
        config_size = new_config_size;

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

    menu_tree = create_menu_tree();

    return 0;
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

    // config_ready does not imply a config file was loaded: the blank entry
    // editor sets it with config_addr still unset.
    if (config == NULL)
        return NULL;

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
