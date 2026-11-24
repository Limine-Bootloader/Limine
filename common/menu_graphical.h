#ifndef MENU_GRAPHICAL_H
#define MENU_GRAPHICAL_H

#include <stdbool.h>
#include <stddef.h>
#include <menu_model.h>

bool menu_graphical_init(void);
void menu_graphical_suspend(void);
void menu_graphical_resume(void);
void menu_graphical_draw(const struct menu_model *model, size_t selected,
                         const char *branding, bool help_hidden,
                         const char *status);
bool menu_graphical_hit_test(size_t cell_x, size_t cell_y, size_t *index);
void menu_graphical_close(void);

#endif
