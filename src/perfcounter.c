/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "gsl_priv.h"

int gsl_perfcounter_select(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t count,
                           const uint32_t* groups, const uint32_t* countables, void* reserved,
                           uint32_t* offsets) {
    if (offsets) memset(offsets, 0, (size_t)count * sizeof(*offsets));

    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_perfcounter_deselect(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t ts, int32_t count,
                             uint32_t* groups, uint32_t* countables) {
    for (int32_t i = 0; i < count; i++) {
        if (groups) groups[i] = 0;
        if (countables) countables[i] = 0;
    }

    return GSL_SUCCESS;
}

int gsl_perfcounter_query_selections(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t group,
                                     uint32_t count, uint32_t* countables, int32_t* max) {
    if (max) *max = -1;

    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_perfcounter_read(void) {
    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_perfcounters_read(void) {
    return GSL_FAILURE_NOTSUPPORTED;
}
