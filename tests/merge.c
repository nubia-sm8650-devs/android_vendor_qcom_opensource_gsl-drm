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
int gsl_syncobj_create(struct gsl_syncobj** out, gsl_devhandle_t handle, gsl_ctxid_t id,
                       uint32_t ts);
int gsl_syncobj_clone(struct gsl_syncobj** out, struct gsl_syncobj* obj);
int gsl_syncobj_merge(struct gsl_syncobj** out, struct gsl_syncobj* a, struct gsl_syncobj* b);
struct gsl_syncsource;
int gsl_syncsource_create(struct gsl_syncsource** out);
void gsl_syncsource_destroy(struct gsl_syncsource* src);
int gsl_syncobj_create_from_source(struct gsl_syncobj** out, struct gsl_syncsource* src);
int gsl_syncobj_signal(struct gsl_syncobj* obj);

static struct gsl_syncobj* from_point(struct gsl_timeline_obj* tl, uint64_t point) {
    struct gsl_syncobj* obj = gsl_syncobj_new(1);

    assert(obj);
    obj->points[0] = (struct gsl_sync_point){.tl = gsl_tl_get(tl), .point = point};
    return obj;
}

static void check_points(const struct gsl_syncobj* obj, struct gsl_timeline_obj** tls,
                         const uint64_t* values, uint32_t count) {
    assert(obj->count == count);
    for (uint32_t i = 0; i < count; i++) {
        bool found = false;

        for (uint32_t j = 0; j < count; j++) {
            if (obj->points[j].tl != tls[i]) continue;
            assert(obj->points[j].point == values[i]);
            found = true;
        }
        assert(found);
    }
    for (uint32_t i = 1; i < count; i++)
        assert((uintptr_t)obj->points[i - 1].tl < (uintptr_t)obj->points[i].tl);
}

static void test_large_history(void) {
    struct gsl_timeline_obj* tls[5];
    struct gsl_syncobj *a = gsl_syncobj_new(15758), *b = gsl_syncobj_new(15758);
    struct gsl_syncobj *clone, *merged;
    uint64_t values[5] = {0};
    int refs[5];

    assert(a && b);
    for (unsigned int i = 0; i < 5; i++) {
        tls[i] = gsl_tl_create(&test_device);
        assert(tls[i]);
    }
    for (uint32_t i = 0; i < a->count + b->count; i++) {
        unsigned int index = i % 5;
        uint64_t value = i % 97 ? (uint64_t)(i * 1237 % 100000) + (UINT64_C(1) << 35) : 0;
        struct gsl_sync_point* point = i < a->count ? &a->points[i] : &b->points[i - a->count];

        *point = (struct gsl_sync_point){.tl = gsl_tl_get(tls[index]), .point = value};
        if (value > values[index]) values[index] = value;
    }
    assert(!gsl_syncobj_clone(&clone, a));
    struct gsl_sync_point first = a->points[0];
    for (unsigned int i = 0; i < 5; i++) refs[i] = tls[i]->refs;

    assert(!gsl_syncobj_merge(&merged, a, b));
    check_points(merged, tls, values, 5);
    assert(a->count == 15758 && b->count == 15758 && clone == a && a->refs == 2);
    assert(first.tl == a->points[0].tl && first.point == a->points[0].point);
    for (unsigned int i = 0; i < 5; i++) assert(tls[i]->refs == refs[i] + 1);

    gsl_syncobj_put(merged);
    for (unsigned int i = 0; i < 5; i++) assert(tls[i]->refs == refs[i]);
    gsl_syncobj_put(clone);
    gsl_syncobj_put(a);
    gsl_syncobj_put(b);
    for (unsigned int i = 0; i < 5; i++) {
        assert(tls[i]->refs == 1);
        gsl_tl_put(&test_device, tls[i]);
    }
}

static void test_incremental_history(void) {
    struct gsl_timeline_obj* tls[5];
    struct gsl_syncobj* history = NULL;
    uint64_t values[5] = {0};

    for (unsigned int i = 0; i < 5; i++) {
        tls[i] = gsl_tl_create(&test_device);
        assert(tls[i]);
    }
    for (uint32_t i = 0; i < 30000; i++) {
        unsigned int index = i % 5;
        struct gsl_syncobj* point = from_point(tls[index], i + 1);

        assert(!gsl_syncobj_merge_into(&history, point));
        gsl_syncobj_put(point);
        values[index] = i + 1;
        assert(history->count <= 5);
    }
    check_points(history, tls, values, 5);
    struct gsl_syncobj* self;
    assert(!gsl_syncobj_merge(&self, history, history));
    check_points(self, tls, values, 5);
    gsl_syncobj_put(self);
    for (unsigned int i = 0; i < 5; i++) assert(tls[i]->refs == 2);

    gsl_syncobj_put(history);
    for (unsigned int i = 0; i < 5; i++) {
        assert(tls[i]->refs == 1);
        gsl_tl_put(&test_device, tls[i]);
    }
}

static uint32_t wait_handles[2];
static uint64_t completed[2];

static int check_wait(const struct drm_syncobj_timeline_wait* req) {
    const uint32_t* handles = (void*)(uintptr_t)req->handles;
    const uint64_t* points = (void*)(uintptr_t)req->points;

    assert(req->count_handles == 2);
    assert(req->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL);
    for (unsigned int i = 0; i < 2; i++) {
        unsigned int index = handles[i] == wait_handles[0] ? 0 : 1;

        assert(handles[i] == wait_handles[index]);
        assert(points[i] == (index ? 5 : 9));
        if (completed[index] < points[i]) return ETIME;
    }
    return 0;
}

static void test_independent_waits(void) {
    struct gsl_timeline_obj *a = gsl_tl_create(&test_device), *b = gsl_tl_create(&test_device);
    struct gsl_syncobj *pa = from_point(a, 3), *pb = from_point(b, 5);
    struct gsl_syncobj *newer = from_point(a, 9), *merged;

    assert(!gsl_syncobj_merge(&merged, pa, pb));
    assert(!gsl_syncobj_merge_into(&merged, newer));
    assert(merged->count == 2 && pa->points[0].point == 3);
    wait_handles[0] = a->handle;
    wait_handles[1] = b->handle;
    completed[0] = completed[1] = 0;
    mock.check_wait = check_wait;
    assert(gsl_syncobj_wait_internal(&test_device, merged, 0) == GSL_FAILURE_TIMEOUT);
    completed[1] = 5;
    assert(gsl_syncobj_wait_internal(&test_device, merged, 0) == GSL_FAILURE_TIMEOUT);
    completed[0] = 9;
    assert(!gsl_syncobj_wait_internal(&test_device, merged, 0));
    unsigned int waits = mock.timeline_waits;
    assert(!gsl_syncobj_wait_internal(&test_device, merged, 0));
    assert(mock.timeline_waits == waits);
    mock.check_wait = NULL;

    gsl_syncobj_put(merged);
    gsl_syncobj_put(newer);
    gsl_syncobj_put(pb);
    gsl_syncobj_put(pa);
    assert(a->refs == 1 && b->refs == 1);
    gsl_tl_put(&test_device, a);
    gsl_tl_put(&test_device, b);
}

static void test_failures_and_fds(void) {
    struct gsl_timeline_obj* tl = gsl_tl_create(&test_device);
    struct gsl_syncobj *a = from_point(tl, 3), *b = from_point(tl, 9);
    struct gsl_syncobj *dst, *merged;
    int refs = tl->refs;

    for (unsigned int fail = 1; fail <= 2; fail++) {
        dst = gsl_syncobj_get(a);
        mock.track_allocations = true;
        mock.allocations = 0;
        mock.fail_allocation = fail;
        assert(gsl_syncobj_merge_into(&dst, b) == GSL_FAILURE_OUTOFMEM);
        mock.track_allocations = false;
        mock.fail_allocation = 0;
        assert(dst == a && tl->refs == refs && a->refs == 2 && b->refs == 1);
        gsl_syncobj_put(dst);
    }

    a->fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    b->fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    b->user = true;
    assert(a->fd >= 0 && b->fd >= 0);
    unsigned int merges = mock.fd_merges;
    assert(!gsl_syncobj_merge(&merged, a, b));
    assert(merged->count == 1 && merged->points[0].point == 9 && merged->user);
    assert(merged->fd >= 0 && merged->fd != a->fd && merged->fd != b->fd);
    assert(mock.fd_merges == merges + 1);
    gsl_syncobj_put(merged);

    dst = gsl_syncobj_get(a);
    mock.fd_merge_errno = EMFILE;
    assert(gsl_syncobj_merge_into(&dst, b) == GSL_FAILURE_SYSTEMERROR);
    assert(dst == a && tl->refs == refs && a->refs == 2 && b->refs == 1);
    mock.fd_merge_errno = 0;
    gsl_syncobj_put(dst);

    int saved_fd = b->fd;
    b->fd = -1;
    dst = gsl_syncobj_get(a);
    mock.fail_dup = true;
    assert(gsl_syncobj_merge_into(&dst, b) == GSL_FAILURE_SYSTEMERROR);
    assert(dst == a && tl->refs == refs && a->refs == 2 && b->refs == 1);
    mock.fail_dup = false;
    b->fd = saved_fd;
    gsl_syncobj_put(dst);

    int fd = dup(a->fd);
    assert(fd >= 0);
    close(fd);
    fd = dup(b->fd);
    assert(fd >= 0);
    close(fd);
    gsl_syncobj_put(a);
    gsl_syncobj_put(b);
    assert(tl->refs == 1);
    gsl_tl_put(&test_device, tl);

    struct gsl_syncobj overflow = {.refs = 1, .fd = -1, .count = UINT32_MAX};
    struct gsl_syncobj extra = {.refs = 1, .fd = -1, .count = 1};
    dst = &overflow;
    assert(gsl_syncobj_merge_into(&dst, &extra) == GSL_FAILURE_OUTOFMEM);
    assert(dst == &overflow);
}

static void test_sources_and_empty(void) {
    struct gsl_syncsource* source;
    struct gsl_syncobj *a, *b, *merged;

    assert(!gsl_syncsource_create(&source));
    assert(!gsl_syncobj_create_from_source(&a, source));
    assert(!gsl_syncobj_create_from_source(&b, source));
    assert(a->points[0].tl != b->points[0].tl);
    assert(!gsl_syncobj_merge(&merged, a, a));
    assert(merged->count == 1 && merged->user && merged->points[0].point == 1);
    gsl_syncobj_put(merged);

    assert(!gsl_syncobj_merge(&merged, a, b));
    assert(merged->count == 2 && merged->user);
    assert(!gsl_tl_signaled(a->points[0].tl, 1) && !gsl_tl_signaled(b->points[0].tl, 1));
    assert(!gsl_syncobj_signal(merged));
    assert(gsl_tl_signaled(a->points[0].tl, 1) && gsl_tl_signaled(b->points[0].tl, 1));
    gsl_syncobj_put(merged);
    gsl_syncobj_put(a);
    gsl_syncobj_put(b);
    gsl_syncsource_destroy(source);

    a = gsl_syncobj_new(0);
    b = gsl_syncobj_new(0);
    assert(a && b);
    assert(!gsl_syncobj_merge(&merged, a, b));
    assert(!merged->count && !merged->points && merged->fd == -1);
    gsl_syncobj_put(merged);
    gsl_syncobj_put(a);
    gsl_syncobj_put(b);
}

static void test_timestamp_wrap(void) {
    gsl_ctxid_t id = gsl_context_create(1, 0, GSL_CONTEXT_FLAG_CLIENT_GENERATED_TS, NULL);
    struct gsl_syncobj *old, *newer, *merged;
    uint32_t ts = UINT32_MAX - 1;

    assert(id != UINT32_MAX);
    assert(!gsl_context_marker(1, id, &ts, NULL, NULL));
    assert(!gsl_syncobj_create(&old, 1, id, ts));
    ts = 1;
    assert(!gsl_context_marker(1, id, &ts, NULL, NULL));
    assert(!gsl_syncobj_create(&newer, 1, id, ts));
    assert(old->points[0].point == 1 && newer->points[0].point == 4);
    assert(!gsl_syncobj_merge(&merged, old, newer));
    assert(merged->count == 1 && merged->points[0].point == 4);
    assert(old->points[0].point == 1);

    gsl_syncobj_put(merged);
    gsl_syncobj_put(newer);
    gsl_syncobj_put(old);
    assert(!gsl_context_destroy(1, id));
}

int main(void) {
    test_init();
    test_large_history();
    test_incremental_history();
    test_independent_waits();
    test_failures_and_fds();
    test_sources_and_empty();
    test_timestamp_wrap();
    test_fini();
    puts("sync merge tests passed");
    return 0;
}
