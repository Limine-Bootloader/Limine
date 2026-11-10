#ifndef LIB__MISC_H
#define LIB__MISC_H

#include <stdbool.h>
#include <stdnoreturn.h>

#define SIZEOF_ARRAY(array) (sizeof(array) / sizeof(array[0]))

noreturn void panic(bool allow_menu, const char *fmt, ...);

#endif
