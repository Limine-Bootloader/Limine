#ifndef LIB__MISC_H__
#define LIB__MISC_H__

#include <stdbool.h>
#include <stdnoreturn.h>

#define SIZEOF_ARRAY(array) (sizeof(array) / sizeof(array[0]))

noreturn void panic(bool allow_menu, const char *fmt, ...);

#endif
