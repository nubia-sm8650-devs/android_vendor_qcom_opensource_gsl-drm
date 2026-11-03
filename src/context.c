/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <drm/drm.h>

#include "gsl_priv.h"

#define GSL_CONTEXT_FLAG_BIND 0x00040000u
#define GSL_CONTEXT_PRIORITY_SHIFT 12
#define GSL_CONTEXT_PRIORITY_MASK 0xfu
#define GSL_CONTEXT_TYPE_CL 4

#define GSL_SUBMIT_FLAG_MEMLIST 0x1u
#define GSL_IBDESC_MEMLIST_MASK 0x3u

#define GSL_GUILTY_CONTEXT_RESET 0x8253
#define GSL_INNOCENT_CONTEXT_RESET 0x8254

#define GSL_WORKER_POLL_MS 100
#define GSL_WORKER_DRAIN_MS 5000
#define GSL_DESTROY_WAIT_MS 20000
#define GSL_REAP_BATCH_SIZE 64

struct gsl_ibdesc {
    uint64_t gpuaddr;
    uint64_t hostptr;
    uint32_t sizedwords;
    uint32_t flags;
};

struct gsl_cmdobj {
    struct gsl_memdesc* memdesc;
    uint64_t sizedwords;
    uint64_t offset;
    uint64_t flags;
};

struct gsl_deferred {
    struct gsl_bo* bo;
    struct gsl_deferred* next;
};

static void gsl_context_stop_worker(struct gsl_context* ctx);

static pthread_mutex_t gsl_ctx_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gsl_context* gsl_contexts;
static uint32_t gsl_next_ctx_id;

static pthread_mutex_t gsl_defer_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gsl_deferred* gsl_deferred;
static struct gsl_deferred* gsl_deferred_tail;

int gsl_timestamp_cmp(uint32_t a, uint32_t b) {
    if (a == b) return 0;

    return (int32_t)(a - b) > 0 ? 1 : -1;
}

static bool gsl_ts_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static uint64_t gsl_context_point(const struct gsl_context* ctx, uint32_t ts) {
    int64_t point = (int64_t)ctx->last_point + (int32_t)(ts - ctx->last_ts);

    return point > 0 ? (uint64_t)point : 0;
}

uint64_t gsl_context_ts_point(struct gsl_context* ctx, uint32_t ts) {
    uint64_t point;

    pthread_mutex_lock(&ctx->lock);
    point = gsl_context_point(ctx, ts);
    pthread_mutex_unlock(&ctx->lock);

    return point;
}

struct gsl_context* gsl_context_get(gsl_ctxid_t id) {
    struct gsl_context* ctx;

    pthread_mutex_lock(&gsl_ctx_lock);
    for (ctx = gsl_contexts; ctx; ctx = ctx->next) {
        if (ctx->id == id) {
            ctx->refs++;
            break;
        }
    }
    pthread_mutex_unlock(&gsl_ctx_lock);

    return ctx;
}

void gsl_context_put(struct gsl_context* ctx) {
    struct gsl_device* dev = gsl_device_default();
    bool last;

    if (!ctx) return;

    pthread_mutex_lock(&gsl_ctx_lock);
    last = --ctx->refs == 0;
    pthread_mutex_unlock(&gsl_ctx_lock);
    if (!last) return;

    gsl_context_stop_worker(ctx);
    pthread_mutex_destroy(&ctx->qlock);
    pthread_cond_destroy(&ctx->qcond);

    if (dev && ctx->queue) {
        uint32_t id = ctx->queue;

        gsl_ioctl(dev, DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, &id);
    }
    gsl_syncobj_put(ctx->pending_wait);
    gsl_tl_put(dev, ctx->tl);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

int gsl_context_snapshot(struct gsl_sync_point** points, uint32_t* count) {
    struct gsl_sync_point* out = NULL;
    struct gsl_context* ctx;
    uint32_t n = 0;

    pthread_mutex_lock(&gsl_ctx_lock);
    for (ctx = gsl_contexts; ctx; ctx = ctx->next) {
        struct gsl_sync_point* grown;

        pthread_mutex_lock(&ctx->lock);
        if (!ctx->submitted) {
            pthread_mutex_unlock(&ctx->lock);
            continue;
        }

        grown = realloc(out, (n + 1) * sizeof(*out));
        if (!grown) {
            pthread_mutex_unlock(&ctx->lock);
            pthread_mutex_unlock(&gsl_ctx_lock);
            for (uint32_t i = 0; i < n; i++) gsl_tl_put(NULL, out[i].tl);
            free(out);
            return GSL_FAILURE_OUTOFMEM;
        }
        out = grown;
        out[n].tl = gsl_tl_get(ctx->tl);
        out[n].point = ctx->last_point;
        n++;
        pthread_mutex_unlock(&ctx->lock);
    }
    pthread_mutex_unlock(&gsl_ctx_lock);

    *points = out;
    *count = n;

    return GSL_SUCCESS;
}

static void gsl_retire_points_put(struct gsl_device* dev, struct gsl_bo* bo) {
    for (uint32_t i = 0; i < bo->nretire; i++) gsl_tl_put(dev, bo->retire[i].tl);
    free(bo->retire);
    bo->retire = NULL;
    bo->nretire = 0;
}

static int gsl_retire_points_add(struct gsl_bo* bo, const struct gsl_sync_point* point) {
    struct gsl_sync_point* grown = realloc(bo->retire, (bo->nretire + 1) * sizeof(*grown));

    if (!grown) return GSL_FAILURE_OUTOFMEM;

    grown[bo->nretire].tl = gsl_tl_get(point->tl);
    grown[bo->nretire].point = point->point;
    bo->retire = grown;
    bo->nretire++;

    return GSL_SUCCESS;
}

void gsl_bo_release(struct gsl_device* dev, struct gsl_bo* bo, const struct gsl_sync_point* after) {
    struct gsl_deferred* node;

    if (!gsl_device_owns(dev, bo->gen)) {
        gsl_retire_points_put(dev, bo);
        gsl_bo_forget(bo);
        return;
    }

    gsl_reap_deferred(dev);

    if (!bo->va_size || gsl_context_snapshot(&bo->retire, &bo->nretire)) {
        gsl_retire_points_put(dev, bo);
        gsl_bo_destroy(dev, bo);
        return;
    }

    if (after && after->point && gsl_retire_points_add(bo, after)) {
        struct gsl_syncobj obj = {.count = 1, .points = (struct gsl_sync_point*)after, .fd = -1};

        gsl_syncobj_wait_internal(dev, &obj, GSL_TIMEOUT_INFINITE);
    }

    if (!bo->nretire) {
        gsl_retire_points_put(dev, bo);
        gsl_bo_destroy(dev, bo);
        return;
    }

    node = malloc(sizeof(*node));
    if (!node) {
        struct gsl_syncobj obj = {.count = bo->nretire, .points = bo->retire, .fd = -1};

        gsl_syncobj_wait_internal(dev, &obj, GSL_TIMEOUT_INFINITE);
        gsl_retire_points_put(dev, bo);
        gsl_bo_destroy(dev, bo);
        return;
    }

    node->bo = bo;
    node->next = NULL;
    pthread_mutex_lock(&gsl_defer_lock);
    if (gsl_deferred_tail)
        gsl_deferred_tail->next = node;
    else
        gsl_deferred = node;
    gsl_deferred_tail = node;
    pthread_mutex_unlock(&gsl_defer_lock);
}

void gsl_reap_deferred(struct gsl_device* dev) {
    struct gsl_deferred *batch[GSL_REAP_BATCH_SIZE], *node, *done = NULL;
    struct gsl_timeline_obj** tls = NULL;
    uint32_t* handles = NULL;
    uint64_t* values = NULL;
    uint32_t count = 0, ntls = 0;
    size_t max_points = 0;

    pthread_mutex_lock(&gsl_defer_lock);
    for (node = gsl_deferred; node && count < GSL_REAP_BATCH_SIZE; node = node->next) {
        batch[count++] = node;
        max_points += node->bo->nretire;
    }
    if (!count) goto unlock;

    tls = calloc(max_points, sizeof(*tls));
    handles = calloc(max_points, sizeof(*handles));
    values = calloc(max_points, sizeof(*values));
    if (!tls || !handles || !values) goto unlock;

    for (uint32_t i = 0; i < count; i++) {
        struct gsl_bo* bo = batch[i]->bo;

        for (uint32_t j = 0; j < bo->nretire; j++) {
            struct gsl_timeline_obj* tl = bo->retire[j].tl;

            if (tl->deferred_index) continue;
            tls[ntls] = tl;
            handles[ntls] = tl->handle;
            tl->deferred_index = ++ntls;
        }
    }

    struct drm_syncobj_timeline_array req = {
            .handles = (uintptr_t)handles,
            .points = (uintptr_t)values,
            .count_handles = ntls,
    };
    if (gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_QUERY, &req)) goto reset;

    gsl_deferred = batch[count - 1]->next;
    if (!gsl_deferred) gsl_deferred_tail = NULL;
    for (uint32_t i = 0; i < count; i++) {
        struct gsl_bo* bo = batch[i]->bo;
        bool signaled = true;

        for (uint32_t j = 0; j < bo->nretire; j++) {
            if (values[bo->retire[j].tl->deferred_index - 1] < bo->retire[j].point) {
                signaled = false;
                break;
            }
        }

        node = batch[i];
        if (signaled) {
            node->next = done;
            done = node;
        } else {
            node->next = NULL;
            if (gsl_deferred_tail)
                gsl_deferred_tail->next = node;
            else
                gsl_deferred = node;
            gsl_deferred_tail = node;
        }
    }

reset:
    for (uint32_t i = 0; i < ntls; i++) tls[i]->deferred_index = 0;
unlock:
    pthread_mutex_unlock(&gsl_defer_lock);
    free(tls);
    free(handles);
    free(values);

    while ((node = done)) {
        done = node->next;
        gsl_retire_points_put(dev, node->bo);
        gsl_bo_destroy(dev, node->bo);
        free(node);
    }
}

void gsl_deferred_reset(void) {
    struct gsl_deferred* node;

    pthread_mutex_lock(&gsl_defer_lock);
    node = gsl_deferred;
    gsl_deferred = NULL;
    gsl_deferred_tail = NULL;
    pthread_mutex_unlock(&gsl_defer_lock);

    while (node) {
        struct gsl_deferred* next = node->next;

        gsl_retire_points_put(NULL, node->bo);
        gsl_bo_forget(node->bo);
        free(node);
        node = next;
    }
}

static uint32_t gsl_msm_priority(struct gsl_device* dev, uint32_t type, uint32_t flags) {
    uint32_t prio = (flags >> GSL_CONTEXT_PRIORITY_SHIFT) & GSL_CONTEXT_PRIORITY_MASK;
    uint32_t msm_prio;
    uint64_t levels;

    if (type == GSL_CONTEXT_TYPE_CL)
        msm_prio = prio >= 1 && prio <= 7 ? 2 : 3;
    else if (!(prio & 0x7))
        msm_prio = 2;
    else
        msm_prio = prio < 9 ? 1 : 3;

    if (!gsl_get_param(dev, MSM_PARAM_PRIORITIES, &levels) && levels && msm_prio >= levels)
        msm_prio = (uint32_t)levels - 1;

    return msm_prio;
}

static int gsl_context_new(struct gsl_device* dev, uint32_t type, uint32_t flags, gsl_ctxid_t* id) {
    struct drm_msm_submitqueue queue = {.prio = gsl_msm_priority(dev, type, flags)};
    struct gsl_context* ctx;
    int ret;

    if (flags & GSL_CONTEXT_FLAG_BIND) {
        GSL_ERR("Bind contexts are unavailable on DRM kernel");
        return GSL_FAILURE_BADPARAM;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return GSL_FAILURE_OUTOFMEM;

    ctx->refs = 1;
    ctx->type = type;
    ctx->flags = flags;
    ctx->last_point = 0;
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_mutex_init(&ctx->qlock, NULL);
    pthread_cond_init(&ctx->qcond, NULL);

    ret = gsl_ioctl(dev, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &queue);
    if (ret) {
        GSL_ERR("Failed to create kernel drawqueue %d", ret);
        goto err;
    }
    ctx->queue = queue.id;

    ctx->tl = gsl_tl_create(dev);
    if (!ctx->tl) {
        ret = GSL_FAILURE_OUTOFMEM;
        goto err;
    }

    pthread_mutex_lock(&gsl_ctx_lock);
    do {
        if (++gsl_next_ctx_id == UINT32_MAX) gsl_next_ctx_id = 1;
    } while (gsl_next_ctx_id == 0);
    ctx->id = gsl_next_ctx_id;
    ctx->next = gsl_contexts;
    gsl_contexts = ctx;
    pthread_mutex_unlock(&gsl_ctx_lock);

    *id = ctx->id;
    GSL_DBG("CTXT %u Created with flags 0x%x queue %u prio %u", ctx->id, flags, ctx->queue,
            queue.prio);

    return GSL_SUCCESS;

err:
    if (ctx->queue) {
        uint32_t qid = ctx->queue;

        gsl_ioctl(dev, DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, &qid);
    }
    pthread_cond_destroy(&ctx->qcond);
    pthread_mutex_destroy(&ctx->qlock);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    return ret;
}

gsl_ctxid_t gsl_context_create(gsl_devhandle_t handle, uint32_t type, uint32_t flags, void* extra) {
    struct gsl_device* dev = gsl_device_get(handle);
    gsl_ctxid_t id;

    if (!dev) return UINT32_MAX;

    if (flags & GSL_CONTEXT_FLAG_CLIENT_GENERATED_TS) flags |= GSL_CONTEXT_FLAG_USER_GENERATED_TS;

    if (gsl_context_new(dev, type, flags, &id)) return UINT32_MAX;

    return id;
}

int gsl_context_destroy(gsl_devhandle_t handle, gsl_ctxid_t id) {
    struct gsl_device* dev = gsl_device_get(handle);
    struct gsl_context **link, *ctx = NULL;
    struct gsl_sync_point point;

    if (!dev) return GSL_FAILURE_BADPARAM;

    pthread_mutex_lock(&gsl_ctx_lock);
    for (link = &gsl_contexts; *link; link = &(*link)->next) {
        if ((*link)->id == id) {
            ctx = *link;
            *link = ctx->next;
            break;
        }
    }
    pthread_mutex_unlock(&gsl_ctx_lock);

    if (!ctx) return GSL_FAILURE_BADPARAM;

    gsl_context_stop_worker(ctx);

    pthread_mutex_lock(&ctx->lock);
    point.tl = ctx->tl;
    point.point = ctx->submitted ? ctx->last_point : 0;
    pthread_mutex_unlock(&ctx->lock);

    struct gsl_syncobj obj = {.count = 1, .points = &point, .fd = -1};

    if (gsl_syncobj_wait_internal(dev, &obj, GSL_DESTROY_WAIT_MS))
        GSL_ERR("Failed waiting for pending submits to ctxt %u. destroying context anyway.", id);

    gsl_context_put(ctx);

    return GSL_SUCCESS;
}

void gsl_contexts_stop(void) {
    struct gsl_context* ctx;

    pthread_mutex_lock(&gsl_ctx_lock);
    for (ctx = gsl_contexts; ctx; ctx = ctx->next) ctx->refs++;
    ctx = gsl_contexts;
    pthread_mutex_unlock(&gsl_ctx_lock);

    while (ctx) {
        struct gsl_context* next = ctx->next;

        gsl_context_stop_worker(ctx);
        gsl_context_put(ctx);
        ctx = next;
    }
}

void gsl_contexts_reset(bool forked) {
    struct gsl_context* ctx;

    pthread_mutex_lock(&gsl_ctx_lock);
    ctx = gsl_contexts;
    gsl_contexts = NULL;
    pthread_mutex_unlock(&gsl_ctx_lock);

    if (forked) return;

    while (ctx) {
        struct gsl_context* next = ctx->next;

        gsl_context_put(ctx);
        ctx = next;
    }
}

static int gsl_context_next_ts(struct gsl_context* ctx, uint32_t* ts) {
    uint32_t next;

    if (ctx->flags & GSL_CONTEXT_FLAG_CLIENT_GENERATED_TS) {
        next = *ts;
        if (ctx->submitted && !gsl_ts_after(next, ctx->last_ts)) {
            GSL_ERR("ctx: %d next client ts %u must be greater than current ts %u", ctx->id, next,
                    ctx->last_ts);
            return GSL_FAILURE_BADPARAM;
        }
    } else {
        next = ctx->last_ts + 1;
        if (next == UINT32_MAX) next = 1;
    }

    *ts = next;

    return GSL_SUCCESS;
}

struct gsl_submit_job {
    struct drm_msm_gem_submit_cmd* cmds;
    uint32_t ncmds;
    uint32_t ts;
    struct drm_msm_vm_bind_op* ops;
    uint32_t nops;
    struct gsl_syncobj* wait;
    struct gsl_syncobj* pending;
    struct drm_msm_syncobj* out;
    struct gsl_timeline_obj** out_tls;
    uint32_t nout;
    struct gsl_submit_job* next;
};

static void gsl_submit_job_free(struct gsl_device* dev, struct gsl_submit_job* job) {
    for (uint32_t i = 0; i < job->nout; i++) gsl_tl_put(dev, job->out_tls[i]);
    gsl_syncobj_put(job->wait);
    gsl_syncobj_put(job->pending);
    free(job->out_tls);
    free(job->out);
    free(job->cmds);
    free(job->ops);
    free(job);
}

static int gsl_deps_wait_available(struct gsl_device* dev, const struct gsl_sync_deps* deps,
                                   uint64_t timeout_abs_ns) {
    uint32_t* handles;
    uint64_t* points;
    int ret;

    if (!deps->count) return GSL_SUCCESS;

    handles = calloc(deps->count, sizeof(*handles));
    points = calloc(deps->count, sizeof(*points));
    if (!handles || !points) {
        free(handles);
        free(points);
        return GSL_FAILURE_OUTOFMEM;
    }
    for (uint32_t i = 0; i < deps->count; i++) {
        handles[i] = deps->in[i].handle;
        points[i] = deps->in[i].point;
    }

    struct drm_syncobj_timeline_wait wait = {
            .handles = (uintptr_t)handles,
            .points = (uintptr_t)points,
            .timeout_nsec = (int64_t)timeout_abs_ns,
            .count_handles = deps->count,
            .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
    };
    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait);

    free(handles);
    free(points);
    return ret;
}

static int gsl_submit_job_run(struct gsl_device* dev, struct gsl_context* ctx,
                              struct gsl_submit_job* job, uint64_t timeout_abs_ns) {
    struct gsl_sync_deps deps = {0};
    uint64_t bind_point = 0;
    int ret = GSL_SUCCESS;

    if (!job->ops) {
        pthread_mutex_lock(&dev->bind_lock);
        bind_point = dev->bind_point;
        pthread_mutex_unlock(&dev->bind_lock);
    }

    if (bind_point) ret = gsl_sync_deps_add_point(&deps, dev->bind_syncobj, bind_point);
    if (!ret) ret = gsl_sync_deps_add(dev, &deps, job->wait);
    if (!ret) ret = gsl_sync_deps_add(dev, &deps, job->pending);
    if (!ret) ret = gsl_deps_wait_available(dev, &deps, timeout_abs_ns);
    if (ret) goto out;

    if (job->ops) {
        ret = gsl_bind_ops(dev, job->ops, job->nops, deps.in, deps.count);
        goto out;
    }

    struct drm_msm_gem_submit req = {
            .flags = MSM_PIPE_3D0 | MSM_SUBMIT_NO_IMPLICIT | MSM_SUBMIT_SYNCOBJ_OUT |
                     (deps.count ? MSM_SUBMIT_SYNCOBJ_IN : 0),
            .nr_cmds = job->ncmds,
            .cmds = (uintptr_t)job->cmds,
            .queueid = ctx->queue,
            .in_syncobjs = (uintptr_t)deps.in,
            .out_syncobjs = (uintptr_t)job->out,
            .nr_in_syncobjs = deps.count,
            .nr_out_syncobjs = job->nout,
            .syncobj_stride = sizeof(struct drm_msm_syncobj),
    };

    ret = gsl_ioctl(dev, DRM_IOCTL_MSM_GEM_SUBMIT, &req);
    if (ret == GSL_FAILURE_HANG) {
        __atomic_store_n(&dev->lost, true, __ATOMIC_RELAXED);
        ret = GSL_FAILURE_CTXT_DEADLOCK;
    }
    if (ret) GSL_ERR("submit on ctx %u ts %u failed %d", ctx->id, job->ts, ret);

out:
    gsl_sync_deps_fini(dev, &deps);
    return ret;
}

static void gsl_submit_job_signal(struct gsl_device* dev, const struct gsl_submit_job* job) {
    uint32_t* handles = calloc(job->nout, sizeof(*handles));
    uint64_t* points = calloc(job->nout, sizeof(*points));

    if (handles && points) {
        for (uint32_t i = 0; i < job->nout; i++) {
            handles[i] = job->out[i].handle;
            points[i] = job->out[i].point;
        }

        struct drm_syncobj_timeline_array req = {
                .handles = (uintptr_t)handles,
                .points = (uintptr_t)points,
                .count_handles = job->nout,
        };

        gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &req);
    }

    free(handles);
    free(points);
}

static void* gsl_context_worker(void* data) {
    struct gsl_context* ctx = data;
    struct gsl_device* dev = gsl_device_default();

    pthread_mutex_lock(&ctx->qlock);
    for (;;) {
        struct gsl_submit_job* job;
        uint64_t deadline = 0;
        int ret;

        while (!ctx->jobs && !ctx->stop) pthread_cond_wait(&ctx->qcond, &ctx->qlock);
        job = ctx->jobs;
        if (!job) break;
        pthread_mutex_unlock(&ctx->qlock);

        for (;;) {
            bool stop;

            ret = gsl_submit_job_run(dev, ctx, job, gsl_abs_timeout_ns(GSL_WORKER_POLL_MS));
            if (ret != GSL_FAILURE_TIMEOUT) break;

            pthread_mutex_lock(&ctx->qlock);
            stop = ctx->stop;
            pthread_mutex_unlock(&ctx->qlock);
            if (!stop) continue;

            if (!deadline)
                deadline = gsl_abs_timeout_ns(GSL_WORKER_DRAIN_MS);
            else if (gsl_abs_timeout_ns(0) >= deadline)
                break;
        }
        if (ret) {
            GSL_ERR("dropping deferred submit ts %u on ctx %u: %d", job->ts, ctx->id, ret);
            gsl_submit_job_signal(dev, job);
        }

        pthread_mutex_lock(&ctx->qlock);
        ctx->jobs = job->next;
        if (!ctx->jobs) ctx->jobs_tail = NULL;
        gsl_submit_job_free(dev, job);
    }
    pthread_mutex_unlock(&ctx->qlock);

    return NULL;
}

static int gsl_context_defer(struct gsl_context* ctx, struct gsl_submit_job* job) {
    pthread_mutex_lock(&ctx->qlock);
    if (!ctx->has_worker) {
        if (pthread_create(&ctx->worker, NULL, gsl_context_worker, ctx)) {
            pthread_mutex_unlock(&ctx->qlock);
            return GSL_FAILURE_SYSTEMERROR;
        }
        pthread_setname_np(ctx->worker, "AdrenoGslSubmit");
        ctx->has_worker = true;
    }

    job->next = NULL;
    if (ctx->jobs_tail)
        ctx->jobs_tail->next = job;
    else
        ctx->jobs = job;
    ctx->jobs_tail = job;
    pthread_cond_signal(&ctx->qcond);
    pthread_mutex_unlock(&ctx->qlock);

    return GSL_SUCCESS;
}

static void gsl_context_stop_worker(struct gsl_context* ctx) {
    pthread_t worker;
    bool has_worker;

    pthread_mutex_lock(&ctx->qlock);
    ctx->stop = true;
    pthread_cond_signal(&ctx->qcond);
    has_worker = ctx->has_worker;
    worker = ctx->worker;
    ctx->has_worker = false;
    pthread_mutex_unlock(&ctx->qlock);

    if (has_worker) pthread_join(worker, NULL);
}

static int gsl_context_queue(struct gsl_device* dev, struct gsl_context* ctx,
                             struct gsl_submit_job** job) {
    bool queued;
    int ret;

    pthread_mutex_lock(&ctx->qlock);
    queued = ctx->jobs != NULL;
    pthread_mutex_unlock(&ctx->qlock);

    ret = queued ? GSL_FAILURE_TIMEOUT : gsl_submit_job_run(dev, ctx, *job, 0);
    if (ret == GSL_FAILURE_TIMEOUT) {
        ret = gsl_context_defer(ctx, *job);
        if (!ret) *job = NULL;
    }

    return ret;
}

int gsl_context_bind(struct gsl_device* dev, gsl_ctxid_t id, const struct drm_msm_vm_bind_op* ops,
                     uint32_t count, struct gsl_syncobj* wait) {
    struct gsl_submit_job* job;
    struct gsl_context* ctx;
    int ret;

    if (!count) return GSL_SUCCESS;

    ctx = gsl_context_get(id);
    if (!ctx) {
        struct gsl_sync_deps deps = {0};

        ret = gsl_sync_deps_add(dev, &deps, wait);
        if (!ret) ret = gsl_bind_ops(dev, ops, count, deps.in, deps.count);
        gsl_sync_deps_fini(dev, &deps);

        return ret;
    }

    job = calloc(1, sizeof(*job));
    if (job) job->ops = malloc(count * sizeof(*ops));
    if (!job || !job->ops) {
        free(job);
        gsl_context_put(ctx);
        return GSL_FAILURE_OUTOFMEM;
    }
    memcpy(job->ops, ops, count * sizeof(*ops));
    job->nops = count;
    if (wait) job->wait = gsl_syncobj_get(wait);

    pthread_mutex_lock(&ctx->lock);
    ret = gsl_context_queue(dev, ctx, &job);
    pthread_mutex_unlock(&ctx->lock);

    if (job) gsl_submit_job_free(dev, job);
    gsl_context_put(ctx);

    return ret;
}

static int gsl_context_submit(struct gsl_device* dev, struct gsl_context* ctx,
                              struct drm_msm_gem_submit_cmd* cmds, uint32_t ncmds, uint32_t* ts,
                              struct gsl_syncobj* wait, const struct gsl_timeline_signal* signal) {
    struct gsl_submit_job* job;
    uint32_t next, count;
    uint64_t point;
    int ret;

    gsl_reap_deferred(dev);

    job = calloc(1, sizeof(*job));
    if (!job) return GSL_FAILURE_OUTOFMEM;

    count = 1 + (signal ? signal->count : 0);
    job->out = calloc(count, sizeof(*job->out));
    job->out_tls = calloc(count, sizeof(*job->out_tls));
    job->cmds = calloc(ncmds ? ncmds : 1, sizeof(*job->cmds));
    if (!job->out || !job->out_tls || !job->cmds) {
        gsl_submit_job_free(dev, job);
        return GSL_FAILURE_OUTOFMEM;
    }
    memcpy(job->cmds, cmds, ncmds * sizeof(*cmds));
    job->ncmds = ncmds;
    if (wait) job->wait = gsl_syncobj_get(wait);

    pthread_mutex_lock(&ctx->lock);

    next = *ts;
    ret = gsl_context_next_ts(ctx, &next);
    if (ret) {
        pthread_mutex_unlock(&ctx->lock);
        gsl_submit_job_free(dev, job);
        return ret;
    }
    job->ts = next;
    point = ctx->submitted ? gsl_context_point(ctx, next) : 1;

    job->out_tls[0] = gsl_tl_get(ctx->tl);
    job->out[0].handle = ctx->tl->handle;
    job->out[0].point = point;
    job->nout = 1;
    for (uint32_t i = 0; signal && i < signal->count; i++) {
        if (!signal->timelines[i]) continue;
        job->out_tls[job->nout] = gsl_tl_get(signal->timelines[i]);
        job->out[job->nout].handle = signal->timelines[i]->handle;
        job->out[job->nout].point = signal->values[i];
        job->nout++;
    }

    job->pending = ctx->pending_wait;
    ctx->pending_wait = NULL;

    ret = gsl_context_queue(dev, ctx, &job);
    if (ret) {
        ctx->pending_wait = job->pending;
        job->pending = NULL;
    } else {
        ctx->last_point = point;
        ctx->last_ts = next;
        ctx->submitted = true;
        *ts = next;
    }

    pthread_mutex_unlock(&ctx->lock);
    if (job) gsl_submit_job_free(dev, job);

    return ret;
}

static int gsl_issue_cmdobjs(gsl_devhandle_t handle, gsl_ctxid_t id, const struct gsl_cmdobj* objs,
                             uint32_t count, uint32_t* ts, struct gsl_syncobj* wait,
                             const struct gsl_timeline_signal* signal) {
    struct gsl_device* dev = gsl_device_get(handle);
    struct drm_msm_gem_submit_cmd* cmds;
    struct gsl_context* ctx;
    int ret;

    if (!dev || !ts || (count && !objs)) return GSL_FAILURE_BADPARAM;

    ctx = gsl_context_get(id);
    if (!ctx) return GSL_FAILURE_BADPARAM;

    cmds = calloc(count ? count : 1, sizeof(*cmds));
    if (!cmds) {
        gsl_context_put(ctx);
        return GSL_FAILURE_OUTOFMEM;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!objs[i].memdesc) {
            free(cmds);
            gsl_context_put(ctx);
            return GSL_FAILURE_BADPARAM;
        }
        cmds[i].type = MSM_SUBMIT_CMD_BUF;
        cmds[i].size = (uint32_t)(objs[i].sizedwords * 4);
        cmds[i].iova = objs[i].memdesc->gpuaddr + objs[i].offset;
    }

    ret = gsl_context_submit(dev, ctx, cmds, count, ts, wait, signal);

    free(cmds);
    gsl_context_put(ctx);
    return ret;
}

int gsl_context_marker(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t* ts,
                       struct gsl_syncobj* wait, const struct gsl_timeline_signal* signal) {
    return gsl_issue_cmdobjs(handle, id, NULL, 0, ts, wait, signal);
}

int gsl_command_issueib_with_alloc_list(gsl_devhandle_t handle, gsl_ctxid_t id,
                                        const struct gsl_cmdobj* cmds, uint32_t ncmds,
                                        const struct gsl_cmdobj* allocs, uint32_t nallocs,
                                        uint32_t* ts, uint32_t flags, struct gsl_syncobj* wait,
                                        void* extra) {
    if (!ncmds) return GSL_FAILURE_BADPARAM;

    return gsl_issue_cmdobjs(handle, id, cmds, ncmds, ts, wait, NULL);
}

static int gsl_issue_ibdescs(gsl_devhandle_t handle, gsl_ctxid_t id, const struct gsl_ibdesc* ibs,
                             uint32_t count, uint32_t* ts, uint32_t flags, struct gsl_syncobj* wait,
                             const struct gsl_timeline_signal* signal) {
    struct gsl_memdesc* memdescs;
    struct gsl_cmdobj* objs;
    uint32_t n = 0;
    int ret;

    if (!count || !ibs) return GSL_FAILURE_BADPARAM;

    objs = calloc(count, sizeof(*objs));
    memdescs = calloc(count, sizeof(*memdescs));
    if (!objs || !memdescs) {
        free(objs);
        free(memdescs);
        return GSL_FAILURE_OUTOFMEM;
    }

    for (uint32_t i = 0; i < count; i++) {
        if ((flags & GSL_SUBMIT_FLAG_MEMLIST) && (ibs[i].flags & GSL_IBDESC_MEMLIST_MASK)) continue;

        memdescs[n].gpuaddr = ibs[i].gpuaddr;
        memdescs[n].size = (uint64_t)ibs[i].sizedwords * 4;
        objs[n].memdesc = &memdescs[n];
        objs[n].sizedwords = ibs[i].sizedwords;
        n++;
    }

    ret = n ? gsl_issue_cmdobjs(handle, id, objs, n, ts, wait, signal) : GSL_FAILURE_BADPARAM;

    free(objs);
    free(memdescs);
    return ret;
}

int gsl_command_issueib_sync(gsl_devhandle_t handle, gsl_ctxid_t id, const struct gsl_ibdesc* ibs,
                             uint32_t count, uint32_t* ts, uint32_t flags,
                             struct gsl_syncobj* wait) {
    return gsl_issue_ibdescs(handle, id, ibs, count, ts, flags, wait, NULL);
}

int gsl_command_issueib(gsl_devhandle_t handle, gsl_ctxid_t id, const struct gsl_ibdesc* ibs,
                        uint32_t count, uint32_t* ts, uint32_t flags, struct gsl_syncobj* wait) {
    return gsl_issue_ibdescs(handle, id, ibs, count, ts, flags, wait, NULL);
}

int gsl_command_issueib_timeline_sync(gsl_devhandle_t handle, gsl_ctxid_t id,
                                      const struct gsl_ibdesc* ibs, uint32_t count, uint32_t* ts,
                                      uint32_t flags, struct gsl_syncobj* wait,
                                      const struct gsl_timeline_signal* signal, void* extra) {
    return gsl_issue_ibdescs(handle, id, ibs, count, ts, flags, wait, signal);
}

static int gsl_context_retired(struct gsl_device* dev, struct gsl_context* ctx, uint32_t* ts) {
    uint64_t point;
    int ret = gsl_tl_query(dev, ctx->tl, &point);

    if (ret) return ret;

    pthread_mutex_lock(&ctx->lock);
    *ts = ctx->last_ts - (uint32_t)(ctx->last_point - point);
    pthread_mutex_unlock(&ctx->lock);

    return GSL_SUCCESS;
}

int gsl_command_readtimestamp(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t type,
                              uint32_t* out) {
    struct gsl_device* dev = gsl_device_get(handle);
    struct gsl_context* ctx;
    int ret = GSL_SUCCESS;

    if (!dev || !out) return GSL_FAILURE_BADPARAM;

    ctx = gsl_context_get(id);
    if (!ctx) return GSL_FAILURE_BADPARAM;

    switch (type) {
        case GSL_TIMESTAMP_QUEUED:
            pthread_mutex_lock(&ctx->lock);
            *out = ctx->last_ts;
            pthread_mutex_unlock(&ctx->lock);
            break;
        case GSL_TIMESTAMP_CONSUMED:
        case GSL_TIMESTAMP_RETIRED:
            ret = gsl_context_retired(dev, ctx, out);
            break;
        default:
            GSL_WARN("bad timestamp type %d", type);
            ret = GSL_FAILURE_BADPARAM;
            break;
    }

    gsl_context_put(ctx);
    return ret;
}

int gsl_command_checktimestamp(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t ts, uint32_t type) {
    uint32_t current;
    int ret = gsl_command_readtimestamp(handle, id, type, &current);

    if (ret) return ret;

    return !gsl_ts_after(ts, current);
}

int gsl_command_waittimestamp(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t ts,
                              uint32_t timeout_ms) {
    struct gsl_device* dev = gsl_device_get(handle);
    struct gsl_sync_point point;
    struct gsl_context* ctx;
    int ret;

    if (!dev) return GSL_FAILURE_BADPARAM;

    ctx = gsl_context_get(id);
    if (!ctx) return GSL_FAILURE_BADPARAM;

    pthread_mutex_lock(&ctx->lock);
    point.tl = ctx->tl;
    point.point = gsl_context_point(ctx, ts);
    pthread_mutex_unlock(&ctx->lock);

    struct gsl_syncobj obj = {.count = 1, .points = &point, .fd = -1};

    ret = gsl_syncobj_wait_internal(dev, &obj, timeout_ms);
    if (ret && ret != GSL_FAILURE_TIMEOUT) GSL_ERR("error:%d ctx %08x ts %u", ret, id, ts);

    gsl_context_put(ctx);
    return ret;
}

int gsl_syncobj_merge_into(struct gsl_syncobj** dst, struct gsl_syncobj* src) {
    struct gsl_syncobj* merged;
    uint32_t n = 0;

    if (!*dst) {
        *dst = gsl_syncobj_get(src);
        return GSL_SUCCESS;
    }

    merged = gsl_syncobj_new((*dst)->count + src->count);
    if (!merged) return GSL_FAILURE_OUTOFMEM;

    for (uint32_t i = 0; i < (*dst)->count; i++, n++) {
        merged->points[n].tl = gsl_tl_get((*dst)->points[i].tl);
        merged->points[n].point = (*dst)->points[i].point;
    }
    for (uint32_t i = 0; i < src->count; i++, n++) {
        merged->points[n].tl = gsl_tl_get(src->points[i].tl);
        merged->points[n].point = src->points[i].point;
    }

    if ((*dst)->fd >= 0 || src->fd >= 0) {
        merged->fd = gsl_fd_merge((*dst)->fd, src->fd);
        if (merged->fd < 0) {
            gsl_syncobj_put(merged);
            return GSL_FAILURE_SYSTEMERROR;
        }
    }

    merged->user = (*dst)->user || src->user;
    gsl_syncobj_put(*dst);
    *dst = merged;

    return GSL_SUCCESS;
}

int gsl_command_insertfence(gsl_devhandle_t handle, gsl_ctxid_t id, struct gsl_syncobj* wait) {
    struct gsl_context* ctx;
    int ret;

    if (!wait) return GSL_FAILURE_BADPARAM;

    ctx = gsl_context_get(id);
    if (!ctx) return GSL_FAILURE_BADPARAM;

    pthread_mutex_lock(&ctx->lock);
    ret = gsl_syncobj_merge_into(&ctx->pending_wait, wait);
    pthread_mutex_unlock(&ctx->lock);

    gsl_context_put(ctx);
    return ret;
}

int gsl_command_freememontimestamp_pure(gsl_devhandle_t handle, gsl_ctxid_t id,
                                        struct gsl_memdesc* memdesc, uint32_t ts, uint32_t type) {
    struct gsl_context* ctx = gsl_context_get(id);
    struct gsl_sync_point point;
    int ret;

    if (!ctx) return gsl_memory_free_pure(memdesc);

    point.tl = ctx->tl;
    point.point = gsl_context_ts_point(ctx, ts);
    ret = gsl_memory_free_after(memdesc, &point);

    gsl_context_put(ctx);
    return ret;
}

int gsl_command_resetstatus(gsl_devhandle_t handle, uint32_t* status) {
    struct gsl_device* dev = gsl_device_get(handle);
    struct gsl_context* ctx;
    uint32_t faults = 0;

    if (!status) return GSL_FAILURE_BADPARAM;
    if (!dev) {
        *status = 0;
        return GSL_SUCCESS;
    }

    ctx = gsl_context_get(*status);
    if (!ctx) return GSL_FAILURE_BADPARAM;

    struct drm_msm_submitqueue_query query = {
            .data = (uintptr_t)&faults,
            .id = ctx->queue,
            .param = MSM_SUBMITQUEUE_PARAM_FAULTS,
            .len = sizeof(faults),
    };

    gsl_ioctl(dev, DRM_IOCTL_MSM_SUBMITQUEUE_QUERY, &query);
    if (faults) {
        __atomic_store_n(&dev->lost, true, __ATOMIC_RELAXED);
        *status = GSL_GUILTY_CONTEXT_RESET;
    } else if (__atomic_load_n(&dev->lost, __ATOMIC_RELAXED)) {
        *status = GSL_INNOCENT_CONTEXT_RESET;
    } else {
        *status = 0;
    }

    gsl_context_put(ctx);
    return GSL_SUCCESS;
}
