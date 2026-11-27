#include <stddef.h>
#include <stdbool.h>
#include <menu_model.h>
#include <mm/pmm.h>

static size_t count_entries(struct menu_entry *tree, bool (*skip)(struct menu_entry *)) {
    size_t count = 0;
    for (struct menu_entry *entry = tree; entry != NULL; entry = entry->next) {
        if (skip(entry)) {
            continue;
        }
        count++;
        if (entry->sub != NULL && entry->expanded) {
            count += count_entries(entry->sub, skip);
        }
    }
    return count;
}

static void append_entries(struct menu_model *model, struct menu_entry *tree,
                           bool (*skip)(struct menu_entry *), size_t parent,
                           size_t depth, size_t *index) {
    for (struct menu_entry *entry = tree; entry != NULL; entry = entry->next) {
        if (skip(entry)) {
            continue;
        }

        size_t current = (*index)++;
        model->rows[current] = (struct menu_row){
            .entry = entry,
            .parent = parent,
            .depth = depth
        };

        if (entry->sub != NULL && entry->expanded) {
            append_entries(model, entry->sub, skip, current, depth + 1, index);
        }
    }
}

void menu_model_update(struct menu_model *model, struct menu_entry *tree,
                       bool (*skip)(struct menu_entry *)) {
    size_t count = count_entries(tree, skip);
    if (count != model->count) {
        if (model->rows != NULL) {
            pmm_free(model->rows, model->count * sizeof(*model->rows));
        }
        model->rows = count == 0 ? NULL : ext_mem_alloc_counted(count, sizeof(*model->rows));
        model->count = count;
    }

    size_t index = 0;
    if (count != 0) {
        append_entries(model, tree, skip, (size_t)-1, 0, &index);
    }
}

void menu_model_destroy(struct menu_model *model) {
    if (model->rows != NULL) {
        pmm_free(model->rows, model->count * sizeof(*model->rows));
    }
    model->rows = NULL;
    model->count = 0;
}
