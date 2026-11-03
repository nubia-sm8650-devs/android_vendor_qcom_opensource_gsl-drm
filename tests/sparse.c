/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "mock.h"

struct gsl_membind_region {
    uint64_t target_offset;
    const struct gsl_memdesc* phys;
    uint64_t child_offset;
    uint64_t length;
    uint64_t flags;
};

struct gsl_membind {
    const struct gsl_memdesc* virt;
    const struct gsl_membind_region* regions;
    uint64_t count;
};

gsl_ctxid_t gsl_context_create(gsl_devhandle_t handle, uint32_t type, uint32_t flags, void* extra);
int gsl_context_destroy(gsl_devhandle_t handle, gsl_ctxid_t id);
int gsl_command_insertfence(gsl_devhandle_t handle, gsl_ctxid_t id, struct gsl_syncobj* wait);
int gsl_memory_bind_sync(gsl_devhandle_t handle, gsl_ctxid_t id, const struct gsl_membind* binds,
                         uint64_t count, struct gsl_syncobj* wait,
                         const struct gsl_timeline_signal* signal, uint32_t* ts);

static bool contains(const struct drm_msm_syncobj* points, uint32_t count, uint32_t handle,
                     uint64_t point) {
    for (uint32_t i = 0; i < count; i++)
        if (points[i].handle == handle && points[i].point == point) return true;
    return false;
}

int main(void) {
    test_init();
    gsl_ctxid_t id = gsl_context_create(1, 0, 0, NULL);
    uint32_t ts = 0;

    assert(id != UINT32_MAX);
    assert(!gsl_context_marker(1, id, &ts, NULL, NULL) && ts == 1);
    struct gsl_context* ctx = gsl_context_get(id);
    struct gsl_timeline_obj* tl = gsl_tl_create(&test_device);
    struct gsl_sync_point dependency = {.tl = tl, .point = 1};
    struct gsl_syncobj wait = {.refs = 1, .fd = -1, .count = 1, .points = &dependency};
    struct gsl_bo virt = {.type = GSL_BO_VIRT, .gen = 1, .iova = 0x100000000ull};
    struct gsl_bo phys = {.type = GSL_BO_PHYS, .gen = 1, .handle = 50};
    struct gsl_memdesc vm = {.priv = &virt}, pm = {.priv = &phys};
    struct gsl_membind_region region = {.phys = &pm, .length = 4096};
    struct gsl_membind bind = {.virt = &vm, .regions = &region, .count = 1};

    assert(!gsl_command_insertfence(1, id, &wait));
    assert(!gsl_memory_bind_sync(1, id, &bind, 1, &wait, NULL, &ts));
    assert(ts == 2 && mock.bind_calls == 1 && mock.bind_nin == 3);
    assert(ctx->sparse_queue && mock.bind_queue == ctx->sparse_queue);
    assert(ctx->sparse_queue != test_device.bind_queue && !test_device.bind_point);
    assert(contains(mock.bind_in, mock.bind_nin, ctx->tl->handle, 1));
    assert(contains(mock.bind_in, mock.bind_nin, tl->handle, 1));
    assert(mock.submit_nin == 4 && mock.submit_nout == 1);
    assert(contains(mock.submit_in, mock.submit_nin, ctx->sparse_syncobj, 1));
    assert(contains(mock.submit_out, mock.submit_nout, ctx->tl->handle, 2));
    assert(!ctx->pending_wait);

    assert(!gsl_context_marker(1, id, &ts, NULL, NULL) && ts == 3);
    assert(!contains(mock.submit_in, mock.submit_nin, ctx->sparse_syncobj, 1));

    uint64_t value = 9;
    struct gsl_timeline_signal signal = {.timelines = &tl, .values = &value, .count = 1};
    unsigned int submits = mock.submit_calls;
    assert(!gsl_memory_bind_sync(1, id, &bind, 1, NULL, &signal, &ts));
    assert(ts == 4 && mock.submit_calls == submits + 1);
    assert(contains(mock.bind_in, mock.bind_nin, ctx->tl->handle, 3));
    assert(mock.submit_nout == 2);
    assert(contains(mock.submit_out, mock.submit_nout, tl->handle, 9));

    unsigned int binds = mock.bind_calls;
    assert(gsl_memory_bind_sync(1, UINT32_MAX, &bind, 1, NULL, NULL, &ts) == GSL_FAILURE_BADPARAM);
    assert(ts == 4 && mock.bind_calls == binds);

    mock.bind_errno = EINVAL;
    assert(!gsl_command_insertfence(1, id, &wait));
    assert(gsl_memory_bind_sync(1, id, &bind, 1, NULL, NULL, &ts) == GSL_FAILURE_BADPARAM);
    assert(ts == 4 && ctx->pending_wait);
    mock.bind_errno = 0;

    binds = mock.bind_calls;
    mock.submit_timeouts = 1;
    assert(!gsl_memory_bind_sync(1, id, &bind, 1, NULL, NULL, &ts) && ts == 5);
    gsl_contexts_stop();
    assert(mock.bind_calls == binds + 1);
    assert(contains(mock.submit_in, mock.submit_nin, ctx->sparse_syncobj, ctx->sparse_point));
    assert(contains(mock.submit_out, mock.submit_nout, ctx->tl->handle, 5));

    gsl_context_put(ctx);
    gsl_tl_put(&test_device, tl);
    assert(!gsl_context_destroy(1, id));

    id = gsl_context_create(1, 0, GSL_CONTEXT_FLAG_CLIENT_GENERATED_TS, NULL);
    assert(id != UINT32_MAX);
    ts = 17;
    assert(!gsl_memory_bind_sync(1, id, &bind, 1, NULL, NULL, &ts));
    assert(ts == 17 && mock.bind_nin == 0);
    assert(gsl_memory_bind_sync(1, id, &bind, 1, NULL, NULL, &ts) == GSL_FAILURE_BADPARAM);
    ts = 20;
    assert(!gsl_memory_bind_sync(1, id, &bind, 1, NULL, NULL, &ts));
    assert(ts == 20 && mock.bind_nin == 1 && mock.bind_in[0].point == 1);

    gsl_ctxid_t other = gsl_context_create(1, 0, 0, NULL);
    ctx = gsl_context_get(id);
    test_device.bind_point = 5;
    ts = 21;
    assert(!gsl_memory_bind_sync(1, id, &bind, 1, NULL, NULL, &ts));
    assert(contains(mock.bind_in, mock.bind_nin, test_device.bind_syncobj, 5));
    assert(test_device.bind_point == 5);
    assert(!gsl_context_marker(1, other, &ts, NULL, NULL));
    assert(mock.submit_nin == 1);
    assert(contains(mock.submit_in, mock.submit_nin, test_device.bind_syncobj, 5));
    gsl_context_put(ctx);
    assert(!gsl_context_destroy(1, other));
    assert(!gsl_context_destroy(1, id));
    test_fini();
    puts("sparse bind tests passed");
    return 0;
}
