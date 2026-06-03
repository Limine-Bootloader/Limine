#ifndef LIB__ELSEWHERE_H__
#define LIB__ELSEWHERE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct elsewhere_range {
    uint64_t elsewhere;
    uint64_t target;
    uint64_t length;
};

bool elsewhere_append(
        bool allow_wraparound,
        struct elsewhere_range *ranges, uint64_t *ranges_count,
        uint64_t ranges_max,
        void *elsewhere, uint64_t *target, size_t t_length);

void elsewhere_reserve_target(uint64_t base, uint64_t length);

#endif
