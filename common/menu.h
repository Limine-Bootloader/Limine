#ifndef MENU_H
#define MENU_H

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

#define KEYBOARD_LAYOUT_UNKNOWN (-1)
#define KEYBOARD_LAYOUT_QWERTY 0
#define KEYBOARD_LAYOUT_DVORAK 1
#define KEYBOARD_LAYOUT_AZERTY 2

extern int current_keyboard_layout;

#endif
