/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>

#include "mock.h"

gsl_ctxid_t gsl_context_create(gsl_devhandle_t handle, uint32_t type, uint32_t flags, void* extra);
int gsl_context_destroy(gsl_devhandle_t handle, gsl_ctxid_t id);
int gsl_command_readtimestamp(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t type, uint32_t* out);
int gsl_command_checktimestamp(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t ts, uint32_t type);
int gsl_command_waittimestamp(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t ts,
                              uint32_t timeout_ms);

int main(void) {
    test_init();
    gsl_ctxid_t id = gsl_context_create(1, 0, 0, NULL);
    uint32_t ts = 0, out;

    assert(id != UINT32_MAX);
    for (int i = 0; i < 3; i++) assert(!gsl_context_marker(1, id, &ts, NULL, NULL));
    assert(ts == 3);

    mock.completed = 2;
    unsigned int queries = mock.queries, waits;
    assert(!gsl_command_readtimestamp(1, id, GSL_TIMESTAMP_RETIRED, &out) && out == 2);
    assert(mock.queries == queries + 1);
    assert(gsl_command_checktimestamp(1, id, 1, GSL_TIMESTAMP_RETIRED) == 1);
    assert(gsl_command_checktimestamp(1, id, 2, GSL_TIMESTAMP_CONSUMED) == 1);
    assert(mock.queries == queries + 1);

    waits = mock.timeline_waits;
    assert(!gsl_command_waittimestamp(1, id, 2, 1000));
    assert(mock.timeline_waits == waits);

    assert(gsl_command_checktimestamp(1, id, 3, GSL_TIMESTAMP_RETIRED) == 0);
    assert(mock.queries == queries + 2);
    mock.completed = 3;
    assert(gsl_command_checktimestamp(1, id, 3, GSL_TIMESTAMP_RETIRED) == 1);
    assert(gsl_command_checktimestamp(1, id, 3, GSL_TIMESTAMP_RETIRED) == 1);
    assert(mock.queries == queries + 3);

    assert(!gsl_context_destroy(1, id));
    test_fini();
    puts("timestamp tests passed");
    return 0;
}
