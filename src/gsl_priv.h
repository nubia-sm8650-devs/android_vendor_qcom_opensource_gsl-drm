/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <drm/msm_drm.h>

#include "gsl.h"
#include "gsl_os.h"

#define GSL_HANDLE_3D 1

struct gsl_vma_range;

struct gsl_vma {
    pthread_mutex_t lock;
    struct gsl_vma_range* free;
};

struct gsl_device {
    int fd;
    int refs;
    uint32_t gen;
    uint32_t chip_id;
    uint32_t gmem_size;
    uint64_t gmem_base;
    uint64_t va_start;
    uint64_t va_size;
    uint32_t highest_bank_bit;
    uint32_t ubwc_swizzle;
    uint32_t macrotile_mode;
    uint32_t raytracing;
    uint64_t uche_trap_base;
    bool has_prr;
    bool cached_noncoherent;
    bool lost;

    uint32_t bind_queue;
    uint32_t bind_syncobj;
    uint64_t bind_point;
    pthread_mutex_t bind_lock;

    struct gsl_vma vma;
};

enum gsl_bo_type {
    GSL_BO_NORMAL,
    GSL_BO_IMPORT,
    GSL_BO_PHYS,
    GSL_BO_VIRT,
};

struct gsl_timeline_obj {
    int refs;
    uint32_t gen;
    uint32_t handle;
    uint32_t deferred_index;
    uint64_t available;
};

struct gsl_sync_point {
    struct gsl_timeline_obj* tl;
    uint64_t point;
};

struct gsl_syncobj {
    int refs;
    bool user;
    int fd;
    uint32_t count;
    struct gsl_sync_point* points;
};

struct gsl_submit_job;

struct gsl_context {
    int refs;
    uint32_t id;
    uint32_t queue;
    uint32_t type;
    uint32_t flags;
    struct gsl_timeline_obj* tl;
    uint32_t last_ts;
    uint64_t last_point;
    bool submitted;
    pthread_mutex_t lock;
    pthread_mutex_t state_lock;
    struct gsl_syncobj* pending_wait;
    struct gsl_context* next;

    pthread_mutex_t sparse_lock;
    uint32_t sparse_queue;
    uint32_t sparse_syncobj;
    uint64_t sparse_point;

    pthread_mutex_t qlock;
    pthread_cond_t qcond;
    struct gsl_submit_job* jobs;
    struct gsl_submit_job* jobs_tail;
    pthread_t worker;
    bool has_worker;
    bool stop;
};

struct gsl_bo {
    enum gsl_bo_type type;
    int refs;
    uint32_t gen;
    uint32_t handle;
    uint32_t msm_flags;
    uint64_t size;
    void* map;
    uint64_t iova;
    uint64_t va_size;
    int dmabuf_fd;
    struct gsl_bo* next;
    struct gsl_sync_point* retire;
    uint32_t nretire;
};

struct gsl_library {
    pthread_mutex_t lock;
    int refs;
    pid_t pid;
    uint32_t flags;
    struct gsl_device* device;
};

extern struct gsl_library gsl_lib;

void* get_panel_settings(void);

struct gsl_device* gsl_device_get(gsl_devhandle_t handle);
struct gsl_device* gsl_device_default(void);
bool gsl_device_owns(const struct gsl_device* dev, uint32_t gen);

int gsl_ioctl(struct gsl_device* dev, unsigned long request, void* arg);
int gsl_errno_status(int err);
int gsl_get_param(struct gsl_device* dev, uint32_t param, uint64_t* value);

int gsl_vma_init(struct gsl_vma* vma, uint64_t start, uint64_t size);
void gsl_vma_fini(struct gsl_vma* vma);
uint64_t gsl_vma_alloc(struct gsl_vma* vma, uint64_t size, uint32_t align_shift);
int gsl_vma_reserve(struct gsl_vma* vma, uint64_t addr, uint64_t size);
void gsl_vma_free(struct gsl_vma* vma, uint64_t addr, uint64_t size);

int gsl_bind(struct gsl_device* dev, uint32_t op, uint32_t handle, uint64_t offset, uint64_t iova,
             uint64_t range);
int gsl_bind_unmap(struct gsl_device* dev, uint64_t iova, uint64_t range);
struct gsl_bind_queue {
    uint32_t id;
    uint32_t syncobj;
    uint64_t* last;
    pthread_mutex_t* lock;
};

int gsl_bind_ops_on(struct gsl_device* dev, const struct gsl_bind_queue* queue,
                    const struct drm_msm_vm_bind_op* ops, uint32_t count,
                    const struct drm_msm_syncobj* in, uint32_t nin, uint64_t* point);
int gsl_bind_ops(struct gsl_device* dev, const struct drm_msm_vm_bind_op* ops, uint32_t count,
                 const struct drm_msm_syncobj* in, uint32_t nin, uint64_t* point);

uint64_t gsl_abs_timeout_ns(uint32_t timeout_ms);

struct gsl_timeline_obj* gsl_tl_create(struct gsl_device* dev);
struct gsl_timeline_obj* gsl_tl_get(struct gsl_timeline_obj* tl);
void gsl_tl_put(struct gsl_device* dev, struct gsl_timeline_obj* tl);
int gsl_tl_query(struct gsl_device* dev, struct gsl_timeline_obj* tl, uint64_t* value);
void gsl_tl_mark_available(struct gsl_timeline_obj* tl, uint64_t point);
bool gsl_tl_available(const struct gsl_timeline_obj* tl, uint64_t point);
bool gsl_syncobj_points_available(const struct gsl_syncobj* obj);

struct gsl_syncobj* gsl_syncobj_new(uint32_t count);
struct gsl_syncobj* gsl_syncobj_get(struct gsl_syncobj* obj);
void gsl_syncobj_put(struct gsl_syncobj* obj);
int gsl_sync_points_signaled(struct gsl_device* dev, const struct gsl_sync_point* points,
                             uint32_t count, bool* signaled);
int gsl_fd_merge(int a, int b);
int gsl_syncobj_wait_internal(struct gsl_device* dev, struct gsl_syncobj* obj, uint32_t timeout_ms);

struct gsl_sync_deps {
    struct drm_msm_syncobj* in;
    uint32_t count;
    uint32_t* temps;
    uint32_t ntemps;
};

int gsl_sync_deps_add(struct gsl_device* dev, struct gsl_sync_deps* deps,
                      const struct gsl_syncobj* obj);
int gsl_sync_deps_add_point(struct gsl_sync_deps* deps, uint32_t handle, uint64_t point);
int gsl_sync_deps_add_points(struct gsl_sync_deps* deps, const struct gsl_syncobj* obj);
void gsl_sync_deps_fini(struct gsl_device* dev, struct gsl_sync_deps* deps);
int gsl_sync_deps_wait(struct gsl_device* dev, const struct gsl_sync_deps* deps, uint32_t flags,
                       uint64_t timeout_abs_ns);
int gsl_syncobj_add_pending(struct gsl_device* dev, const struct gsl_syncobj* obj,
                            struct gsl_sync_deps* deps);

int gsl_syncobj_merge_into(struct gsl_syncobj** dst, struct gsl_syncobj* src);
struct gsl_timeline_signal {
    struct gsl_timeline_obj** timelines;
    uint64_t* values;
    uint32_t count;
};

struct gsl_context* gsl_context_get(gsl_ctxid_t id);
uint64_t gsl_context_ts_point(struct gsl_context* ctx, uint32_t ts);
int gsl_context_marker(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t* ts,
                       struct gsl_syncobj* wait, const struct gsl_timeline_signal* signal);
void gsl_context_put(struct gsl_context* ctx);
int gsl_context_bind(struct gsl_device* dev, gsl_ctxid_t id, const struct drm_msm_vm_bind_op* ops,
                     uint32_t count, struct gsl_syncobj* wait,
                     const struct gsl_timeline_signal* signal, uint32_t* ts);
int gsl_context_snapshot(struct gsl_sync_point** points, uint32_t* count);
void gsl_bo_release(struct gsl_device* dev, struct gsl_bo* bo, const struct gsl_sync_point* after);
void gsl_bo_destroy(struct gsl_device* dev, struct gsl_bo* bo);
void gsl_bo_forget(struct gsl_bo* bo);
void gsl_reap_deferred(struct gsl_device* dev);
void gsl_contexts_stop(void);
void gsl_contexts_reset(bool forked);
void gsl_deferred_reset(void);
void gsl_imports_reset(void);
int gsl_memory_free_pure(struct gsl_memdesc* memdesc);
int gsl_memory_free_after(struct gsl_memdesc* memdesc, const struct gsl_sync_point* after);
