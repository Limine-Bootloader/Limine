#ifndef LIB__TRUEOS_FIRMWARE_BRIDGE_H__
#define LIB__TRUEOS_FIRMWARE_BRIDGE_H__

#include <stdint.h>

#define TRUEOS_FW_BRIDGE_OFF_FIRMWARE_CR3       0
#define TRUEOS_FW_BRIDGE_OFF_FIRMWARE_STACK_TOP 8
#define TRUEOS_FW_BRIDGE_OFF_TARGET             16
#define TRUEOS_FW_BRIDGE_OFF_ARG0               24
#define TRUEOS_FW_BRIDGE_OFF_ARG1               32
#define TRUEOS_FW_BRIDGE_OFF_ARG2               40
#define TRUEOS_FW_BRIDGE_OFF_ARG3               48
#define TRUEOS_FW_BRIDGE_OFF_RESULT             56
#define TRUEOS_FW_BRIDGE_OFF_CALLER_CR3         64
#define TRUEOS_FW_BRIDGE_OFF_CALLER_RSP         72
#define TRUEOS_FW_BRIDGE_OFF_CALLER_CR4         80
#define TRUEOS_FW_BRIDGE_OFF_RESERVED           88
#define TRUEOS_FW_BRIDGE_OFF_CRC_OUTPUT         96
#define TRUEOS_FW_BRIDGE_OFF_PAYLOAD_LEN        100
#define TRUEOS_FW_BRIDGE_OFF_PAYLOAD            104
#define TRUEOS_FW_BRIDGE_PAYLOAD_BYTES          64
#define TRUEOS_FW_BRIDGE_CONTROL_BYTES          168

#ifndef __ASSEMBLER__

struct trueos_firmware_bridge_control {
    uint64_t firmware_cr3;
    uint64_t firmware_stack_top;
    uint64_t target;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t result;
    uint64_t caller_cr3;
    uint64_t caller_rsp;
    uint64_t caller_cr4;
    uint64_t reserved;
    uint32_t crc_output;
    uint32_t payload_len;
    uint8_t payload[TRUEOS_FW_BRIDGE_PAYLOAD_BYTES];
};

#if defined (__x86_64__) && defined (UEFI)
extern uint8_t trueos_firmware_bridge_entry[];
extern uint8_t trueos_firmware_bridge_end[];
#endif

#endif

#endif
