#ifndef MENU_H__
#define MENU_H__

#include <stdbool.h>
#include <stdnoreturn.h>

#define MENU_PATH_MAX 1024

#if defined(UEFI)
bool reboot_to_fw_ui_supported(void);
noreturn void reboot_to_fw_ui(void);
#endif

noreturn void menu(bool first_run);

noreturn void boot(char *config);

char *config_entry_editor(const char *title, const char *orig_entry);

extern bool booting_from_editor;

enum keyboard_layout {
    KEYBOARD_LAYOUT_UNKNOWN = -1,
    KEYBOARD_LAYOUT_QWERTY,
    KEYBOARD_LAYOUT_DVORAK,
    KEYBOARD_LAYOUT_AZERTY,
};

extern enum keyboard_layout current_keyboard_layout;

#endif
