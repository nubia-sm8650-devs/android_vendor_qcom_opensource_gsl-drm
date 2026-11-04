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
    uint32_t bind_queue;
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
    void (*on_submit)(void);
    unsigned int ioctl_calls;
    unsigned int timeline_waits;
    unsigned int fd_imports;
    uint32_t submit_flags;
    int submit_fence_fd;
    bool fake_clock;
    uint64_t now_ns;
    bool track_allocations;
    unsigned int allocations;
    unsigned int reallocations;
    unsigned int fail_allocation;
    unsigned int syncobj_destroys;
    int fd_import_errno;
    unsigned int fd_merges;
    int fd_merge_errno;
    int (*check_wait)(const struct drm_syncobj_timeline_wait* req);
    void (*check_submit)(const struct drm_msm_gem_submit* req);
};

extern struct gsl_mock mock;
extern struct gsl_device test_device;

void test_init(void);
void test_fini(void);
int gsl_syncobj_export_fd(struct gsl_device* dev, const struct gsl_syncobj* obj, int* out);
