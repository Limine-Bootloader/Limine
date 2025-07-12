#if !defined(__i386__) && !defined(__x86_64__)

#include <stdint.h>
#include <stddef.h>
#include <libfdt.h>

static int fdt_get_or_add_chosen_node(void *fdt) {
    int offset = fdt_subnode_offset(fdt, 0, "chosen");

    if (offset == -FDT_ERR_NOTFOUND) {
        offset = fdt_add_subnode(fdt, 0, "chosen");
    }

    return offset;
}

int fdt_set_chosen_string(void *fdt, const char *name, const char *value) {
    int chosen_offset = fdt_get_or_add_chosen_node(fdt);
    if (chosen_offset < 0) {
        return chosen_offset;
    }

    return fdt_setprop_string(fdt, chosen_offset, name, value);
}

int fdt_set_chosen_uint64(void *fdt, const char *name, uint64_t value) {
    int chosen_offset = fdt_get_or_add_chosen_node(fdt);
    if (chosen_offset < 0) {
        return chosen_offset;
    }

    return fdt_setprop_u64(fdt, chosen_offset, name, value);
}

int fdt_set_chosen_uint32(void *fdt, const char *name, uint32_t value) {
    int chosen_offset = fdt_get_or_add_chosen_node(fdt);
    if (chosen_offset < 0) {
        return chosen_offset;
    }

    return fdt_setprop_u32(fdt, chosen_offset, name, value);
}

#endif
