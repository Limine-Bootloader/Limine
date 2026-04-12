#ifndef LIB__BLI_H__
#define LIB__BLI_H__

#if defined (UEFI)

void init_bli(void);
void bli_on_boot(void);
bool bli_update_oneshot_timeout(size_t *timeout, bool *skip_timeout);
bool bli_update_timeout(size_t *timeout, bool *skip_timeout);
void bli_set_selected_entry(const char *path);
bool bli_get_default_entry(char *path, size_t buf_size);
bool bli_get_oneshot_entry(char *path, size_t buf_size);

#endif

#endif
