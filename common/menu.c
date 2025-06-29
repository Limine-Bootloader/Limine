#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <config.h>
#include <menu.h>
#include <lib/print.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/config.h>
#include <lib/term.h>
#include <lib/gterm.h>
#include <lib/getchar.h>
#include <lib/uri.h>
#include <mm/pmm.h>
#include <drivers/vbe.h>
#include <drivers/vga_textmode.h>
#include <protos/linux.h>
#include <protos/chainload.h>
#include <protos/multiboot1.h>
#include <protos/multiboot2.h>
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

static char *menu_branding = NULL;
static char *menu_branding_colour = NULL;
no_unwind bool booting_from_editor = false;
static no_unwind bool booting_from_blank = false;
static no_unwind char saved_orig_entry[EDITOR_MAX_BUFFER_SIZE];
static no_unwind char saved_title[64];

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
    NULL
};

static bool validation_enabled = true;
static bool invalid_syntax = false;

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
    invalid_syntax = true;
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
    size_t _window_size   = terms[0]->rows - 8;
    size_t window_offset  = 0;
    size_t line_size      = terms[0]->cols - 2;

    bool display_overflow_error = false;

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
    invalid_syntax = false;

    print("\e[2J\e[H");
    FOR_TERM(TERM->cursor_enabled = false);
    {
        size_t x, y;
        print("\n");
        terms[0]->get_cursor_pos(terms[0], &x, &y);
        set_cursor_pos_helper(terms[0]->cols / 2 - DIV_ROUNDUP(strlen(menu_branding), 2), y);
        print("\e[3%sm%s\e[0m", menu_branding_colour, menu_branding);
        print("\n\n");
    }

    print("    \e[32mESC\e[0m Discard and Exit    \e[32mF10\e[0m Boot\n\n");

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
                if (i == (terms[0]->cols / 2) - DIV_ROUNDUP(title_length, 2) - 1 - 1) {
                    print(serial ? "|%s|" : "┤%s├", title);
                    i += (title_length + 2) - 1;
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
                if (y == terms[0]->rows - 3) {
                    print(serial ? ">" : "→");
                    set_cursor_pos_helper(0, y + 1);
                    print(serial ? "\\" : "└");
                } else {
                    print(serial ? ">" : "→");
                    set_cursor_pos_helper(0, y + 1);
                    print(serial ? "<" : "←");
                }
            }
            window_size--;
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

    // syntax error alert
    if (validation_enabled) {
        size_t x, y;
        terms[0]->get_cursor_pos(terms[0], &x, &y);
        set_cursor_pos_helper(0, terms[0]->rows - 1);
        FOR_TERM(TERM->scroll_enabled = false);
        if (invalid_syntax) {
            print("\e[31mConfiguration is INVALID.\e[0m");
        } else {
            print("\e[32mConfiguration is valid.\e[0m");
        }
        FOR_TERM(TERM->scroll_enabled = true);
        set_cursor_pos_helper(x, y);
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

    for (size_t i = 0; i < terms[0]->cols - 2; i++) {
        switch (i) {
            case 1: case 2: case 3:
                if (current_line - window_offset >= window_size) {
                    print(serial ? "v" : "↓");
                    break;
                }
                // FALLTHRU
            default:
                print(serial ? "-" : "─");
        }
    }
    terms[0]->get_cursor_pos(terms[0], &tmpx, &tmpy);
    print(serial ? "/" : "┘");
    set_cursor_pos_helper(0, tmpy + 1);

    if (display_overflow_error) {
        FOR_TERM(TERM->scroll_enabled = false);
        print("\e[31mText buffer not big enough, delete something instead.");
        FOR_TERM(TERM->scroll_enabled = true);
        display_overflow_error = false;
    }

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
                for (size_t i = cursor_offset; ; i++) {
                    buffer[i] = buffer[i+1];
                    if (!buffer[i])
                        break;
                }
            }
            break;
        case GETCHAR_F10:
            memcpy(saved_orig_entry, buffer, buffer_len);
            saved_orig_entry[buffer_len] = 0;
            strcpy(saved_title, title);
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
            } else {
                display_overflow_error = true;
            }
            break;
    }

    goto refresh;
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
        if (!no_print) print(" %s \e[27m\n", current_entry->name);
        (*max_height)++;
        cur_len += 1 + strlen(current_entry->name) + 1;
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
        rewound_memmap = ext_mem_alloc((memmap_entries + 16) * sizeof(struct memmap_entry));
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

    char *hash_mismatch_panic_str = config_get_value(NULL, 0, "HASH_MISMATCH_PANIC");
    hash_mismatch_panic = hash_mismatch_panic_str == NULL || strcmp(hash_mismatch_panic_str, "yes") == 0;

    char *randomise_mem_str = config_get_value(NULL, 0, "RANDOMISE_MEMORY");
    if (randomise_mem_str == NULL)
        randomise_mem_str = config_get_value(NULL, 0, "RANDOMIZE_MEMORY");
    bool randomise_mem = randomise_mem_str != NULL && strcmp(randomise_mem_str, "yes") == 0;
    if (randomise_mem) {
        pmm_randomise_memory();
    }

    char *editor_enabled_str = config_get_value(NULL, 0, "EDITOR_ENABLED");
    if (editor_enabled_str != NULL) {
        editor_enabled = strcmp(editor_enabled_str, "yes") == 0;
    }

    char *help_hidden_str = config_get_value(NULL, 0, "INTERFACE_HELP_HIDDEN");
    if (help_hidden_str != NULL) {
        help_hidden = strcmp(help_hidden_str, "yes") == 0;
    }

    menu_branding = config_get_value(NULL, 0, "INTERFACE_BRANDING");
    if (menu_branding == NULL) {
#if defined (BIOS)
        {
            uint32_t eax, ebx, ecx, edx;
            if (!cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 29))) {
                menu_branding = "Limine " LIMINE_VERSION " (ia-32, BIOS)";
            } else {
                menu_branding = "Limine " LIMINE_VERSION " (x86-64, BIOS)";
            }
        }
#elif defined (UEFI)
#if defined (__i386__)
        {
            uint32_t eax, ebx, ecx, edx;
            if (!cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 29))) {
                menu_branding = "Limine " LIMINE_VERSION " (ia-32, UEFI32)";
            } else {
                menu_branding = "Limine " LIMINE_VERSION " (x86-64, UEFI32)";
            }
        }
#else
        menu_branding = "Limine " LIMINE_VERSION " ("
#if defined (__x86_64__)
            "x86-64"
#elif defined (__riscv)
            "riscv64"
#elif defined (__aarch64__)
            "aarch64"
#elif defined (__loongarch64)
            "loongarch64"
#endif
            ", UEFI)";
#endif
#endif
    }

    menu_branding_colour = config_get_value(NULL, 0, "INTERFACE_BRANDING_COLOUR");
    if (menu_branding_colour == NULL)
        menu_branding_colour = config_get_value(NULL, 0, "INTERFACE_BRANDING_COLOR");
    if (menu_branding_colour == NULL)
        menu_branding_colour = "6";

    bool skip_timeout = false;
    struct menu_entry *selected_menu_entry = NULL;

    size_t selected_entry = 0;

    char *default_entry = config_get_value(NULL, 0, "DEFAULT_ENTRY");
    if (default_entry != NULL) {
        selected_entry = strtoui(default_entry, NULL, 10);
        if (selected_entry)
            selected_entry--;
    }

#if defined (UEFI)
    char *remember_last = config_get_value(NULL, 0, "REMEMBER_LAST_ENTRY");
    if (remember_last != NULL && strcasecmp(remember_last, "yes") == 0) {
        UINTN getvar_size = sizeof(size_t);
        size_t last;
        if (gRT->GetVariable(L"LimineLastBootedEntry",
                             &limine_efi_vendor_guid,
                             NULL,
                             &getvar_size,
                             &last) == 0 && getvar_size == sizeof(size_t)) {
            selected_entry = last;
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
    char *timeout_config = config_get_value(NULL, 0, "TIMEOUT");
    if (timeout_config != NULL) {
        if (!strcmp(timeout_config, "no"))
            skip_timeout = true;
        else
            timeout = strtoui(timeout_config, NULL, 10);
    }

#if defined(UEFI)
    bool reboot_to_firmware_supported = reboot_to_fw_ui_supported();
#endif

    if (!first_run) {
        quiet = false;
        skip_timeout = true;
    }

    if (!skip_timeout && !timeout) {
        if (max_entries == 0 || selected_menu_entry->sub != NULL) {
            quiet = false;
            print("Default entry is not valid or directory, booting to menu.\n");
            skip_timeout = true;
        } else {
            goto autoboot;
        }
    }

    menu_init_term();

    size_t tree_offset = 0;

refresh:
    if (selected_entry >= tree_offset + terms[0]->rows - 8) {
        tree_offset = selected_entry - (terms[0]->rows - 9);
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
        terms[0]->get_cursor_pos(terms[0], &x, &y);
        set_cursor_pos_helper(terms[0]->cols / 2 - DIV_ROUNDUP(strlen(menu_branding), 2), y);
        print("\e[3%sm%s\e[0m", menu_branding_colour, menu_branding);
        print("\n\n\n\n");
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
        set_cursor_pos_helper(terms[0]->cols / 2 - strlen(msg) / 2, terms[0]->rows / 2);
        print("%s\n", msg);
    }

    size_t max_tree_len, max_tree_height;
    max_entries = print_tree(tree_offset, terms[0]->rows - 8, NULL, 0, 0, selected_entry, menu_tree,
                             &selected_menu_entry, &max_tree_len, &max_tree_height);

    if (max_entries != 0) {
        size_t tree_prefix_len = (terms[0]->cols / 2 - DIV_ROUNDUP(max_tree_len, 2)) - 2;
        char *tree_prefix = ext_mem_alloc(tree_prefix_len + 1);
        memset(tree_prefix, ' ', tree_prefix_len);

        if (max_tree_height > terms[0]->rows - 10) {
            max_tree_height = terms[0]->rows - 10;
        }

        set_cursor_pos_helper(0, terms[0]->rows / 2 - max_tree_height / 2);

        max_entries = print_tree(tree_offset, terms[0]->rows - 8, tree_prefix, 0, 0, selected_entry, menu_tree,
                                 &selected_menu_entry, NULL, NULL);

        pmm_free(tree_prefix, tree_prefix_len);
    }

    {
        size_t x, y;
        terms[0]->get_cursor_pos(terms[0], &x, &y);

        if (max_entries != 0) {
            if (tree_offset > 0) {
                set_cursor_pos_helper(terms[0]->cols / 2 - 1, 4);
                print(serial ? "^^^" : "↑↑↑");
            }

            if (tree_offset + (terms[0]->rows - 8) < max_entries) {
                set_cursor_pos_helper(terms[0]->cols / 2 - 1, terms[0]->rows - 3);
                print(serial ? "vvv" : "↓↓↓");
            }
        }

        if (!help_hidden) {
            set_cursor_pos_helper(0, 3);
            if (max_entries != 0) {
                if (selected_menu_entry->sub == NULL) {
                    print("    \e[32mARROWS\e[0m Select    \e[32mENTER\e[0m Boot    %s",
                          editor_enabled ? "\e[32mE\e[0m Edit" : "");
                } else {
                    print("    \e[32mARROWS\e[0m Select    \e[32mENTER\e[0m %s",
                          selected_menu_entry->expanded ? "Collapse" : "Expand");
                }
            }
#if defined(UEFI)
            if (reboot_to_firmware_supported) {
                set_cursor_pos_helper(terms[0]->cols - (editor_enabled ? 37 : 20), 3);
                print("\e[32mS\e[0m Firmware Setup");
            }
#endif
            if (editor_enabled) {
                set_cursor_pos_helper(terms[0]->cols - 17, 3);
                print("\e[32mB\e[0m Blank Entry");
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
            set_cursor_pos_helper(0, terms[0]->rows - 1);
            FOR_TERM(TERM->scroll_enabled = false);
            print("\e[2K\e[32mBooting automatically in \e[92m%u\e[32m, press any key to stop the countdown...\e[0m", i);
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

    set_cursor_pos_helper(0, terms[0]->rows - 1);
    if (max_entries != 0 && selected_menu_entry->comment != NULL) {
        FOR_TERM(TERM->scroll_enabled = false);
        print("\e[36m%s\e[0m", selected_menu_entry->comment);
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
                            term_fallback();
#endif
                        }
                    } else {
                        reset_term();
                    }
                }

#if defined (UEFI)
                gRT->SetVariable(L"LimineLastBootedEntry",
                                 &limine_efi_vendor_guid,
                                 EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                                 sizeof(size_t),
                                 &selected_entry);
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

    char *cmdline = config_get_value(config, 0, "KERNEL_CMDLINE");
    if (!cmdline) {
        cmdline = config_get_value(config, 0, "CMDLINE");
    }
    if (!cmdline) {
        cmdline = "";
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

    panic(true, "Unsupported protocol specified.");
}
