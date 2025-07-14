#ifndef LIB__TERM_H__
#define LIB__TERM_H__

#include <stddef.h>
#include <stdint.h>
#include <lib/print.h>
#include <flanterm.h>

enum {
    _NOT_READY,
    GTERM,
    TEXTMODE,
    FALLBACK
};

#if defined (BIOS)
extern int current_video_mode;
#endif

extern struct flanterm_context **terms;
extern size_t terms_i;

extern int term_backend;

#define TERM_CTX_SIZE ((uint64_t)(-1))
#define TERM_CTX_SAVE ((uint64_t)(-2))
#define TERM_CTX_RESTORE ((uint64_t)(-3))
#define TERM_FULL_REFRESH ((uint64_t)(-4))
#define TERM_OOB_OUTPUT_GET ((uint64_t)(-10))
#define TERM_OOB_OUTPUT_SET ((uint64_t)(-11))

#define FOR_TERM(...) do { \
    for (size_t FOR_TERM_i = 0; FOR_TERM_i < terms_i; FOR_TERM_i++) { \
        struct flanterm_context *TERM = terms[FOR_TERM_i]; \
        __VA_ARGS__ \
        ; \
    } \
} while (0)

static inline void reset_term(void) {
    for (size_t i = 0; i < terms_i; i++) {
        struct flanterm_context *term = terms[i];

        print("\e[2J\e[H");
        flanterm_context_reinit(term);
        term->cursor_enabled = true;
        term->double_buffer_flush(term);
    }
}

static inline void set_cursor_pos_helper(size_t x, size_t y) {
    print("\e[%u;%uH", (int)y + 1, (int)x + 1);
}

void term_notready(void);
void term_fallback(void);
void _term_write(struct flanterm_context *term, uint64_t buf, uint64_t count);

#endif
