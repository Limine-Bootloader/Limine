#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <config.h>
#include <menu.h>
#include <lib/bli.h>
#include <lib/print.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/config.h>
#if defined (UEFI)
#  include <lib/tpm.h>
#endif
#include <lib/term.h>
#include <lib/gterm.h>
#include <lib/getchar.h>
#include <lib/uri.h>
#include <mm/pmm.h>
#include <drivers/vbe.h>
#include <drivers/vga_textmode.h>
#include <drivers/serial.h>
#include <protos/linux.h>
#include <protos/chainload.h>
#include <protos/multiboot1.h>
#include <protos/multiboot2.h>
#include <protos/efi_boot_entry.h>
#include <protos/limine.h>
#include <sys/cpu.h>
#include <lib/misc.h>

#if defined (UEFI)
EFI_GUID limine_efi_vendor_guid =
    { 0x513ee0d0, 0x6e43, 0xcb05, { 0xb2, 0x72, 0xf1, 0x46, 0xa2, 0xfc, 0xb8, 0x8a } };
#endif

#define EDITOR_MAX_BUFFER_SIZE 4096
#define TOK_KEY 0
#define TOK_EQUALS 1
#define TOK_VALUE 2
#define TOK_BADKEY 3
#define TOK_COMMENT 4

static char interface_help_colour[24] = "\e[38;2;0;170;0m";
static char interface_help_colour_bright[24] = "\e[38;2;85;255;85m";
static char menu_branding_colour[24] = "\e[38;2;0;170;170m";

static char *menu_branding = NULL;

static char *write_uint8_dec(char *p, uint8_t v) {
    if (v >= 100) {
        *p++ = '0' + v / 100;
        *p++ = '0' + (v / 10) % 10;
        *p++ = '0' + v % 10;
    } else if (v >= 10) {
        *p++ = '0' + v / 10;
        *p++ = '0' + v % 10;
    } else {
        *p++ = '0' + v;
    }
    return p;
}

static void format_fg_rgb_escape(char *buf, uint32_t rgb) {
    char *p = buf;
    *p++ = '\e'; *p++ = '['; *p++ = '3'; *p++ = '8'; *p++ = ';';
    *p++ = '2'; *p++ = ';';
    p = write_uint8_dec(p, (rgb >> 16) & 0xff);
    *p++ = ';';
    p = write_uint8_dec(p, (rgb >> 8) & 0xff);
    *p++ = ';';
    p = write_uint8_dec(p, rgb & 0xff);
    *p++ = 'm';
    *p = '\0';
}

static bool parse_rgb_colour_value(const char *str, uint32_t *out) {
    const char *end;
    uint32_t v = strtoui(str, &end, 16);
    if (end == str) {
        return false;
    }
    *out = v & 0xffffff;
    return true;
}

static uint32_t brighten_rgb(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff;
    uint32_t g = (rgb >> 8) & 0xff;
    uint32_t b = rgb & 0xff;
    r = r + 0x55 > 0xff ? 0xff : r + 0x55;
    g = g + 0x55 > 0xff ? 0xff : g + 0x55;
    b = b + 0x55 > 0xff ? 0xff : b + 0x55;
    return (r << 16) | (g << 8) | b;
}

no_unwind bool booting_from_editor = false;
static no_unwind bool booting_from_blank = false;
static no_unwind char saved_orig_entry[EDITOR_MAX_BUFFER_SIZE];
static no_unwind char saved_title[256];

static size_t get_line_offset(size_t *displacement, size_t index, const char *buffer) {
    size_t offset = 0;
    size_t _index = index;
    for (size_t i = 0; buffer[i]; i++) {
        if (!_index--)
            break;
        if (buffer[i] == '\n')
            offset = i + 1;
    }
    if (displacement)
        *displacement = index - offset;
    return offset;
}

static size_t get_line_length(size_t index, const char *buffer) {
    size_t i;
    for (i = index; buffer[i] != '\n' && buffer[i] != 0; i++);
    return i - index;
}

static size_t get_next_line(size_t index, const char *buffer) {
    if (buffer[index] == 0)
        return index;
    size_t displacement;
    get_line_offset(&displacement, index, buffer);
    while (buffer[index] != '\n') {
        if (buffer[index] == 0)
            return index;
        index++;
    }
    index++;
    size_t next_line_length = get_line_length(index, buffer);
    if (displacement > next_line_length)
        displacement = next_line_length;
    return index + displacement;
}

static size_t get_prev_line(size_t index, const char *buffer) {
    size_t offset, displacement, prev_line_offset, prev_line_length;
    offset = get_line_offset(&displacement, index, buffer);
    if (offset) {
        prev_line_offset = get_line_offset(NULL, offset - 1, buffer);
        prev_line_length = get_line_length(prev_line_offset, buffer);
        if (displacement > prev_line_length)
            displacement = prev_line_length;
        return prev_line_offset + displacement;
    }
    return offset;
}

static const char *VALID_KEYS[] = {
    "COMMENT",
    "PROTOCOL",
    "CMDLINE",
    "PATH",
    "KERNEL_CMDLINE",
    "KERNEL_PATH",
    "INITRD_PATH",
    "MODULE_PATH",
    "MODULE_STRING",
    "MODULE_CMDLINE",
    "RESOLUTION",
    "TEXTMODE",
    "KASLR",
    "RANDOMISE_HHDM_BASE",
    "RANDOMIZE_HHDM_BASE",
    "PAGING_MODE",
    "MAX_PAGING_MODE",
    "MIN_PAGING_MODE",
    "DRIVE",
    "PARTITION",
    "MBR_ID",
    "GPT_GUID",
    "GPT_UUID",
    "IMAGE_PATH",
	"DTB_PATH",
    "ENTRY",
    "IF_FW_TYPE",
    "IF_ARCH",
    NULL
};

static bool validation_enabled = true;

static int validate_line(const char *buffer) {
    if (!validation_enabled) return TOK_KEY;
    if (buffer[0] == '#')
        return TOK_COMMENT;
    char keybuf[64];
    size_t i;
    for (i = 0; buffer[i] && i < 64; i++) {
        if (buffer[i] == ':') goto found_equals;
        keybuf[i] = buffer[i];
    }
fail:
    if (i < 64) keybuf[i] = 0;
    if (keybuf[0] == '\n' || (!keybuf[0] && buffer[0] != ':')) return TOK_KEY; // blank line is valid
    return TOK_BADKEY;
found_equals:
    if (i < 64) keybuf[i] = 0;
    for (i = 0; VALID_KEYS[i]; i++) {
        if (!strcasecmp(keybuf, VALID_KEYS[i])) {
            return TOK_KEY;
        }
    }
    goto fail;
}

static void putchar_tokencol(int type, char c) {
    switch (type) {
        case TOK_KEY:
            print("\e[36m%c\e[0m", c);
            break;
        case TOK_EQUALS:
            print("\e[32m%c\e[0m", c);
            break;
        default:
        case TOK_VALUE:
            print("\e[39m%c\e[0m", c);
            break;
        case TOK_BADKEY:
            print("\e[31m%c\e[0m", c);
            break;
        case TOK_COMMENT:
            print("\e[33m%c\e[0m", c);
            break;
    }
}

static bool editor_no_term_reset = false;

char *config_entry_editor(const char *title, const char *orig_entry) {
    FOR_TERM(TERM->autoflush = false);

    FOR_TERM(TERM->cursor_enabled = true);

    print("\e[2J\e[H");

    if (booting_from_editor) {
        orig_entry = saved_orig_entry;
        title = saved_title;
    }

    size_t cursor_offset  = 0;
    size_t entry_size     = strlen(orig_entry);
    size_t _window_size   = terms[0]->rows - 7 + (menu_branding[0] == '\0' ? 2 : 0);
    size_t window_offset  = 0;
    size_t line_size      = terms[0]->cols - 2;

    // Skip leading newlines
    while (*orig_entry == '\n') {
        orig_entry++;
        entry_size--;
    }

    if (entry_size >= EDITOR_MAX_BUFFER_SIZE) {
        panic(true, "Entry is too big to be edited.");
    }

    bool syntax_highlighting_enabled = true;
    char *syntax_highlighting_enabled_config = config_get_value(NULL, 0, "EDITOR_HIGHLIGHTING");
    if (syntax_highlighting_enabled_config != NULL
     && strcmp(syntax_highlighting_enabled_config, "no") == 0) {
        syntax_highlighting_enabled = false;
    }

    validation_enabled = true;
    char *validation_enabled_config = config_get_value(NULL, 0, "EDITOR_VALIDATION");
    if (validation_enabled_config != NULL
     && strcmp(validation_enabled_config, "no") == 0) {
        validation_enabled = false;
    }

    char *buffer = ext_mem_alloc(EDITOR_MAX_BUFFER_SIZE);
    memcpy(buffer, orig_entry, entry_size);
    buffer[entry_size] = 0;

refresh:
    print("\e[2J\e[H");
    FOR_TERM(TERM->cursor_enabled = false);
    {
        size_t x, y;
        print("\n");
        if (menu_branding[0] != '\0') {
            terms[0]->get_cursor_pos(terms[0], &x, &y);
            {
                size_t branding_len = strlen(menu_branding);
                size_t max_len = terms[0]->cols - 2;
                if (branding_len <= max_len) {
                    set_cursor_pos_helper((terms[0]->cols - branding_len) / 2, y);
                    print("%s%s\e[0m", menu_branding_colour, menu_branding);
                } else {
                    set_cursor_pos_helper(1, y);
                    print("%s%S...\e[0m", menu_branding_colour, menu_branding, (size_t)(max_len - 3));
                }
            }
            print("\n\n");
        }
        terms[0]->get_cursor_pos(terms[0], &x, &y);
        set_cursor_pos_helper((terms[0]->cols - 32) / 2, y);
        print("%sESC\e[0m Discard and Exit    %sF10\e[0m Boot", interface_help_colour, interface_help_colour);
        print("\n\n");
    }

    print(serial ? "/" : "┌");
    for (size_t i = 0; i < terms[0]->cols - 2; i++) {
        switch (i) {
            case 1: case 2: case 3:
                if (window_offset > 0) {
                    print(serial ? "^" : "↑");
                    break;
                }
                // FALLTHRU
            default: {
                size_t title_length = strlen(title);
                size_t max_title = terms[0]->cols - 4;
                size_t display_length = title_length;
                bool truncated = false;
                if (display_length > max_title && max_title > 3) {
                    display_length = max_title;
                    truncated = true;
                }
                if (i == (terms[0]->cols - display_length - 4) / 2) {
                    if (truncated) {
                        print(serial ? "|%S...|" : "┤%S...├", title, (size_t)(display_length - 3));
                    } else {
                        print(serial ? "|%s|" : "┤%s├", title);
                    }
                    i += (display_length + 2) - 1;
                } else {
                    print(serial ? "-" : "─");
                }
            }
        }
    }
    size_t tmpx, tmpy;

    terms[0]->get_cursor_pos(terms[0], &tmpx, &tmpy);
    print(serial ? "\\" : "┐");
    set_cursor_pos_helper(0, tmpy + 1);
    print(serial ? "|" : "│");

    size_t cursor_x, cursor_y;
    size_t current_line = 0, line_offset = 0, window_size = _window_size;
    bool printed_cursor = false;
    bool printed_early = false;
    int token_type = validate_line(buffer);
    size_t tab_space_count = 0;
    for (size_t i = 0; ; i++) {
        // tab
        if (buffer[i] == '\t') {
            tab_space_count = 8 - (line_offset % 8);
            goto tab_part;
        }

        // newline
        if (buffer[i] == '\n'
         && current_line <  window_offset + window_size
         && current_line >= window_offset) {
            size_t x, y;
            terms[0]->get_cursor_pos(terms[0], &x, &y);
            if (i == cursor_offset) {
                cursor_x = x;
                cursor_y = y;
                printed_cursor = true;
            }
            set_cursor_pos_helper(terms[0]->cols - 1, y);
            if (current_line == window_offset + window_size - 1) {
                terms[0]->get_cursor_pos(terms[0], &tmpx, &tmpy);
                print(serial ? "|" : "│");
                set_cursor_pos_helper(0, tmpy + 1);
                print(serial ? "\\" : "└");
            } else {
                terms[0]->get_cursor_pos(terms[0], &tmpx, &tmpy);
                print(serial ? "|" : "│");
                set_cursor_pos_helper(0, tmpy + 1);
                print(serial ? "|" : "│");
            }
            line_offset = 0;
            token_type = validate_line(buffer + i + 1);
            current_line++;
            continue;
        }

        // switch to token type 1 if equals sign
        if (token_type == TOK_KEY && buffer[i] == ':') token_type = TOK_EQUALS;

tab_part:
        if (buffer[i] != 0 && line_offset % line_size == line_size - 1) {
            if (current_line <  window_offset + window_size
             && current_line >= window_offset) {
                if (i == cursor_offset && !printed_cursor) {
                    terms[0]->get_cursor_pos(terms[0], &cursor_x, &cursor_y);
                    printed_cursor = true;
                }
                if (syntax_highlighting_enabled) {
                    putchar_tokencol(token_type, tab_space_count ? ' ' : buffer[i]);
                } else {
                    print("%c", tab_space_count ? ' ' : buffer[i]);
                }
                if (tab_space_count != 0) {
                    tab_space_count--;
                }
                printed_early = true;
                size_t x, y;
                terms[0]->get_cursor_pos(terms[0], &x, &y);
                if (y >= terms[0]->rows - 2) {
                    print(serial ? ">" : "→");
                    set_cursor_pos_helper(0, y + 1);
                    print(serial ? "\\" : "└");
                } else {
                    print(serial ? ">" : "→");
                    set_cursor_pos_helper(0, y + 1);
                    print(serial ? "<" : "←");
                }
            }
            if (window_size > 0) {
                window_size--;
            }
        }

        if (i == cursor_offset
         && current_line <  window_offset + window_size
         && current_line >= window_offset
         && !printed_cursor) {
            terms[0]->get_cursor_pos(terms[0], &cursor_x, &cursor_y);
            printed_cursor = true;
        }

        if (buffer[i] == 0 || current_line >= window_offset + window_size) {
            if (!printed_cursor) {
                if (i <= cursor_offset) {
                    window_offset++;
                    goto refresh;
                }
                if (i > cursor_offset) {
                    window_offset--;
                    goto refresh;
                }
            }
            break;
        }

        if (buffer[i] == '\n') {
            line_offset = 0;
            token_type = validate_line(buffer + i + 1);
            current_line++;
            continue;
        }

        if (current_line >= window_offset) {
            line_offset++;

            // syntax highlighting
            if (!printed_early) {
                if (syntax_highlighting_enabled) {
                    putchar_tokencol(token_type, tab_space_count ? ' ' : buffer[i]);
                } else {
                    print("%c", tab_space_count ? ' ' : buffer[i]);
                }

	        if (tab_space_count != 0) {
                    tab_space_count--;
                }
            }

            printed_early = false;

            // switch to token type 2 after equals sign
            if (token_type == TOK_EQUALS) token_type = TOK_VALUE;

        }

        if (tab_space_count != 0) {
            goto tab_part;
        }
    }

    if (current_line - window_offset < window_size) {
        size_t x, y;
        for (size_t i = 0; i < (window_size - (current_line - window_offset)) - 1; i++) {
            terms[0]->get_cursor_pos(terms[0], &x, &y);
            set_cursor_pos_helper(terms[0]->cols - 1, y);
            print(serial ? "|" : "│");
            set_cursor_pos_helper(0, y + 1);
            print(serial ? "|" : "│");
        }
        terms[0]->get_cursor_pos(terms[0], &x, &y);
        set_cursor_pos_helper(terms[0]->cols - 1, y);
        print(serial ? "|" : "│");
        set_cursor_pos_helper(0, y + 1);
        print(serial ? "\\" : "└");
    }

    {
        const char *overflow_msg = (strlen(buffer) >= EDITOR_MAX_BUFFER_SIZE - 1) ? "Buffer full" : NULL;
        size_t overflow_len = overflow_msg ? strlen(overflow_msg) : 0;

        for (size_t i = 0; i < terms[0]->cols - 2; i++) {
            switch (i) {
                case 1: case 2: case 3:
                    if (current_line - window_offset >= window_size) {
                        print(serial ? "v" : "↓");
                        break;
                    }
                    // FALLTHRU
                default:
                    if (overflow_msg != NULL
                     && i == (terms[0]->cols - overflow_len - 4) / 2) {
                        print(serial ? "|" : "┤");
                        print("\e[31m%s\e[0m", overflow_msg);
                        print(serial ? "|" : "├");
                        i += (overflow_len + 2) - 1;
                    } else {
                        print(serial ? "-" : "─");
                    }
            }
        }
    }
    terms[0]->get_cursor_pos(terms[0], &tmpx, &tmpy);
    print(serial ? "/" : "┘");

    // Hack to redraw the cursor
    set_cursor_pos_helper(cursor_x, cursor_y);
    FOR_TERM(TERM->cursor_enabled = true);

    FOR_TERM(TERM->double_buffer_flush(TERM));

    int c = getchar();
    size_t buffer_len = strlen(buffer);
    switch (c) {
        case GETCHAR_CURSOR_DOWN:
            cursor_offset = get_next_line(cursor_offset, buffer);
            break;
        case GETCHAR_CURSOR_UP:
            cursor_offset = get_prev_line(cursor_offset, buffer);
            break;
        case GETCHAR_CURSOR_LEFT:
            if (cursor_offset) {
                cursor_offset--;
            }
            break;
        case GETCHAR_CURSOR_RIGHT:
            if (cursor_offset < buffer_len) {
                cursor_offset++;
            }
            break;
        case GETCHAR_HOME: {
            size_t displacement;
            get_line_offset(&displacement, cursor_offset, buffer);
            cursor_offset -= displacement;
            break;
        }
        case GETCHAR_END: {
            cursor_offset += get_line_length(cursor_offset, buffer);
            break;
        }
        case '\b':
            if (cursor_offset) {
                cursor_offset--;
        case GETCHAR_DELETE:
                for (size_t i = cursor_offset; i < buffer_len; i++) {
                    buffer[i] = buffer[i+1];
                    if (!buffer[i])
                        break;
                }
            }
            break;
        case GETCHAR_F10:
            memcpy(saved_orig_entry, buffer, buffer_len);
            saved_orig_entry[buffer_len] = 0;
            size_t title_len = strlen(title);
            if (title_len >= sizeof(saved_title)) {
                title_len = sizeof(saved_title) - 4;
                memcpy(saved_title, title, title_len);
                memcpy(saved_title + title_len, "...", 4);
            } else {
                memcpy(saved_title, title, title_len);
                saved_title[title_len] = 0;
            }
            editor_no_term_reset ? editor_no_term_reset = false : reset_term();
            booting_from_editor = true;
            return buffer;
        case GETCHAR_ESCAPE:
            pmm_free(buffer, EDITOR_MAX_BUFFER_SIZE);
            editor_no_term_reset ? editor_no_term_reset = false : reset_term();
            booting_from_editor = false;
            return NULL;
        default:
            if (buffer_len < EDITOR_MAX_BUFFER_SIZE - 1) {
                if (isprint(c) || c == '\n' || c == '\t') {
                    for (size_t i = buffer_len; ; i--) {
                        buffer[i+1] = buffer[i];
                        if (i == cursor_offset)
                            break;
                    }
                    buffer[cursor_offset++] = c;
                }
            }
            break;
    }

    goto refresh;
}

static inline bool should_skip_entry(struct menu_entry *entry) {
    if (entry->sub != NULL) {
        return false;
    }
    char *cur_entry_protocol = config_get_value(entry->body, 0, "PROTOCOL");
    if (cur_entry_protocol) {
#if defined (UEFI)
        if (strcmp(cur_entry_protocol, "bios") == 0
         || strcmp(cur_entry_protocol, "bios_chainload") == 0) {
#elif defined (BIOS)
        if (strcmp(cur_entry_protocol, "efi") == 0
         || strcmp(cur_entry_protocol, "uefi") == 0
         || strcmp(cur_entry_protocol, "efi_chainload") == 0
         || strcmp(cur_entry_protocol, "efi_boot_entry") == 0) {
#endif
            return true;
        }
    }
    char *cur_entry_if_fw_type = config_get_value(entry->body, 0, "IF_FW_TYPE");
    if (cur_entry_if_fw_type) {
        if (strcasecmp(cur_entry_if_fw_type, current_firmware()) != 0) {
            return true;
        }
    }
    char *cur_entry_if_arch = config_get_value(entry->body, 0, "IF_ARCH");
    if (cur_entry_if_arch) {
        const char *arch = current_arch();
        char *cur_arch = cur_entry_if_arch;
        bool skip = true;
        while (*cur_arch) {
            char *cur_arch_end = cur_arch;
            while (*cur_arch_end && !isspace(*cur_arch_end)) {
                ++cur_arch_end;
            }
            if (cur_arch == cur_arch_end) {
                ++cur_arch;
                continue;
            }
            char buf[16];
            if (cur_arch_end - cur_arch >= 16) {
                cur_arch = cur_arch_end;
                continue;
            }
            memcpy(buf, cur_arch, cur_arch_end - cur_arch);
            buf[cur_arch_end - cur_arch] = '\0';
            if (strcasecmp(buf, arch) == 0) {
                skip = false;
                break;
            }
            cur_arch = cur_arch_end;
        }
        if (skip) {
            return true;
        }
    }
    return false;
}

// Count visible (non-skipped) entries in a subtree, respecting expansion state.
static size_t count_visible_entries(struct menu_entry *entry) {
    size_t count = 0;
    while (entry != NULL) {
        if (should_skip_entry(entry)) {
            entry = entry->next;
            continue;
        }
        count++;
        if (entry->sub && entry->expanded) {
            count += count_visible_entries(entry->sub);
        }
        entry = entry->next;
    }
    return count;
}

#if defined(UEFI)
// Count same-named non-skipped siblings preceding this entry (for #N suffix).
static size_t get_sibling_dup_index(struct menu_entry *entry) {
    struct menu_entry *first = entry->parent != NULL ? entry->parent->sub : menu_tree;
    size_t index = 0;
    for (struct menu_entry *e = first; e != entry; e = e->next) {
        if (should_skip_entry(e)) {
            continue;
        }
        if (strcmp(e->name, entry->name) == 0) {
            index++;
        }
    }
    return index;
}

// Write a name into buf, escaping \, / and # characters.
// Returns number of bytes written (not counting NUL terminator).
static size_t escape_name(const char *name, char *buf, size_t buf_size) {
    if (buf_size == 0) {
        return 0;
    }
    size_t j = 0;
    for (size_t i = 0; name[i] != '\0'; i++) {
        if (name[i] == '\\' || name[i] == '/' || name[i] == '#') {
            if (j + 2 < buf_size) {
                buf[j++] = '\\';
                buf[j++] = name[i];
            } else {
                break;
            }
        } else {
            if (j + 1 < buf_size) {
                buf[j++] = name[i];
            } else {
                break;
            }
        }
    }
    buf[j] = '\0';
    return j;
}

static void get_entry_path(struct menu_entry *entry, char *buf, size_t buf_size, size_t *pos) {
    if (entry == NULL || buf_size == 0) {
        return;
    }

    if (entry->parent != NULL) {
        get_entry_path(entry->parent, buf, buf_size, pos);
        if (*pos < buf_size - 1) {
            buf[(*pos)++] = '/';
        }
    }

    size_t remaining = *pos < buf_size ? buf_size - *pos : 0;
    *pos += escape_name(entry->name, buf + *pos, remaining);

    size_t dup_index = get_sibling_dup_index(entry);
    if (dup_index > 0 && *pos < buf_size - 1) {
        buf[(*pos)++] = '#';
        char digits[16];
        size_t ndigits = 0;
        size_t val = dup_index;
        do {
            digits[ndigits++] = '0' + (val % 10);
            val /= 10;
        } while (val > 0);
        for (size_t i = ndigits; i > 0 && *pos < buf_size - 1; i--) {
            buf[(*pos)++] = digits[i - 1];
        }
    }

    if (*pos < buf_size) {
        buf[*pos] = '\0';
    }
}
#endif

// Parse one component from an escaped path string.
// Writes the unescaped name into name_buf and the duplicate index into *dup_index.
// Returns a pointer to the remainder of the path (past the separator).
static const char *parse_path_component(const char *path, char **name_out, size_t *dup_index) {
    *dup_index = 0;
    const char *p = path;

    // First pass: measure the component length
    size_t len = 0;
    const char *scan = path;
    while (*scan != '\0' && *scan != '/') {
        if (*scan == '\\' && scan[1] != '\0') {
            len++;
            scan += 2;
            continue;
        }
        if (*scan == '#') {
            const char *q = scan + 1;
            if (*q >= '0' && *q <= '9') {
                while (*q >= '0' && *q <= '9') {
                    q++;
                }
                if (*q == '\0' || *q == '/') {
                    break;
                }
            }
        }
        len++;
        scan++;
    }

    // Allocate and fill
    char *name_buf = ext_mem_alloc(len + 1);
    size_t j = 0;

    while (*p != '\0' && *p != '/') {
        if (*p == '\\' && p[1] != '\0') {
            name_buf[j++] = p[1];
            p += 2;
            continue;
        }
        if (*p == '#') {
            const char *q = p + 1;
            if (*q >= '0' && *q <= '9') {
                const char *start = q;
                while (*q >= '0' && *q <= '9') {
                    q++;
                }
                if (*q == '\0' || *q == '/') {
                    *dup_index = strtoui(start, NULL, 10);
                    p = q;
                    break;
                }
            }
        }
        name_buf[j++] = *p;
        p++;
    }

    name_buf[j] = '\0';
    *name_out = name_buf;

    if (*p == '/') {
        p++;
    }
    return p;
}

// Find an entry by its escaped path string. If expand_dirs is true, directories on the
// path to the target are expanded. Returns true if found, writing the entry and its
// visible index to *found_entry and *found_index.
static bool find_entry_by_path(const char *path, struct menu_entry *current_entry,
                                size_t base_index, struct menu_entry **found_entry,
                                size_t *found_index, bool expand_dirs) {
    char *comp_name;
    size_t dup_index = 0;
    const char *rest = parse_path_component(path, &comp_name, &dup_index);
    bool is_last = (*rest == '\0');

    size_t idx = base_index;
    size_t same_name_count = 0;
    bool ret = false;

    while (current_entry != NULL) {
        if (should_skip_entry(current_entry)) {
            current_entry = current_entry->next;
            continue;
        }

        bool name_matches = (strcmp(current_entry->name, comp_name) == 0);

        if (name_matches && same_name_count == dup_index) {
            if (is_last && current_entry->sub == NULL) {
                *found_entry = current_entry;
                if (found_index != NULL) {
                    *found_index = idx;
                }
                ret = true;
                break;
            } else if (!is_last && current_entry->sub != NULL) {
                if (expand_dirs) {
                    current_entry->expanded = true;
                }
                ret = find_entry_by_path(rest, current_entry->sub,
                                         idx + 1, found_entry, found_index, expand_dirs);
                break;
            }
        }

        if (name_matches) {
            same_name_count++;
        }

        idx++;
        if (current_entry->sub && current_entry->expanded) {
            idx += count_visible_entries(current_entry->sub);
        }

        current_entry = current_entry->next;
    }

    pmm_free(comp_name, strlen(comp_name) + 1);
    return ret;
}

static size_t print_tree(size_t offset, size_t window, const char *shift, size_t level, size_t base_index, size_t selected_entry,
                      struct menu_entry *current_entry,
                      struct menu_entry **selected_menu_entry,
                      size_t *max_len, size_t *max_height) {
    size_t max_entries = 0;

    bool no_print = false;
    size_t dummy_max_len = 0;
    if (max_len == NULL) {
        max_len = &dummy_max_len;
    }
    size_t dummy_max_height = 0;
    if (max_height == NULL) {
        max_height = &dummy_max_height;
    }
    if (!level) {
        *max_len = 0;
        *max_height = 0;
    }
    if (shift == NULL) {
        no_print = true;
    }

    for (;;) {
        size_t cur_len = 0;
        if (current_entry == NULL)
            break;
        if (should_skip_entry(current_entry)) {
            current_entry = current_entry->next;
            continue;
        }
        if (!no_print && base_index + max_entries < offset) {
            goto skip_line;
        }
        if (!no_print && base_index + max_entries >= offset + window) {
            goto skip_line;
        }
        if (!no_print) print("%s", shift);
        if (level) {
            for (size_t i = level - 1; i > 0; i--) {
                struct menu_entry *actual_parent = current_entry;
                for (size_t j = 0; j < i; j++)
                    actual_parent = actual_parent->parent;
                if (actual_parent->next != NULL) {
                    if (!no_print) print(serial ? " |" : " │");
                } else {
                    if (!no_print) print("  ");
                }
                cur_len += 2;
            }
            if (current_entry->next == NULL) {
                if (!no_print) print(serial ? " `" : " └");
            } else {
                if (!no_print) print(serial ? " |" : " ├");
            }
            cur_len += 2;
        }
        if (current_entry->sub) {
            if (!no_print) print(current_entry->expanded ? "[-]" : "[+]");
        } else if (level) {
            if (!no_print) print(serial ? "-->" : "──►");
        } else {
            if (!no_print) print("   ");
        }
        cur_len += 3;
        if (base_index + max_entries == selected_entry) {
            *selected_menu_entry = current_entry;
            if (!no_print) print("\e[7m");
        }
        {
            size_t name_len = strlen(current_entry->name);
            if (!no_print) {
                size_t prefix_len = shift ? strlen(shift) : 0;
                size_t used = prefix_len + cur_len + 1 + 1; // shift + decorations + space before + space after
                size_t max_name = (terms[0]->cols > used) ? terms[0]->cols - used : 0;
                if (name_len > max_name && max_name > 3) {
                    print(" %S...\e[27m\n", current_entry->name, (size_t)(max_name - 3));
                } else {
                    print(" %s \e[27m\n", current_entry->name);
                }
            }
            (*max_height)++;
            cur_len += 1 + name_len + 1;
        }
skip_line:
        if (current_entry->sub && current_entry->expanded) {
            max_entries += print_tree(offset, window, shift, level + 1, base_index + max_entries + 1,
                                      selected_entry,
                                      current_entry->sub,
                                      selected_menu_entry,
                                      max_len, max_height);
        }
        max_entries++;
        current_entry = current_entry->next;
        if (cur_len > *max_len) {
            *max_len = cur_len;
        }
    }
    return max_entries;
}

static struct memmap_entry *rewound_memmap = NULL;
static size_t rewound_memmap_entries = 0;
static no_unwind uint8_t *rewound_data;
#if defined (BIOS)
static no_unwind uint8_t *rewound_s2_data;
static no_unwind uint8_t *rewound_bss;
#endif

extern symbol data_begin;
extern symbol data_end;
#if defined (BIOS)
extern symbol s2_data_begin;
extern symbol s2_data_end;
extern symbol bss_begin;
extern symbol bss_end;
#endif

static void menu_init_term(void) {
    // If there is GRAPHICS config key and the value is "yes", enable graphics
#if defined (BIOS)
    char *graphics = config_get_value(NULL, 0, "GRAPHICS");
#elif defined (UEFI)
    char *graphics = "yes";
#endif

    if (graphics == NULL || strcmp(graphics, "no") != 0) {
        size_t req_width = 0, req_height = 0, req_bpp = 0;

        char *menu_resolution = config_get_value(NULL, 0, "INTERFACE_RESOLUTION");
        if (menu_resolution != NULL)
            parse_resolution(&req_width, &req_height, &req_bpp, menu_resolution);

        if (!quiet && !gterm_init(NULL, NULL, NULL, req_width, req_height)) {
#if defined (BIOS)
            vga_textmode_init(true);
#elif defined (UEFI)
            serial = true;
            term_fallback();
#endif
        }
    } else {
#if defined (BIOS)
        if (!quiet) {
            vga_textmode_init(true);
        }
#endif
    }
}

#if defined(UEFI)
bool reboot_to_fw_ui_supported(void) {
    uint64_t os_indications_supported;
    UINTN size = sizeof(os_indications_supported);
    EFI_GUID global_variable = EFI_GLOBAL_VARIABLE;
    EFI_STATUS status = gRT->GetVariable(L"OsIndicationsSupported", &global_variable, NULL, &size, &os_indications_supported);
    if (status == EFI_SUCCESS && size == sizeof(os_indications_supported)) {
        return (os_indications_supported & EFI_OS_INDICATIONS_BOOT_TO_FW_UI) != 0;
    }
    return false;
}

noreturn void reboot_to_fw_ui(void) {
    reset_term();
    print("Rebooting to the firmware setup...\n");

    uint64_t os_indications;
    UINTN size = sizeof(os_indications);
    EFI_GUID global_variable = EFI_GLOBAL_VARIABLE;
    EFI_STATUS status = gRT->GetVariable(L"OsIndications", &global_variable, NULL, &size, &os_indications);
    if (status != EFI_SUCCESS || size != sizeof(os_indications)) {
        if (status == EFI_NOT_FOUND) {
            os_indications = 0;
            goto not_found;
        }

        panic(true, "Failed to get OsIndications variable, status=%X", status);
    }

not_found:;
    uint64_t new_os_indications = os_indications | EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
    status = gRT->SetVariable(L"OsIndications", &global_variable,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(new_os_indications), &new_os_indications);

    if (status != EFI_SUCCESS) {
        panic(true, "Failed to set OsIndications variable, status=%X", status);
    }

    gRT->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
    panic(true, "Failed to reboot to firmware UI");
}
#endif

noreturn void _menu(bool first_run) {
    size_t data_size = (uintptr_t)data_end - (uintptr_t)data_begin;
#if defined (BIOS)
    size_t s2_data_size = (uintptr_t)s2_data_end - (uintptr_t)s2_data_begin;
    size_t bss_size = (uintptr_t)bss_end - (uintptr_t)bss_begin;
#endif

    if (rewound_memmap != NULL) {
        memcpy(data_begin, rewound_data, data_size);
#if defined (BIOS)
        memcpy(s2_data_begin, rewound_s2_data, s2_data_size);
        memcpy(bss_begin, rewound_bss, bss_size);
#endif
        memcpy(memmap, rewound_memmap, rewound_memmap_entries * sizeof(struct memmap_entry));
        memmap_entries = rewound_memmap_entries;
    } else {
        rewound_data = ext_mem_alloc(data_size);
#if defined (BIOS)
        rewound_s2_data = ext_mem_alloc(s2_data_size);
        rewound_bss = ext_mem_alloc(bss_size);
#endif
        /* addition due to allocation potentially adding new memory map entries */
        rewound_memmap = ext_mem_alloc_counted(memmap_entries + 16, sizeof(struct memmap_entry));
        memcpy(rewound_memmap, memmap, memmap_entries * sizeof(struct memmap_entry));
        rewound_memmap_entries = memmap_entries;
        memcpy(rewound_data, data_begin, data_size);
#if defined (BIOS)
        memcpy(rewound_s2_data, s2_data_begin, s2_data_size);
        memcpy(rewound_bss, bss_begin, bss_size);
#endif
    }

    term_fallback();

    if (bad_config == false) {
        if (!init_config_smbios()) {

#if defined (UEFI)
            if (init_config_disk(boot_volume)) {
#endif
            volume_iterate_parts(boot_volume,
                if (!init_config_disk(_PART)) {
                    boot_volume = _PART;
                    break;
                }
            );
#if defined (UEFI)
            }
#endif
        }
    }

#if defined (__riscv)
    init_riscv(NULL);
#endif

    char *quiet_str = config_get_value(NULL, 0, "QUIET");
    quiet = quiet_str != NULL && strcmp(quiet_str, "yes") == 0;

    char *verbose_str = config_get_value(NULL, 0, "VERBOSE");
    verbose = verbose_str != NULL && strcmp(verbose_str, "yes") == 0;

    char *serial_str = config_get_value(NULL, 0, "SERIAL");
    serial =
#if defined (UEFI)
        is_efi_serial_present() &&
#endif
        serial_str != NULL && strcmp(serial_str, "yes") == 0;

#if defined (UEFI)
    if (!serial) {
        char *graphics_str = config_get_value(NULL, 0, "GRAPHICS");
        if (graphics_str != NULL && strcmp(graphics_str, "no") == 0) {
            serial = true;
        }
    }
#endif

#if defined (BIOS)
    if (serial) {
        char *baudrate_s = config_get_value(NULL, 0, "SERIAL_BAUDRATE");
        if (baudrate_s == NULL) {
            serial_baudrate = 115200;
        } else {
            serial_baudrate = strtoui(baudrate_s, NULL, 10);
            if (serial_baudrate == 0 || serial_baudrate > 115200) {
                serial_baudrate = 115200;
            }
        }
    }
#endif

    char *hash_mismatch_panic_str = config_get_value(NULL, 0, "HASH_MISMATCH_PANIC");
    hash_mismatch_panic = hash_mismatch_panic_str == NULL || strcmp(hash_mismatch_panic_str, "yes") == 0;

    if (secure_boot_active) {
        hash_mismatch_panic = true;
        editor_enabled = false;
    }

#if defined (UEFI)
    char *measured_boot_str = config_get_value(NULL, 0, "MEASURED_BOOT");
    measured_boot = measured_boot_str != NULL && strcmp(measured_boot_str, "yes") == 0;
    if (secure_boot_active) {
        measured_boot = true;
    }
    // Cannot do measured boot without a TPM/CC interface.
    if (!tpm_present()) {
        measured_boot = false;
    }

    // Measure the on-disk config bytes now that measured_boot is final.
    size_t raw_size;
    const char *raw = config_get_raw(&raw_size);
    if (raw != NULL) {
        tpm_measure(TPM_PCR_LOADED_IMAGES, TPM_EV_IPL,
                    raw, raw_size, "limine_cfg", NULL);
    }
#endif

    char *randomise_mem_str = config_get_value(NULL, 0, "RANDOMISE_MEMORY");
    if (randomise_mem_str == NULL)
        randomise_mem_str = config_get_value(NULL, 0, "RANDOMIZE_MEMORY");
    bool randomise_mem = randomise_mem_str != NULL && strcmp(randomise_mem_str, "yes") == 0;
    if (randomise_mem) {
        pmm_randomise_memory();
    }

    char *editor_enabled_str = config_get_value(NULL, 0, "EDITOR_ENABLED");
    if (editor_enabled_str != NULL && !secure_boot_active) {
        editor_enabled = strcmp(editor_enabled_str, "yes") == 0;
    }

    char *help_hidden_str = config_get_value(NULL, 0, "INTERFACE_HELP_HIDDEN");
    if (help_hidden_str != NULL) {
        help_hidden = strcmp(help_hidden_str, "yes") == 0;
    }

    uint32_t help_rgb = 0x00aa00;
    char *interface_help_colour_str = config_get_value(NULL, 0, "INTERFACE_HELP_COLOUR");
    if (interface_help_colour_str == NULL) {
        interface_help_colour_str = config_get_value(NULL, 0, "INTERFACE_HELP_COLOR");
    }
    if (interface_help_colour_str != NULL) {
        parse_rgb_colour_value(interface_help_colour_str, &help_rgb);
    }
    format_fg_rgb_escape(interface_help_colour, help_rgb);

    uint32_t help_bright_rgb = brighten_rgb(help_rgb);
    char *interface_help_colour_bright_str = config_get_value(NULL, 0, "INTERFACE_HELP_COLOUR_BRIGHT");
    if (interface_help_colour_bright_str == NULL) {
        interface_help_colour_bright_str = config_get_value(NULL, 0, "INTERFACE_HELP_COLOR_BRIGHT");
    }
    if (interface_help_colour_bright_str != NULL) {
        parse_rgb_colour_value(interface_help_colour_bright_str, &help_bright_rgb);
    }
    format_fg_rgb_escape(interface_help_colour_bright, help_bright_rgb);

    bool custom_branding = false;
    {
        char *tmp = config_get_value(NULL, 0, "INTERFACE_BRANDING");
        if (tmp != NULL) {
            size_t len = strlen(tmp) + 1;
            menu_branding = ext_mem_alloc(len);
            memcpy(menu_branding, tmp, len);
            custom_branding = true;
        }
    }
    if (menu_branding == NULL) {
#if defined (BIOS)
        {
            uint32_t eax, ebx, ecx, edx;
            if (!cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 29))) {
                menu_branding = strdup("Limine " LIMINE_VERSION " (ia-32, BIOS)");
            } else {
                menu_branding = strdup("Limine " LIMINE_VERSION " (x86-64, BIOS)");
            }
        }
#elif defined (UEFI)
#if defined (__i386__)
        {
            uint32_t eax, ebx, ecx, edx;
            if (!cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 29))) {
                menu_branding = strdup("Limine " LIMINE_VERSION " (ia-32, UEFI32)");
            } else {
                menu_branding = strdup("Limine " LIMINE_VERSION " (x86-64, UEFI32)");
            }
        }
#else
        menu_branding = strdup("Limine " LIMINE_VERSION " ("
#if defined (__x86_64__)
            "x86-64"
#elif defined (__riscv)
            "riscv64"
#elif defined (__aarch64__)
            "aarch64"
#elif defined (__loongarch64)
            "loongarch64"
#endif
            ", UEFI)");
#endif
#endif
    }

    if (secure_boot_active && !custom_branding) {
        const char *suffix = ", Secure Boot)";
        size_t suffix_len = strlen(suffix) + 1;
        size_t old_len = strlen(menu_branding);
        menu_branding = pmm_realloc(menu_branding, old_len + 1, old_len + suffix_len);
        memcpy(menu_branding + old_len - 1, suffix, suffix_len);
    }

    {
        char *tmp = config_get_value(NULL, 0, "INTERFACE_BRANDING_COLOUR");
        if (tmp == NULL)
            tmp = config_get_value(NULL, 0, "INTERFACE_BRANDING_COLOR");
        if (tmp != NULL) {
            uint32_t rgb;
            if (parse_rgb_colour_value(tmp, &rgb)) {
                format_fg_rgb_escape(menu_branding_colour, rgb);
            }
        }
    }

    bool skip_timeout = false;
    struct menu_entry *selected_menu_entry = NULL;

    size_t selected_entry = 0;

    bool has_entry = false;

#if defined (UEFI)
    {
        char path[MENU_PATH_MAX];
        if (bli_get_oneshot_entry(path, MENU_PATH_MAX)) {
            // Find the entry with this path, expand directories, and get its index.
            struct menu_entry *found_entry = NULL;
            size_t found_index = 0;
            find_entry_by_path(path, menu_tree, 0, &found_entry, &found_index, true);
            if (found_entry != NULL) {
                selected_entry = found_index;
                has_entry = true;
            }
        }
    }
#endif

    if (!has_entry) {
        char *default_entry = config_get_value(NULL, 0, "DEFAULT_ENTRY");
        if (default_entry != NULL) {
            bool is_index = true;
            for (const char *p = default_entry; *p != '\0'; p++) {
                if (*p < '0' || *p > '9') {
                    is_index = false;
                    break;
                }
            }
            if (is_index) {
                selected_entry = strtoui(default_entry, NULL, 10);
                if (selected_entry)
                    selected_entry--;
            } else {
                // Copy the path since find_entry_by_path calls config_get_value
                // internally (via should_skip_entry), which clobbers the static buffer.
                char default_entry_path[MENU_PATH_MAX];
                size_t len = strlen(default_entry);
                if (len >= sizeof(default_entry_path)) {
                    len = sizeof(default_entry_path) - 1;
                }
                memcpy(default_entry_path, default_entry, len);
                default_entry_path[len] = '\0';
                struct menu_entry *found_entry = NULL;
                size_t found_index = 0;
                find_entry_by_path(default_entry_path, menu_tree, 0, &found_entry, &found_index, true);
                if (found_entry != NULL) {
                    selected_entry = found_index;
                }
            }
        }
    }

#if defined (UEFI)
    if (!has_entry) {
        char *remember_last = config_get_value(NULL, 0, "REMEMBER_LAST_ENTRY");
        if (remember_last != NULL && strcasecmp(remember_last, "yes") == 0) {
            char last_entry_path[MENU_PATH_MAX];
            UINTN getvar_size = sizeof(last_entry_path);
            if (gRT->GetVariable(L"LimineLastBootedEntry",
                                 &limine_efi_vendor_guid,
                                 NULL,
                                 &getvar_size,
                                 last_entry_path) == 0 && getvar_size > 0) {
                // Ensure NUL termination
                last_entry_path[getvar_size < sizeof(last_entry_path) ? getvar_size : sizeof(last_entry_path) - 1] = '\0';
                // Find the entry with this path, expand directories, and get its index.
                struct menu_entry *found_entry = NULL;
                size_t found_index = 0;
                find_entry_by_path(last_entry_path, menu_tree, 0, &found_entry, &found_index, true);
                if (found_entry != NULL) {
                    selected_entry = found_index;
                    has_entry = true;
                }
            }
        }
    }
    if (!has_entry) {
        char path[MENU_PATH_MAX];
        if (bli_get_default_entry(path, MENU_PATH_MAX)) {
            // Find the entry with this path, expand directories, and get its index.
            struct menu_entry *found_entry = NULL;
            size_t found_index = 0;
            find_entry_by_path(path, menu_tree, 0, &found_entry, &found_index, true);
            if (found_entry != NULL) {
                selected_entry = found_index;
                has_entry = true;
            }
        }
    }
#endif

    // Use print tree to load up selected_menu_entry and determine if the
    // default entry is valid.
    size_t max_entries = print_tree(0, 0, NULL, 0, 0, selected_entry, menu_tree, &selected_menu_entry, NULL, NULL);
    if (selected_entry >= max_entries) {
        selected_entry = 0;
    }

    size_t timeout = 5;

    bool has_timeout = false;

#if defined (UEFI)
    has_timeout = bli_update_oneshot_timeout(&timeout, &skip_timeout);
#endif

    if (!has_timeout) {
        char *timeout_config = config_get_value(NULL, 0, "TIMEOUT");
        if (timeout_config != NULL) {
            has_timeout = true;
            if (!strcmp(timeout_config, "no"))
                skip_timeout = true;
            else
                timeout = strtoui(timeout_config, NULL, 10);
        }
    }

#if defined (UEFI)
    if (!has_timeout) {
        has_timeout = bli_update_timeout(&timeout, &skip_timeout);
    }
#endif

    if (timeout > 9999)
        timeout = 9999;

#if defined(UEFI)
    bool reboot_to_firmware_supported = reboot_to_fw_ui_supported();
#endif

    if (!first_run) {
        quiet = false;
        skip_timeout = true;
    }

    if (!skip_timeout && !timeout) {
        if (max_entries == 0 || selected_menu_entry == NULL || selected_menu_entry->sub != NULL) {
            quiet = false;
            print("Default entry is not valid or directory, booting to menu.\n");
            skip_timeout = true;
        } else {
            goto autoboot;
        }
    }

    menu_init_term();

    if (terms[0]->cols < 40 || terms[0]->rows < 16) {
        // Terminal too small for menu, fall back to text console
#if defined (BIOS)
        vga_textmode_init(true);
#elif defined (UEFI)
        serial = true;
        term_fallback();
#endif
    }

    size_t tree_offset = 0;
    size_t header_offset = (menu_branding[0] != '\0') ? 2 : 0;
    bool has_secondary_help = editor_enabled;
#if defined(UEFI)
    has_secondary_help = has_secondary_help || reboot_to_firmware_supported;
#endif
    if (has_secondary_help) {
        header_offset += 2;
    }

refresh:
    if (selected_entry >= tree_offset + terms[0]->rows - 8 - header_offset) {
        tree_offset = selected_entry - (terms[0]->rows - 9 - header_offset);
    }
    if (selected_entry < tree_offset) {
        tree_offset = selected_entry;
    }

    FOR_TERM(TERM->autoflush = false);

    FOR_TERM(TERM->cursor_enabled = false);

    print("\e[2J\e[H");
    {
        size_t x, y;
        print("\n");
        if (menu_branding[0] != '\0') {
            terms[0]->get_cursor_pos(terms[0], &x, &y);
            {
                size_t branding_len = strlen(menu_branding);
                size_t max_len = terms[0]->cols - 2;
                if (branding_len <= max_len) {
                    set_cursor_pos_helper((terms[0]->cols - branding_len) / 2, y);
                    print("%s%s\e[0m", menu_branding_colour, menu_branding);
                } else {
                    set_cursor_pos_helper(1, y);
                    print("%s%S...\e[0m", menu_branding_colour, menu_branding, (size_t)(max_len - 3));
                }
            }
            print("\n\n\n\n");
        }
    }

    if (max_entries == 0) {
        if (quiet) {
            quiet = false;
            menu_init_term();
        }
        const char *msg;
        if (config_ready) {
            msg = "[config file contains no valid entries]";
        } else {
            msg = "[config file not found]";
        }
        set_cursor_pos_helper((terms[0]->cols - strlen(msg)) / 2, (terms[0]->rows - 1) / 2);
        print("%s\n", msg);
    }

    size_t max_tree_len, max_tree_height;
    max_entries = print_tree(tree_offset, terms[0]->rows - 8 - header_offset, NULL, 0, 0, selected_entry, menu_tree,
                             &selected_menu_entry, &max_tree_len, &max_tree_height);

    if (max_entries != 0) {
        size_t tree_prefix_len = (terms[0]->cols > max_tree_len + 3) ? (terms[0]->cols - max_tree_len - 3) / 2 : 1;
        char *tree_prefix = ext_mem_alloc(tree_prefix_len + 1);
        memset(tree_prefix, ' ', tree_prefix_len);

        if (max_tree_height > terms[0]->rows - 8 - header_offset) {
            max_tree_height = terms[0]->rows - 8 - header_offset;
        }

        size_t tree_start = (terms[0]->rows - max_tree_height) / 2;
        if (tree_start < 4 + header_offset) {
            tree_start = 4 + header_offset;
        }
        set_cursor_pos_helper(0, tree_start);

        max_entries = print_tree(tree_offset, terms[0]->rows - 8 - header_offset, tree_prefix, 0, 0, selected_entry, menu_tree,
                                 &selected_menu_entry, NULL, NULL);

        pmm_free(tree_prefix, tree_prefix_len + 1);
    }

    {
        size_t x, y;
        terms[0]->get_cursor_pos(terms[0], &x, &y);

        if (max_entries != 0) {
            if (tree_offset > 0) {
                set_cursor_pos_helper((terms[0]->cols - 3) / 2, 3 + header_offset);
                print(serial ? "^^^" : "↑↑↑");
            }

            if (tree_offset + (terms[0]->rows - 8 - header_offset) < max_entries) {
                set_cursor_pos_helper((terms[0]->cols - 3) / 2, terms[0]->rows - 4);
                print(serial ? "vvv" : "↓↓↓");
            }
        }

        if (!help_hidden) {
            if (max_entries != 0) {
                size_t primary_row = 1 + header_offset - (has_secondary_help ? 2 : 0);
                if (selected_menu_entry->sub == NULL) {
                    if (editor_enabled) {
                        set_cursor_pos_helper((terms[0]->cols - 37) / 2, primary_row);
                        print("%sARROWS\e[0m Select    %sENTER\e[0m Boot    %sE\e[0m Edit",
                              interface_help_colour, interface_help_colour, interface_help_colour);
                    } else {
                        set_cursor_pos_helper((terms[0]->cols - 27) / 2, primary_row);
                        print("%sARROWS\e[0m Select    %sENTER\e[0m Boot",
                              interface_help_colour, interface_help_colour);
                    }
                } else {
                    const char *action = selected_menu_entry->expanded ? "Collapse" : "Expand";
                    size_t len = 23 + strlen(action);
                    set_cursor_pos_helper((terms[0]->cols - len) / 2, primary_row);
                    print("%sARROWS\e[0m Select    %sENTER\e[0m %s",
                          interface_help_colour, interface_help_colour, action);
                }
            }
            if (has_secondary_help) {
                size_t secondary_row = 1 + header_offset;
#if defined(UEFI)
                if (reboot_to_firmware_supported && editor_enabled) {
                    set_cursor_pos_helper((terms[0]->cols - 33) / 2, secondary_row);
                    print("%sS\e[0m Firmware Setup    %sB\e[0m Blank Entry",
                          interface_help_colour, interface_help_colour);
                } else if (reboot_to_firmware_supported) {
                    set_cursor_pos_helper((terms[0]->cols - 16) / 2, secondary_row);
                    print("%sS\e[0m Firmware Setup", interface_help_colour);
                } else
#endif
                if (editor_enabled) {
                    set_cursor_pos_helper((terms[0]->cols - 13) / 2, secondary_row);
                    print("%sB\e[0m Blank Entry", interface_help_colour);
                }
            }
        }
        set_cursor_pos_helper(x, y);
    }

    if (max_entries == 0 || selected_menu_entry->sub != NULL)
        skip_timeout = true;

    int c;

    if (skip_timeout == false) {
        print("\n\n");
        for (size_t i = timeout; i; i--) {
            size_t ndigits = 1;
            for (size_t tmp = i / 10; tmp > 0; tmp /= 10) ndigits++;
            size_t msg_len = 28 + ndigits;
            set_cursor_pos_helper((terms[0]->cols - msg_len) / 2, terms[0]->rows - 2);
            FOR_TERM(TERM->scroll_enabled = false);
            print("\e[2K%sBooting automatically in %s%U%s...\e[0m",
                  interface_help_colour, interface_help_colour_bright, (uint64_t)i, interface_help_colour);
            FOR_TERM(TERM->scroll_enabled = true);
            FOR_TERM(TERM->double_buffer_flush(TERM));
            if ((c = pit_sleep_and_quit_on_keypress(1))) {
                skip_timeout = true;
                if (quiet) {
                    quiet = false;
                    menu_init_term();
                    goto timeout_aborted;
                }
                print("\e[2K");
                FOR_TERM(TERM->double_buffer_flush(TERM));
                goto timeout_aborted;
            }
        }
        goto autoboot;
    }

    if (max_entries != 0 && selected_menu_entry->comment != NULL) {
        size_t comment_len = strlen(selected_menu_entry->comment);
        size_t max_len = terms[0]->cols - 2;
        FOR_TERM(TERM->scroll_enabled = false);
        if (comment_len <= max_len) {
            set_cursor_pos_helper((terms[0]->cols - comment_len) / 2, terms[0]->rows - 2);
            print("\e[36m%s\e[0m", selected_menu_entry->comment);
        } else {
            set_cursor_pos_helper(1, terms[0]->rows - 2);
            print("\e[36m%S...\e[0m", selected_menu_entry->comment, (size_t)(max_len - 3));
        }
        FOR_TERM(TERM->scroll_enabled = true);
    }

    if (booting_from_editor) {
        if (booting_from_blank) {
            goto editor_blank;
        }
        goto editor;
    }

    FOR_TERM(TERM->double_buffer_flush(TERM));

    for (;;) {
        c = getchar();
timeout_aborted:
        if (max_entries == 0) {
            switch (c) {
                case 'b': case 'B': case 's': case 'S':
                    break;
                default:
                    continue;

            }
        }
        switch (c) {
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9': {
                int ent = (c - '0') - 1;
                if (ent < (int)max_entries) {
                    selected_entry = ent;
                    print_tree(0, 0, NULL, 0, 0, selected_entry, menu_tree,
                               &selected_menu_entry, NULL, NULL);
                    goto autoboot;
                }
                goto refresh;
            }
            case GETCHAR_HOME:
                selected_entry = 0;
                goto refresh;
            case GETCHAR_END:
                selected_entry = max_entries - 1;
                goto refresh;
            case GETCHAR_CURSOR_UP:
                if (selected_entry == 0)
                    selected_entry = max_entries - 1;
                else
                    selected_entry--;
                goto refresh;
            case GETCHAR_CURSOR_DOWN:
                if (++selected_entry == max_entries)
                    selected_entry = 0;
                goto refresh;
            case GETCHAR_CURSOR_RIGHT:
            case '\n':
            case ' ':
            autoboot:
                if (max_entries == 0) {
                    break;
                }
                if (selected_menu_entry->sub != NULL) {
                    selected_menu_entry->expanded = !selected_menu_entry->expanded;
                    goto refresh;
                }
                if (!quiet) {
                    if (term_backend == FALLBACK) {
                        if (!gterm_init(NULL, NULL, NULL, 0, 0)) {
#if defined (BIOS)
                            vga_textmode_init(true);
#elif defined (UEFI)
                            serial = true;
                            term_fallback();
#endif
                        }
                    } else {
                        reset_term();
                    }
                }

#if defined (UEFI)
                // Save the entry's path so it can persist between boots.
                char entry_path[MENU_PATH_MAX];
                size_t pos = 0;
                get_entry_path(selected_menu_entry, entry_path, sizeof(entry_path), &pos);
                gRT->SetVariable(L"LimineLastBootedEntry",
                                 &limine_efi_vendor_guid,
                                 EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                                 strlen(entry_path) + 1,
                                 entry_path);
                bli_set_selected_entry(entry_path);
#endif

                boot(selected_menu_entry->body);
            case 'e':
            case 'E': {
                if (editor_enabled) {
                    editor:
                    if (max_entries == 0 || selected_menu_entry->sub != NULL) {
                        break;
                    }
                    editor_no_term_reset = true;
                    char *new_body = config_entry_editor(selected_menu_entry->name, selected_menu_entry->body);
                    if (new_body == NULL)
                        goto refresh;
                    selected_menu_entry->body = new_body;
                    goto autoboot;
                }
                break;
            }
#if defined(UEFI)
            case 's':
            case 'S': {
                if (reboot_to_firmware_supported) {
                    reboot_to_fw_ui();
                }
                break;
            }
#endif
            case 'b':
            case 'B': {
                if (editor_enabled) {
                    editor_blank:
                    booting_from_blank = true;
                    char *new_entry = config_entry_editor("Blank Entry", "");
                    if (new_entry != NULL) {
                        config_ready = true;
                        boot(new_entry);
                    }
                    booting_from_blank = false;
                    goto refresh;
                }
                break;
            }
        }
    }
}

noreturn void boot(char *config) {
#if defined (__riscv)
    init_riscv(config);
#endif

    char *cmdline;
    {
        char *tmp = config_get_value(config, 0, "KERNEL_CMDLINE");
        if (!tmp) {
            tmp = config_get_value(config, 0, "CMDLINE");
        }
        if (tmp) {
            size_t len = strlen(tmp) + 1;
            cmdline = ext_mem_alloc(len);
            memcpy(cmdline, tmp, len);
        } else {
            cmdline = "";
        }
    }

    char *proto = config_get_value(config, 0, "PROTOCOL");
    if (proto == NULL) {
        panic(true, "Boot protocol not specified for this entry");
    }

    if (!strcmp(proto, "limine")) {
        limine_load(config, cmdline);
    } else if (!strcmp(proto, "linux")) {
        linux_load(config, cmdline);
    } else if (!strcmp(proto, "multiboot1") || !strcmp(proto, "multiboot")) {
#if defined (__x86_64__) || defined (__i386__)
        multiboot1_load(config, cmdline);
#else
        quiet = false;
        print("Multiboot 1 is not available on non-x86 architectures.\n\n");
#endif
    } else if (!strcmp(proto, "multiboot2")) {
#if defined (__x86_64__) || defined (__i386__)
        multiboot2_load(config, cmdline);
#else
        quiet = false;
        print("Multiboot 2 is not available on non-x86 architectures.\n\n");
#endif
#if defined (BIOS)
    } else if (!strcmp(proto, "bios_chainload")
            || !strcmp(proto, "bios")) {
#elif defined (UEFI)
    } else if (!strcmp(proto, "efi_chainload")
            || !strcmp(proto, "efi")
            || !strcmp(proto, "uefi")) {
#endif
        chainload(config, cmdline);
    }
#if defined (UEFI)
    else if (!strcmp(proto, "efi_boot_entry")) {
        efi_boot_entry(config);
    }
#endif

    panic(true, "Unsupported protocol specified.");
}
