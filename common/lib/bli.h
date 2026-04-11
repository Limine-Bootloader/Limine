#ifndef LIB__BLI_H__
#define LIB__BLI_H__

#if defined (UEFI)

void init_bli(void);
void bli_on_boot(void);
bool bli_update_oneshot_timeout(size_t *timeout, bool *skip_timeout);
bool bli_update_timeout(size_t *timeout, bool *skip_timeout);

#endif

#endif
