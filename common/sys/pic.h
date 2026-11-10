#ifndef SYS__PIC_H
#define SYS__PIC_H

#include <stdbool.h>

void pic_eoi(int irq);
void pic_flush(void);
void pic_set_mask(int line, bool status);
void pic_mask_all(void);

#endif
