#ifndef LIB__GETCHAR_H__
#define LIB__GETCHAR_H__

#include <stddef.h>

#define GETCHAR_CURSOR_LEFT  (-10)
#define GETCHAR_CURSOR_RIGHT (-11)
#define GETCHAR_CURSOR_UP    (-12)
#define GETCHAR_CURSOR_DOWN  (-13)
#define GETCHAR_DELETE       (-14)
#define GETCHAR_END          (-15)
#define GETCHAR_HOME         (-16)
#define GETCHAR_PGUP         (-17)
#define GETCHAR_PGDOWN       (-18)
#define GETCHAR_F10          (-19)
#define GETCHAR_ESCAPE       (-20)
#define GETCHAR_MOUSE        (-30)

#if defined (BIOS)
#   define GETCHAR_RCTRL 0x4
#   define GETCHAR_LCTRL GETCHAR_RCTRL
#elif defined (UEFI)
#   define GETCHAR_RCTRL EFI_RIGHT_CONTROL_PRESSED
#   define GETCHAR_LCTRL EFI_LEFT_CONTROL_PRESSED
#endif

int getchar(void);

#endif
