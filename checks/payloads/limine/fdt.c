#include <stddef.h>
#include "common.h"
#include "fdt.h"

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

bool fdt_magic_ok(const void *dtb) {
    return be32(dtb) == 0xd00dfeed;
}

uint32_t fdt_totalsize(const void *dtb) {
    return be32((const uint8_t *)dtb + 4);
}

static size_t align4(size_t v) {
    return (v + 3) & ~(size_t)3;
}

// Calls fn for every property, with the node path components on a stack.
struct walk {
    const char *names[16];
    size_t lens[16];
    int depth;
};

typedef bool (*node_fn)(void *ctx, struct walk *w, const char *name, size_t len);
typedef bool (*prop_fn)(void *ctx, struct walk *w, const char *name, const void *val, int len);

static void fdt_walk(const void *dtb, void *ctx, node_fn on_node, prop_fn on_prop) {
    const uint8_t *base = dtb;
    const uint8_t *p = base + be32(base + 8);
    const char *strings = (const char *)base + be32(base + 12);
    struct walk w = { .depth = -1 };
    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        switch (tok) {
            case FDT_BEGIN_NODE: {
                const char *name = (const char *)p;
                size_t len = strlen(name);
                p += align4(len + 1);
                w.depth++;
                if (w.depth < 16) {
                    w.names[w.depth] = name;
                    w.lens[w.depth] = len;
                }
                if (on_node != NULL && !on_node(ctx, &w, name, len)) {
                    return;
                }
                break;
            }
            case FDT_END_NODE:
                w.depth--;
                break;
            case FDT_PROP: {
                uint32_t len = be32(p);
                uint32_t nameoff = be32(p + 4);
                const void *val = p + 8;
                p += 8 + align4(len);
                if (on_prop != NULL && !on_prop(ctx, &w, strings + nameoff, val, (int)len)) {
                    return;
                }
                break;
            }
            case FDT_NOP:
                break;
            default:
                return;
        }
    }
}

struct count_ctx {
    const char *name;
    int count;
};

static bool count_node(void *ctx, struct walk *w, const char *name, size_t len) {
    struct count_ctx *c = ctx;
    size_t n = strlen(c->name);
    if (w->depth == 1 && len >= n && memcmp(name, c->name, n) == 0
     && (name[n] == 0 || name[n] == '@')) {
        c->count++;
    }
    return true;
}

int fdt_count_root_nodes(const void *dtb, const char *name) {
    struct count_ctx c = { name, 0 };
    fdt_walk(dtb, &c, count_node, NULL);
    return c.count;
}

struct prop_ctx {
    const char *path;
    const char *prop;
    const void *val;
    int len;
};

static bool path_matches(struct walk *w, const char *path) {
    // "/" is the root alone; "/a/b" names components 1 and 2.
    if (path[0] == '/' && path[1] == 0) {
        return w->depth == 0;
    }
    int depth = 0;
    const char *p = path;
    while (*p == '/') {
        p++;
        depth++;
        const char *end = p;
        while (*end != 0 && *end != '/') {
            end++;
        }
        if (depth > w->depth || depth >= 16) {
            return false;
        }
        if ((size_t)(end - p) != w->lens[depth] || memcmp(p, w->names[depth], end - p) != 0) {
            return false;
        }
        p = end;
    }
    return depth == w->depth;
}

static bool find_prop(void *ctx, struct walk *w, const char *name, const void *val, int len) {
    struct prop_ctx *c = ctx;
    if (strcmp(name, c->prop) == 0 && path_matches(w, c->path)) {
        c->val = val;
        c->len = len;
        return false;
    }
    return true;
}

const void *fdt_getprop_path(const void *dtb, const char *path, const char *prop, int *len) {
    struct prop_ctx c = { path, prop, NULL, 0 };
    fdt_walk(dtb, &c, NULL, find_prop);
    if (len != NULL) {
        *len = c.len;
    }
    return c.val;
}
