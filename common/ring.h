/* SPDX-License-Identifier: MPL-2.0 */
/* A single-producer, single-consumer ring of fixed-size records: lock-free between exactly two threads (the
 * real-time block and the network loop). Capacity is a power of two; head and tail run free and wrap. */
#ifndef MR_RING_H
#define MR_RING_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    uint32_t esz, cap;   /* record size in bytes, records */
    uint32_t head, tail; /* head: the producer's, tail: the consumer's */
} mr_ring_t;

static inline int mr_ring_init(mr_ring_t *r, uint32_t esz, uint32_t cap_pow2)
{
    r->buf = calloc(cap_pow2, esz);
    r->esz = esz;
    r->cap = cap_pow2;
    r->head = r->tail = 0;
    return r->buf ? 0 : -1;
}

static inline uint32_t mr_ring_count(const mr_ring_t *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

static inline uint32_t mr_ring_space(const mr_ring_t *r) { return r->cap - mr_ring_count(r); }

/* up to n records in; returns how many went */
static inline uint32_t mr_ring_push(mr_ring_t *r, const void *src, uint32_t n)
{
    const uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    const uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    const uint32_t space = r->cap - (head - tail);
    const uint8_t *s = src;
    uint32_t i;
    if (n > space)
        n = space;
    for (i = 0; i < n;) {   /* at most two runs: to the end of the buffer, then from its start */
        const uint32_t at = (head + i) & (r->cap - 1);
        uint32_t run = r->cap - at;
        if (run > n - i)
            run = n - i;
        memcpy(r->buf + (size_t)at * r->esz, s + (size_t)i * r->esz, (size_t)run * r->esz);
        i += run;
    }
    __atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
    return n;
}

/* up to n records out (dst NULL: discard them); returns how many came */
static inline uint32_t mr_ring_pop(mr_ring_t *r, void *dst, uint32_t n)
{
    const uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    const uint32_t avail = head - tail;
    uint8_t *d = dst;
    uint32_t i;
    if (n > avail)
        n = avail;
    for (i = 0; d && i < n;) {
        const uint32_t at = (tail + i) & (r->cap - 1);
        uint32_t run = r->cap - at;
        if (run > n - i)
            run = n - i;
        memcpy(d + (size_t)i * r->esz, r->buf + (size_t)at * r->esz, (size_t)run * r->esz);
        i += run;
    }
    __atomic_store_n(&r->tail, tail + n, __ATOMIC_RELEASE);
    return n;
}

/* the oldest record, not taken (NULL when empty) */
static inline const void *mr_ring_peek(const mr_ring_t *r)
{
    const uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    if (__atomic_load_n(&r->head, __ATOMIC_ACQUIRE) == tail)
        return NULL;
    return r->buf + (size_t)(tail & (r->cap - 1)) * r->esz;
}

#endif
