#ifndef PXE_H
#define PXE_H

#include <stdint.h>
#include <lib/part.h>

#define DHCP_ACK_PACKET_LEN 296

extern uint8_t cached_dhcp_packet[DHCP_ACK_PACKET_LEN];
extern bool cached_dhcp_ack_valid;

#if defined (BIOS)

struct volume *pxe_bind_volume(void);
void pxe_init(void);
int pxe_call(uint16_t opcode, uint16_t buf_seg, uint16_t buf_off);

// PXE 2.1, 3.4.1 (BOOTPLAYER). The specification never gives BOOTP_DHCPVEND a
// value, so the vendor area is left open-ended.
struct bootph {
    uint8_t opcode;
    uint8_t hardware;
    uint8_t hardlen;
    uint8_t gatehops;
    uint32_t ident;
    uint16_t seconds;
    uint16_t flags;
    uint32_t cip;
    uint32_t yip;
    uint32_t sip;
    uint32_t gip;
    uint8_t caddr[16];
    uint8_t sname[64];
    uint8_t bootfile[128];
    uint8_t vendor[];
} __attribute__((packed));

_Static_assert(sizeof(struct bootph) == 236, "BOOTP vendor area is at offset 236");

 struct PXENV_UNDI_GET_INFORMATION {
    uint16_t Status;
    uint16_t BaseIo;
    uint16_t IntNumber;
    uint16_t MaxTranUnit;
    uint16_t HwType;
    uint16_t HwAddrLen;
    uint8_t CurrentNodeAddress[16];
    uint8_t PermNodeAddress[16];
    uint16_t ROMAddress;
    uint16_t RxBufCt;
    uint16_t TxBufCt;
 };

// PXE 2.1, Table 1-1.
struct segoff16 {
    uint16_t offset;
    uint16_t segment;
} __attribute__((packed));

struct segdesc {
    uint16_t segment_address;
    uint32_t physical_address;
    uint16_t seg_size;
} __attribute__((packed));

// PXE 2.1, Table 3-1.
struct pxenv {
    uint8_t signature[6];
    uint16_t version;
    uint8_t length;
    uint8_t checksum;
    struct segoff16 rm_entry;
    uint32_t pm_offset;
    uint16_t pm_selector;
    uint16_t stack_seg;
    uint16_t stack_size;
    uint16_t bc_code_seg;
    uint16_t bc_code_size;
    uint16_t bc_data_seg;
    uint16_t bc_data_size;
    uint16_t undi_data_seg;
    uint16_t undi_data_size;
    uint16_t undi_code_seg;
    uint16_t undi_code_size;
    struct segoff16 pxe_ptr;
} __attribute__((packed));

_Static_assert(sizeof(struct pxenv) == 0x2c, "PXENV+ layout");

// PXE 2.1, Table 3-2.
struct pxe {
    uint8_t signature[4];
    uint8_t struct_length;
    uint8_t struct_cksum;
    uint8_t struct_rev;
    uint8_t reserved1;
    struct segoff16 undi_rom_id;
    struct segoff16 base_rom_id;
    struct segoff16 entry_point_sp;
    struct segoff16 entry_point_esp;
    struct segoff16 status_callout;
    uint8_t reserved2;
    uint8_t seg_desc_cnt;
    uint16_t first_selector;
    struct segdesc stack;
    struct segdesc undi_data;
    struct segdesc undi_code;
    struct segdesc undi_code_write;
    struct segdesc bc_data;
    struct segdesc bc_code;
    struct segdesc bc_code_write;
} __attribute__((packed));

_Static_assert(sizeof(struct pxe) == 0x58, "!PXE layout");

// PXE 2.1, 3.4.1.
#define PXENV_GET_CACHED_INFO 0x0071
#define PXENV_PACKET_TYPE_DHCP_DISCOVER 1
#define PXENV_PACKET_TYPE_DHCP_ACK 2
#define PXENV_PACKET_TYPE_CACHED_REPLY 3
struct pxenv_get_cached_info {
    uint16_t status;
    uint16_t packet_type;
    uint16_t buffer_size;
    struct segoff16 buffer;
    uint16_t buffer_limit;
} __attribute__((packed));

#elif defined (UEFI)

struct volume *pxe_bind_volume(EFI_HANDLE efi_handle, EFI_PXE_BASE_CODE *pxe_base_code);

#endif

#endif
