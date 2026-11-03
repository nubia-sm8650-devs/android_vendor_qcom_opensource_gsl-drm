/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>

#include "mock.h"

int gsl_syncobj_wait_multiple(struct gsl_syncobj** objs, int* results, uint64_t count,
                              uint32_t timeout_ms);

int main(void) {
    test_init();
    struct gsl_timeline_obj a = {.refs = 1, .handle = 7}, b = {.refs = 1, .handle = 8};
    struct gsl_sync_point pa = {.tl = &a, .point = 1}, pb = {.tl = &b, .point = 2};
    struct gsl_syncobj oa = {.refs = 1, .fd = -1, .count = 1, .points = &pa};
    struct gsl_syncobj ob = {.refs = 1, .fd = -1, .count = 1, .points = &pb};
    struct gsl_syncobj* objs[] = {&oa, &ob};
    int results[2];

    mock.fence_unsignaled = true;
    assert(!gsl_syncobj_wait_multiple(objs, results, 2, 1000));
    assert(!results[0] && !results[1]);
    assert(mock.poll_waits == 2 && mock.block_waits == 1 && !mock.sleeps);

    pa.point = 3;
    pb.point = 4;
    mock.fence_unsignaled = true;
    mock.block_timeouts = 1;
    mock.poll_waits = mock.block_waits = 0;
    assert(gsl_syncobj_wait_multiple(objs, results, 2, 1000) == GSL_FAILURE_TIMEOUT);
    assert(results[0] == GSL_FAILURE_TIMEOUT && results[1] == GSL_FAILURE_TIMEOUT);
    assert(mock.poll_waits == 2 && mock.block_waits == 1 && !mock.sleeps);

    mock.poll_waits = mock.block_waits = 0;
    assert(gsl_syncobj_wait_multiple(objs, results, 2, 0) == GSL_FAILURE_TIMEOUT);
    assert(mock.poll_waits == 2 && !mock.block_waits);

    test_fini();
    puts("wait tests passed");
    return 0;
}
