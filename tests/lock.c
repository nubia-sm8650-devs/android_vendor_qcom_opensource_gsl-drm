/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mock.h"

gsl_ctxid_t gsl_context_create(gsl_devhandle_t handle, uint32_t type, uint32_t flags, void* extra);
int gsl_context_destroy(gsl_devhandle_t handle, gsl_ctxid_t id);
int gsl_command_readtimestamp(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t type, uint32_t* out);

static gsl_ctxid_t id;
static uint32_t snapshot_count, queued_ts;

static void* reader(void* arg) {
    struct gsl_sync_point* points;

    assert(!gsl_context_snapshot(&points, &snapshot_count));
    for (uint32_t i = 0; i < snapshot_count; i++) gsl_tl_put(NULL, points[i].tl);
    free(points);
    assert(!gsl_command_readtimestamp(1, id, GSL_TIMESTAMP_QUEUED, &queued_ts));

    return NULL;
}

static void during_submit(void) {
    struct timespec deadline;
    pthread_t thread;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    assert(!pthread_create(&thread, NULL, reader, NULL));
    assert(!pthread_timedjoin_np(thread, NULL, &deadline));
}

int main(void) {
    test_init();
    uint32_t ts = 0;

    id = gsl_context_create(1, 0, 0, NULL);
    assert(id != UINT32_MAX);
    assert(!gsl_context_marker(1, id, &ts, NULL, NULL) && ts == 1);

    mock.on_submit = during_submit;
    assert(!gsl_context_marker(1, id, &ts, NULL, NULL) && ts == 2);
    assert(!mock.on_submit && snapshot_count == 1 && queued_ts == 1);

    assert(!gsl_context_destroy(1, id));
    test_fini();
    puts("lock tests passed");
    return 0;
}
