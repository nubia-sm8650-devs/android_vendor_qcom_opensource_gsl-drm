/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "gsl_priv.h"

struct gsl_mock {
    bool fail_dup;
    int sync_errno;
    unsigned int sync_interrupts;
    unsigned int signaled_creates;
    unsigned int queries;
    unsigned int queried_handles;
    unsigned int bind_calls;
    uint32_t bind_nin;
    struct drm_msm_syncobj bind_in[16];
    uint64_t sync_flags[16];
    unsigned int sync_calls;
    uint64_t completed;
};

extern struct gsl_mock mock;
extern struct gsl_device test_device;

void test_init(void);
void test_fini(void);
int gsl_syncobj_export_fd(struct gsl_device* dev, const struct gsl_syncobj* obj, int* out);
