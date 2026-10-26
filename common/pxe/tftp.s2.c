#include <pxe/tftp.h>
#include <pxe/pxe.h>
#if defined (BIOS)
#  include <lib/real.h>
#elif defined (UEFI)
#  include <efi.h>
#endif
#include <lib/print.h>
#include <lib/libc.h>
#include <mm/pmm.h>
#include <lib/misc.h>

// The DHCP ACK, for the Multiboot 2 network tag. With ProxyDHCP or a separate
// boot server, the reply naming the TFTP server has neither the client address
// nor the DHCP server's options.
uint8_t cached_dhcp_packet[DHCP_ACK_PACKET_LEN] = { 0 };
size_t cached_dhcp_packet_len = 0;
bool cached_dhcp_ack_valid = false;

#if defined (BIOS)

static uint32_t get_boot_server_info(void) {
    // Leaving Buffer and BufferSize zero makes PXE report where its own copy of
    // the packet lives (PXE 2.1, 3.4.1), so no buffer has to be sized for a
    // packet of unknown length.
    struct pxenv_get_cached_info cached_info = {
        .status = 0,
        .packet_type = PXENV_PACKET_TYPE_CACHED_REPLY,
    };

    int ret = pxe_call(PXENV_GET_CACHED_INFO, ((uint16_t)rm_seg(&cached_info)), (uint16_t)rm_off(&cached_info));
    if (ret) {
        panic(false, "tftp: Failed to get cached DHCP reply (status %x)", cached_info.status);
    }

    if (cached_info.buffer_size < sizeof(struct bootph)) {
        panic(false, "tftp: Cached DHCP reply too short (%u bytes)", cached_info.buffer_size);
    }

    struct bootph *reply = (struct bootph *)rm_desegment(cached_info.buffer.segment, cached_info.buffer.offset);

    return reply->sip;
}

static void cache_dhcp_ack(void) {
    struct pxenv_get_cached_info cached_info = {
        .status = 0,
        .packet_type = PXENV_PACKET_TYPE_DHCP_ACK,
    };

    int ret = pxe_call(PXENV_GET_CACHED_INFO, ((uint16_t)rm_seg(&cached_info)), (uint16_t)rm_off(&cached_info));
    if (ret || cached_info.buffer_size < sizeof(struct bootph)) {
        return;
    }

    void *ack = (void *)rm_desegment(cached_info.buffer.segment, cached_info.buffer.offset);
    cached_dhcp_packet_len = MIN(cached_info.buffer_size, DHCP_ACK_PACKET_LEN);
    memcpy(cached_dhcp_packet, ack, cached_dhcp_packet_len);
    cached_dhcp_ack_valid = true;
}

static uint32_t parse_ip_addr(const char *server_addr) {
    uint32_t out;

    if (!server_addr || !strlen(server_addr)) {
        if (!cached_dhcp_ack_valid) {
            cache_dhcp_ack();
        }
        return get_boot_server_info();
    }

    // The URI code refuses an address inet_pton() cannot read.
    inet_pton(server_addr, &out);

    return out;
}

struct file_handle *tftp_open(struct volume *part, const char *server_addr, const char *name) {
    // Off PXE there is no PXE API to call.
    if (part == NULL || !part->pxe) {
        return NULL;
    }

    uint32_t server_ip = parse_ip_addr(server_addr);
    const uint16_t server_port = 69; // This couldn't be changed previously either
    int ret = 0;

    struct PXENV_UNDI_GET_INFORMATION undi_info = { 0 };
    ret = pxe_call(UNDI_GET_INFORMATION, ((uint16_t)rm_seg(&undi_info)), (uint16_t)rm_off(&undi_info));
    if (ret) {
        return NULL;
    }

    //TODO figure out a more proper way to do this.
    if (undi_info.MaxTranUnit < 48) {
        print("tftp: Invalid MTU (%u), too small for TFTP headers\n", undi_info.MaxTranUnit);
        return NULL;
    }
    uint16_t mtu = undi_info.MaxTranUnit - 48;

    size_t name_len = strlen(name);
    if (name_len >= 128) {
        print("tftp: Filename too long (max 127 chars)\n");
        return NULL;
    }

    struct pxenv_get_file_size fsize = {
        .status = 0,
        .sip = server_ip,
    };
    memcpy(fsize.name, name, name_len + 1);
    ret = pxe_call(TFTP_GET_FILE_SIZE, ((uint16_t)rm_seg(&fsize)), (uint16_t)rm_off(&fsize));
    if (ret) {
        return NULL;
    }

    struct file_handle *handle = ext_mem_alloc(sizeof(struct file_handle));

    handle->size = fsize.file_size;
    handle->is_memfile = true;

    handle->pxe = true;
    memcpy(handle->pxe_ip, &server_ip, 4);
    handle->pxe_port = server_port;

    handle->path = ext_mem_alloc(1 + name_len + 1);
    handle->path[0] = '/';
    memcpy(&handle->path[1], name, name_len);
    handle->path_len = 1 + name_len + 1;

    struct pxenv_open open = {
        .status = 0,
        .sip = server_ip,
        .port = (server_port >> 8) | (server_port << 8),
        .packet_size = mtu
    };
    memcpy(open.name, name, name_len + 1);

    ret = pxe_call(TFTP_OPEN, ((uint16_t)rm_seg(&open)), (uint16_t)rm_off(&open));
    if (ret) {
        print("tftp: Failed to open file %x or bad packet size", open.status);
        pmm_free(handle->path, handle->path_len);
        pmm_free(handle, sizeof(struct file_handle));
        return NULL;
    }

    // Validate server's negotiated packet size doesn't exceed our buffer
    if (open.packet_size > mtu) {
        print("tftp: Server requested packet size %u exceeds our MTU %u\n", open.packet_size, mtu);
        uint16_t close = 0;
        pxe_call(TFTP_CLOSE, ((uint16_t)rm_seg(&close)), (uint16_t)rm_off(&close));
        pmm_free(handle->path, handle->path_len);
        pmm_free(handle, sizeof(struct file_handle));
        return NULL;
    }

    uint16_t alloc_mtu = mtu;  // Save original MTU for allocation/free
    uint8_t *buf = conv_mem_alloc(alloc_mtu);

    mtu = open.packet_size;
    handle->fd = ext_mem_alloc(handle->size);

    size_t progress = 0;
    bool slow = false;

    while (progress < handle->size) {
        struct pxenv_read read = {
            .boff = ((uint16_t)rm_off(buf)),
            .bseg = ((uint16_t)rm_seg(buf)),
        };

        ret = pxe_call(TFTP_READ, ((uint16_t)rm_seg(&read)), (uint16_t)rm_off(&read));
        if (ret) {
            panic(false, "tftp: Read failure");
        }

        // Validate read size doesn't overflow the buffer (use alloc_mtu, not server's mtu)
        if (read.bsize > alloc_mtu || read.bsize > handle->size - progress) {
            panic(false, "tftp: Server sent more data than expected");
        }

        // Prevent infinite loop from zero-byte reads
        if (read.bsize == 0 && progress < handle->size) {
            panic(false, "tftp: Server returned zero bytes before transfer complete");
        }

        memcpy(handle->fd + progress, buf, read.bsize);

        progress += read.bsize;

        if (read.bsize < mtu && !slow && progress < handle->size) {
            slow = true;
            print("tftp: Server is sending the file in smaller packets (it sent %d bytes), download might take longer.\n", read.bsize);
        }
    }

    uint16_t close = 0;
    ret = pxe_call(TFTP_CLOSE, ((uint16_t)rm_seg(&close)), (uint16_t)rm_off(&close));
    if (ret) {
        panic(false, "tftp: Close failure");
    }

    pmm_free(buf, alloc_mtu);

    return handle;
}

#elif defined (UEFI)

_Static_assert(sizeof(EFI_PXE_BASE_CODE_PACKET) <= DHCP_ACK_PACKET_LEN, "DHCP packet buffer too small");

// The firmware keeps the packet in a fixed-size union, so its length is only
// known from where the End option sits (RFC 2132).
static size_t dhcp_packet_len(const uint8_t *packet, size_t max) {
    size_t i = offsetof(EFI_PXE_BASE_CODE_DHCPV4_PACKET, DhcpOptions);

    while (i < max) {
        if (packet[i] == 255) {
            return i + 1;
        }

        // Pad is the only other option without a length octet.
        if (packet[i] == 0) {
            i++;
        } else if (i + 1 < max) {
            i += 2 + packet[i + 1];
        } else {
            break;
        }
    }

    return max;
}

static EFI_IP_ADDRESS *parse_ip_addr(struct volume *part, const char *server_addr) {
    static EFI_IP_ADDRESS out;

    if (!server_addr || !strlen(server_addr)) {
        EFI_PXE_BASE_CODE_PACKET* packet;
        if (part->pxe_base_code->Mode->PxeReplyReceived) packet = &part->pxe_base_code->Mode->PxeReply;
        else if (part->pxe_base_code->Mode->ProxyOfferReceived) packet = &part->pxe_base_code->Mode->ProxyOffer;
        else packet = &part->pxe_base_code->Mode->DhcpAck;
        memcpy(out.Addr, packet->Dhcpv4.BootpSiAddr, 4);
        if (!cached_dhcp_ack_valid && part->pxe_base_code->Mode->DhcpAckReceived) {
            EFI_PXE_BASE_CODE_PACKET *ack = &part->pxe_base_code->Mode->DhcpAck;
            cached_dhcp_packet_len = dhcp_packet_len(ack->Raw, sizeof(ack->Raw));
            memcpy(cached_dhcp_packet, ack, cached_dhcp_packet_len);
            cached_dhcp_ack_valid = true;
        }
    } else {
        // The URI code refuses an address inet_pton() cannot read.
        inet_pton(server_addr, &out.Addr);
    }

    return &out;
}

struct file_handle *tftp_open(struct volume *part, const char *server_addr, const char *name) {
    if (part == NULL || !part->pxe_base_code) {
        return NULL;
    }

    EFI_IP_ADDRESS *ip = parse_ip_addr(part, server_addr);

    EFI_UINT64 file_size;
    EFI_STATUS status;

    // Firmware from before EDK2 cf9ff46b rejects a NULL BufferPtr for every
    // Mtftp() opcode, not just the reading ones, so give it somewhere to point.
    uint8_t size_query_scratch;

    status = part->pxe_base_code->Mtftp(
            part->pxe_base_code,
            EFI_PXE_BASE_CODE_TFTP_GET_FILE_SIZE,
            &size_query_scratch,
            false,
            &file_size,
            NULL,
            ip,
            (uint8_t *)name,
            NULL,
            true);

    if (status) {
        return NULL;
    }

    struct file_handle *handle = ext_mem_alloc(sizeof(struct file_handle));

    uint64_t expected_size = file_size;

    handle->efi_part_handle = part->efi_handle;
    handle->size = expected_size;
    handle->is_memfile = true;

    handle->pxe = true;
    memcpy(handle->pxe_ip, ip->Addr, 4);
    handle->pxe_port = 69;

    size_t name_len = strlen(name);
    handle->path = ext_mem_alloc(1 + name_len + 1);
    handle->path[0] = '/';
    memcpy(&handle->path[1], name, name_len);
    handle->path_len = 1 + name_len + 1;

    handle->fd = ext_mem_alloc(handle->size);

    status = part->pxe_base_code->Mtftp(
            part->pxe_base_code,
            EFI_PXE_BASE_CODE_TFTP_READ_FILE,
            handle->fd,
            false,
            &file_size,
            NULL,
            ip,
            (uint8_t *)name,
            NULL,
            false);

    if (status || file_size != expected_size) {
        pmm_free(handle->fd, handle->size);
        pmm_free(handle->path, handle->path_len);
        pmm_free(handle, sizeof(struct file_handle));
        return NULL;
    }

    return handle;
}

#endif
