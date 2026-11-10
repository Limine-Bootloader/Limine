#include <stdint.h>
#include <lib/misc.h>
#include <lib/trace.h>
#include <lib/print.h>

#if defined(BIOS)

static const char *exception_names[] = {
    "Division",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "???",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "???",
    "x87",
    "Alignment check",
    "Machine check",
    "SIMD",
    "Virtualisation",
    "???",
    "???",
    "???",
    "???",
    "???",
    "???",
    "???",
    "???",
    "???",
    "Security"
};

void except(uint32_t exception, uint32_t error_code, uint32_t ebp, uint32_t eip) {
    (void)ebp;
    const char *exception_name = exception < SIZEOF_ARRAY(exception_names)
                                 ? exception_names[exception]
                                 : "Unknown";
    panic(false, "%s exception at %x. Error code: %x", exception_name, eip, error_code);
}

#endif
