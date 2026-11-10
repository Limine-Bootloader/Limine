#ifndef PXE__TFTP_H
#define PXE__TFTP_H

#include <stdint.h>
#include <stddef.h>
#include <fs/file.h>

#if defined(BIOS)

#define UNDI_GET_INFORMATION 0xC

#define TFTP_OPEN 0x0020
struct pxenv_open {
    uint16_t status;
    uint32_t sip;
    uint32_t gip;
    uint8_t name[128];
    uint16_t port;
    uint16_t packet_size;
 } __attribute__((packed));

#define TFTP_READ 0x22
struct pxenv_read {
    uint16_t status;
    uint16_t pn;
    uint16_t bsize;
    uint16_t boff;
    uint16_t bseg;
} __attribute__((packed));

#define TFTP_GET_FILE_SIZE 0x25
struct pxenv_get_file_size {
    uint16_t status;
    uint32_t sip;
    uint32_t gip;
    uint8_t name[128];
    uint32_t file_size;
} __attribute__((packed));

#define TFTP_CLOSE 0x21

#endif

struct file_handle *tftp_open(struct volume *part, const char *server_addr, const char *name);

#endif
