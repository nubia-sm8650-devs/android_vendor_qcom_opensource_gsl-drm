/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "mock.h"

gsl_ctxid_t gsl_context_create(gsl_devhandle_t handle, uint32_t type, uint32_t flags, void* extra);
int gsl_context_destroy(gsl_devhandle_t handle, gsl_ctxid_t id);

int main(void) {
    test_init();
    gsl_ctxid_t id = gsl_context_create(1, 0, 0, NULL);
    gsl_ctxid_t other = gsl_context_create(1, 0, 0, NULL);
    uint32_t ts = 0;

    assert(id != UINT32_MAX && other != UINT32_MAX);
    assert(!gsl_context_marker(1, id, &ts, NULL, NULL));
    assert(!gsl_context_marker(1, other, &ts, NULL, NULL));

    for (unsigned int i = 0; i < 128; i++) {
        struct gsl_bo* bo = calloc(1, sizeof(*bo));

        assert(bo);
        bo->gen = test_device.gen;
        bo->dmabuf_fd = -1;
        bo->va_size = 4096;
        bo->iova = gsl_vma_alloc(&test_device.vma, 4096, 12);
        assert(bo->iova);
        gsl_bo_release(&test_device, bo, NULL);
    }

    assert(mock.queries == 127 && mock.queried_handles == 254);
    assert(!mock.bind_calls);

    mock.completed = ts;
    mock.query_errno = EIO;
    gsl_reap_deferred(&test_device);
    assert(!mock.bind_calls);
    mock.query_errno = 0;
    gsl_reap_deferred(&test_device);
    assert(mock.bind_calls == 64);
    gsl_reap_deferred(&test_device);
    assert(mock.bind_calls == 128);
    unsigned int queries = mock.queries;
    gsl_reap_deferred(&test_device);
    assert(mock.queries == queries);

    assert(!gsl_context_destroy(1, id));
    assert(!gsl_context_destroy(1, other));
    test_fini();
    puts("deferred free tests passed");
    return 0;
}
