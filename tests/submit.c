/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "mock.h"

gsl_ctxid_t gsl_context_create(gsl_devhandle_t handle, uint32_t type, uint32_t flags, void* extra);
int gsl_context_destroy(gsl_devhandle_t handle, gsl_ctxid_t id);
int gsl_command_insertfence(gsl_devhandle_t handle, gsl_ctxid_t id, struct gsl_syncobj* wait);
int gsl_timeline_signal(gsl_devhandle_t handle, struct gsl_timeline_obj* tl, uint64_t value);
int gsl_syncobj_wait(struct gsl_syncobj* obj, uint32_t timeout_ms);

int main(void) {
    test_init();
    gsl_ctxid_t id = gsl_context_create(1, 0, 0, NULL);
    uint32_t ts = 0, waits;

    assert(id != UINT32_MAX);
    assert(!gsl_context_marker(1, id, &ts, NULL, NULL) && ts == 1);

    struct gsl_context* ctx = gsl_context_get(id);
    struct gsl_sync_point own = {.tl = ctx->tl, .point = 1};
    struct gsl_syncobj own_obj = {.refs = 1, .fd = -1, .count = 1, .points = &own};
    waits = mock.timeline_waits;
    assert(!gsl_context_marker(1, id, &ts, &own_obj, NULL) && ts == 2);
    assert(mock.timeline_waits == waits && mock.submit_nin == 1);

    struct gsl_timeline_obj* ext = gsl_tl_create(&test_device);
    struct gsl_sync_point ext_point = {.tl = ext, .point = 1};
    struct gsl_syncobj ext_obj = {.refs = 1, .fd = -1, .count = 1, .points = &ext_point};
    assert(!gsl_context_marker(1, id, &ts, &ext_obj, NULL));
    assert(mock.timeline_waits == waits + 1);
    assert(!gsl_timeline_signal(1, ext, 1));
    assert(!gsl_context_marker(1, id, &ts, &ext_obj, NULL));
    assert(mock.timeline_waits == waits + 1);

    struct gsl_syncobj fd_obj = {.refs = 2, .fd = open("/dev/null", O_RDONLY | O_CLOEXEC)};
    struct gsl_syncobj fd_obj2 = {.refs = 1, .fd = open("/dev/null", O_RDONLY | O_CLOEXEC)};
    unsigned int imports = mock.fd_imports;
    assert(fd_obj.fd >= 0 && fd_obj2.fd >= 0);
    assert(!gsl_context_marker(1, id, &ts, &fd_obj, NULL));
    assert((mock.submit_flags & MSM_SUBMIT_FENCE_FD_IN) && mock.submit_fence_fd == fd_obj.fd);
    assert(mock.fd_imports == imports && mock.timeline_waits == waits + 1);

    assert(!gsl_command_insertfence(1, id, &fd_obj));
    assert(!gsl_context_marker(1, id, &ts, &fd_obj2, NULL));
    assert(!(mock.submit_flags & MSM_SUBMIT_FENCE_FD_IN) && mock.submit_nin == 2);
    assert(mock.fd_imports == imports + 2 && mock.timeline_waits == waits + 1);

    unsigned int calls = mock.ioctl_calls;
    assert(!gsl_syncobj_wait(&fd_obj2, 0));
    assert(mock.ioctl_calls == calls);

    close(fd_obj.fd);
    close(fd_obj2.fd);
    gsl_tl_put(&test_device, ext);
    gsl_context_put(ctx);
    assert(!gsl_context_destroy(1, id));
    test_fini();
    puts("submit tests passed");
    return 0;
}
