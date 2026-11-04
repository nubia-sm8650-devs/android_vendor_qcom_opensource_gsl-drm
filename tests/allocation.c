/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>

#include "mock.h"

void* __real_malloc(size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* ptr, size_t size);

static bool fail_allocation(bool reallocating) {
    if (!mock.track_allocations) return false;

    mock.allocations++;
    if (reallocating) mock.reallocations++;
    if (mock.fail_allocation != mock.allocations) return false;

    errno = ENOMEM;
    return true;
}

void* __wrap_malloc(size_t size) {
    return fail_allocation(false) ? NULL : __real_malloc(size);
}

void* __wrap_calloc(size_t count, size_t size) {
    return fail_allocation(false) ? NULL : __real_calloc(count, size);
}

void* __wrap_realloc(void* ptr, size_t size) {
    return fail_allocation(true) ? NULL : __real_realloc(ptr, size);
}
