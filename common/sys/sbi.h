#ifndef SYS__SBI_H
#define SYS__SBI_H

#include <stddef.h>
#include <stdint.h>

struct sbiret {
    long error;
    long value;
};

#define SBI_SUCCESS                 ((long)0)
#define SBI_ERR_FAILED              ((long)-1)
#define SBI_ERR_NOT_SUPPORTED       ((long)-2)
#define SBI_ERR_INVALID_PARAM       ((long)-3)
#define SBI_ERR_DENIED              ((long)-4)
#define SBI_ERR_INVALID_ADDRESS     ((long)-5)
#define SBI_ERR_ALREADY_AVAILABLE   ((long)-6)
#define SBI_ERR_ALREADY_STARTED     ((long)-7)
#define SBI_ERR_ALREADY_STOPPED     ((long)-8)

extern struct sbiret sbicall(int eid, int fid, ...);

#define SBI_EID_HSM         0x48534d

static inline struct sbiret sbi_hart_start(unsigned long hartid,
                                           unsigned long start_addr,
                                           unsigned long opaque) {
    return sbicall(SBI_EID_HSM, 0, hartid, start_addr, opaque);
}

#endif
