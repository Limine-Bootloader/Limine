#include <stdint.h>
#include "common.h"
#include "tpm.h"

#define TPM_ALG_SHA256 0x000b

static uint32_t le32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) {
    return p[0] | (uint16_t)(p[1] << 8);
}

void tpm_dump_log(const void *log, size_t size) {
    const uint8_t *p = log, *end = p + size;
    // The first event is in the SHA-1 format and carries the Spec ID event
    // naming the digest algorithms and sizes every later event uses.
    if (size < 32) {
        check(false, "tpm_event_log.header", "log of %lu bytes", (uint64_t)size);
        return;
    }
    uint32_t first_size = le32(p + 28);
    const uint8_t *spec = p + 32;
    if (spec + first_size > end || first_size < 28 || memcmp(spec, "Spec ID Event03", 16) != 0) {
        check(false, "tpm_event_log.spec_id", "no Spec ID Event03 header");
        return;
    }
    uint32_t nalgs = le32(spec + 24);
    uint16_t alg_ids[8], alg_sizes[8];
    if (nalgs > 8 || 28 + nalgs * 4 > first_size) {
        check(false, "tpm_event_log.algorithms", "%u algorithms", nalgs);
        return;
    }
    for (uint32_t i = 0; i < nalgs; i++) {
        alg_ids[i] = le16(spec + 28 + i * 4);
        alg_sizes[i] = le16(spec + 30 + i * 4);
    }
    p = spec + first_size;
    unsigned n = 0;
    while (p + 12 <= end) {
        uint32_t pcr = le32(p);
        uint32_t type = le32(p + 4);
        uint32_t count = le32(p + 8);
        const uint8_t *q = p + 12;
        const uint8_t *sha256 = NULL;
        for (uint32_t i = 0; i < count; i++) {
            if (q + 2 > end) {
                return;
            }
            uint16_t id = le16(q);
            uint16_t dsize = 0;
            for (uint32_t a = 0; a < nalgs; a++) {
                if (alg_ids[a] == id) {
                    dsize = alg_sizes[a];
                }
            }
            if (dsize == 0) {
                check(false, "tpm_event_log.digest", "unknown algorithm %x", id);
                return;
            }
            if (id == TPM_ALG_SHA256) {
                sha256 = q + 2;
            }
            q += 2 + dsize;
        }
        if (q + 4 > end) {
            return;
        }
        uint32_t esize = le32(q);
        const uint8_t *data = q + 4;
        if (data + esize > end) {
            return;
        }
        kprintf("K tpm.event.%u=%u %08x ", n, pcr, type);
        if (sha256 != NULL) {
            out_hex(sha256, 32);
        } else {
            out_str("-");
        }
        out_char(' ');
        out_escaped((const char *)data, esize < 256 ? esize : 256);
        out_char('\n');
        n++;
        p = data + esize;
    }
    fact("tpm.event_count", "%u", n);
}
