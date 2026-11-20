#ifndef HIDDEN_H
#define HIDDEN_H

// Hides declarations as well as the definitions -fvisibility=hidden hides, so
// that position independent code reaches what other files define directly,
// rather than through the GOT.
#ifndef __ASSEMBLER__
#   pragma GCC visibility push(hidden)
#endif

#endif
