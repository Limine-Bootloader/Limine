#ifndef COMMON__MENU_MODEL_H
#define COMMON__MENU_MODEL_H

#include <stddef.h>
#include <lib/config.h>

struct menu_row {
    struct menu_entry *entry;
    size_t parent;
    size_t depth;
};

struct menu_model {
    struct menu_row *rows;
    size_t count;
};

void menu_model_update(struct menu_model *model, struct menu_entry *tree,
                       bool (*skip)(struct menu_entry *));
void menu_model_destroy(struct menu_model *model);

#endif
