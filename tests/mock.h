/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "gsl_priv.h"

struct gsl_mock {
    bool fail_dup;
    int sync_errno;
    int query_errno;
    unsigned int sync_interrupts;
    unsigned int signaled_creates;
    unsigned int queries;
    unsigned int queried_handles;
    unsigned int bind_calls;
    unsigned int submit_calls;
    unsigned int submit_timeouts;
    int bind_errno;
    uint32_t bind_nin;
    struct drm_msm_syncobj bind_in[16];
    uint32_t submit_nin;
    uint32_t submit_nout;
    struct drm_msm_syncobj submit_in[16];
    struct drm_msm_syncobj submit_out[16];
    uint64_t sync_flags[16];
    unsigned int sync_calls;
    uint64_t completed;
    bool point_pending;
    unsigned int pending_wait_timeouts;
    unsigned int transfers;
    unsigned int available_waits;
    uint64_t transfer_flags;
    uint64_t wait_flags;
    bool fence_unsignaled;
    unsigned int block_timeouts;
    unsigned int poll_waits;
    unsigned int block_waits;
    unsigned int sleeps;
};

extern struct gsl_mock mock;
extern struct gsl_device test_device;

void test_init(void);
void test_fini(void);
int gsl_syncobj_export_fd(struct gsl_device* dev, const struct gsl_syncobj* obj, int* out);
