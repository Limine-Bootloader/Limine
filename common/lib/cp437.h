#ifndef LIB__CP437_H
#define LIB__CP437_H

#include <stdint.h>

// The code point standard CP437 assigns each code, with the IBM PC glyphs for
// the control codes. It never produces 0, which is left blank like the IBM
// glyph.
extern const uint16_t cp437_to_unicode[256];

#endif
