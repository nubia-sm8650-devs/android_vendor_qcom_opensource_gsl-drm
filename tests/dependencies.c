/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "mock.h"

gsl_ctxid_t gsl_context_create(gsl_devhandle_t handle, uint32_t type, uint32_t flags, void* extra);
int gsl_context_destroy(gsl_devhandle_t handle, gsl_ctxid_t id);

static const struct gsl_syncobj* expected;

static void check_submit(const struct drm_msm_gem_submit* req) {
    const struct drm_msm_syncobj* in = (void*)(uintptr_t)req->in_syncobjs;

    assert(req->nr_in_syncobjs == expected->count + 1);
    assert(in[0].handle == test_device.bind_syncobj && in[0].point == 7);
    for (uint32_t i = 0; i < expected->count; i++) {
        assert(in[i + 1].handle == expected->points[i].tl->handle);
        assert(in[i + 1].point == expected->points[i].point);
        assert(!in[i + 1].flags);
    }
}

static void test_submission(void) {
    struct gsl_timeline_obj* tls[5];
    struct gsl_syncobj* history = gsl_syncobj_new(31516);
    gsl_ctxid_t id = gsl_context_create(1, 0, 0, NULL);
    uint32_t ts = 0;

    assert(history && id != UINT32_MAX);
    for (unsigned int i = 0; i < 5; i++) {
        tls[i] = gsl_tl_create(&test_device);
        assert(tls[i]);
        gsl_tl_mark_available(tls[i], UINT64_MAX);
    }
    for (uint32_t i = 0; i < history->count; i++) {
        history->points[i].tl = gsl_tl_get(tls[i % 5]);
        history->points[i].point = (uint64_t)i + 1;
    }

    struct gsl_syncobj small = {.refs = 1, .fd = -1, .count = 1, .points = history->points};
    test_device.bind_point = 7;
    mock.check_submit = check_submit;
    expected = &small;
    mock.track_allocations = true;
    assert(!gsl_context_marker(1, id, &ts, &small, NULL));
    unsigned int allocations = mock.allocations;
    assert(!mock.reallocations);

    expected = history;
    mock.allocations = 0;
    assert(!gsl_context_marker(1, id, &ts, history, NULL));
    assert(mock.allocations == allocations && !mock.reallocations);
    mock.track_allocations = false;
    mock.check_submit = NULL;

    gsl_syncobj_put(history);
    for (unsigned int i = 0; i < 5; i++) gsl_tl_put(&test_device, tls[i]);
    assert(!gsl_context_destroy(1, id));
}

static void test_storage(void) {
    struct gsl_sync_deps deps = {0};

    mock.track_allocations = true;
    mock.allocations = mock.reallocations = 0;
    assert(!gsl_sync_deps_init(&deps, 31516, 0));
    for (uint32_t i = 0; i < 31516; i++)
        assert(!gsl_sync_deps_add_point(&deps, i % 5 + 1, (uint64_t)i + 1));
    assert(mock.allocations == 1 && !mock.reallocations && deps.count == 31516);
    assert(gsl_sync_deps_add_point(&deps, 1, 1) == GSL_FAILURE_OUTOFMEM);
    gsl_sync_deps_fini(&test_device, &deps);

    for (unsigned int fail = 1; fail <= 2; fail++) {
        mock.allocations = 0;
        mock.fail_allocation = fail;
        assert(gsl_sync_deps_init(&deps, 2, 2) == GSL_FAILURE_OUTOFMEM);
        assert(!deps.in && !deps.temps && !deps.capacity && !deps.temp_capacity);
        gsl_sync_deps_fini(&test_device, &deps);
    }
    mock.fail_allocation = 0;
    mock.allocations = 0;
    assert(gsl_sync_deps_init(&deps, (uint64_t)UINT32_MAX + 1, 0) == GSL_FAILURE_OUTOFMEM);
    assert(gsl_sync_deps_init(&deps, 0, (uint64_t)UINT32_MAX + 1) == GSL_FAILURE_OUTOFMEM);
    assert(!mock.allocations);
    mock.track_allocations = false;
}

static void test_imports(void) {
    struct gsl_sync_deps deps = {0};
    struct gsl_syncobj obj = {.refs = 1, .fd = open("/dev/null", O_RDONLY | O_CLOEXEC)};
    unsigned int destroyed = mock.syncobj_destroys;

    assert(obj.fd >= 0);
    assert(!gsl_sync_deps_init(&deps, 2, 2));
    assert(!gsl_sync_deps_add(&test_device, &deps, &obj));
    assert(deps.count == 1 && deps.ntemps == 1 && !deps.in[0].point);
    mock.fd_import_errno = EIO;
    assert(gsl_sync_deps_add(&test_device, &deps, &obj) == GSL_FAILURE_SYSTEMERROR);
    assert(deps.count == 1 && deps.ntemps == 1);
    assert(mock.syncobj_destroys == destroyed + 1);
    mock.fd_import_errno = 0;
    assert(!gsl_sync_deps_add(&test_device, &deps, &obj));
    assert(deps.count == 2 && deps.ntemps == 2);
    unsigned int imports = mock.fd_imports;
    assert(gsl_sync_deps_add(&test_device, &deps, &obj) == GSL_FAILURE_OUTOFMEM);
    assert(mock.fd_imports == imports);
    gsl_sync_deps_fini(&test_device, &deps);
    assert(mock.syncobj_destroys == destroyed + 3);
    close(obj.fd);
}

int main(void) {
    test_init();
    test_submission();
    test_storage();
    test_imports();
    test_fini();
    puts("dependency allocation tests passed");
    return 0;
}
