/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "gsl_priv.h"

struct gsl_vma_range {
    uint64_t start;
    uint64_t end;
    struct gsl_vma_range* next;
};

int gsl_vma_init(struct gsl_vma* vma, uint64_t start, uint64_t size) {
    struct gsl_vma_range* range = malloc(sizeof(*range));

    if (!range) return GSL_FAILURE_OUTOFMEM;

    range->start = start;
    range->end = start + size;
    range->next = NULL;
    vma->free = range;
    pthread_mutex_init(&vma->lock, NULL);

    return GSL_SUCCESS;
}

void gsl_vma_fini(struct gsl_vma* vma) {
    struct gsl_vma_range* range = vma->free;

    while (range) {
        struct gsl_vma_range* next = range->next;

        free(range);
        range = next;
    }
    vma->free = NULL;
    pthread_mutex_destroy(&vma->lock);
}

uint64_t gsl_vma_alloc(struct gsl_vma* vma, uint64_t size, uint32_t align_shift) {
    uint64_t align = 1ull << align_shift;
    struct gsl_vma_range **link, *range;
    uint64_t addr = 0;

    pthread_mutex_lock(&vma->lock);
    for (link = &vma->free; (range = *link); link = &range->next) {
        uint64_t start = (range->start + align - 1) & ~(align - 1);

        if (start < range->start || start + size > range->end || start + size < start) continue;

        addr = start;
        if (start > range->start) {
            struct gsl_vma_range* head = malloc(sizeof(*head));

            if (!head) {
                addr = 0;
                break;
            }
            head->start = range->start;
            head->end = start;
            head->next = range;
            *link = head;
            link = &head->next;
        }

        range->start = start + size;
        if (range->start == range->end) {
            *link = range->next;
            free(range);
        }
        break;
    }
    pthread_mutex_unlock(&vma->lock);

    return addr;
}

void gsl_vma_free(struct gsl_vma* vma, uint64_t addr, uint64_t size) {
    struct gsl_vma_range **link, *range, *prev = NULL, *node;
    uint64_t end = addr + size;

    pthread_mutex_lock(&vma->lock);
    for (link = &vma->free; (range = *link) && range->start < addr; link = &range->next)
        prev = range;

    if (prev && prev->end == addr) {
        prev->end = end;
        if (range && range->start == end) {
            prev->end = range->end;
            prev->next = range->next;
            free(range);
        }
    } else if (range && range->start == end) {
        range->start = addr;
    } else {
        node = malloc(sizeof(*node));
        if (node) {
            node->start = addr;
            node->end = end;
            node->next = range;
            *link = node;
        } else {
            GSL_ERR("leaking GPU VA 0x%llx size 0x%llx", (unsigned long long)addr,
                    (unsigned long long)size);
        }
    }
    pthread_mutex_unlock(&vma->lock);
}
