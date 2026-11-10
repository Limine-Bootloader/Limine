#ifndef DRIVERS__SERIAL_H
#define DRIVERS__SERIAL_H

#if defined(BIOS)

#include <stdint.h>

extern uint32_t serial_baudrate;

void serial_initialise(void);
void serial_out(uint8_t b);
int serial_in(void);

#endif

#endif
