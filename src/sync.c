/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>

#include "gsl_priv.h"

#define GSL_TIMELINE_WAIT_ANY 2

struct gsl_syncsource {
    int refs;
};

struct gsl_membind_region {
    uint64_t target_offset;
    const struct gsl_memdesc* phys;
    uint64_t child_offset;
    uint64_t length;
    uint64_t flags;
};

struct gsl_membind {
    const struct gsl_memdesc* virt;
    const struct gsl_membind_region* regions;
    uint64_t count;
};

static int gsl_syncobj_from_point(struct gsl_timeline_obj* tl, uint64_t point,
                                  struct gsl_syncobj** out) {
    struct gsl_syncobj* obj = gsl_syncobj_new(1);

    if (!obj) return GSL_FAILURE_OUTOFMEM;

    obj->points[0].tl = gsl_tl_get(tl);
    obj->points[0].point = point;
    *out = obj;

    return GSL_SUCCESS;
}

int gsl_syncobj_create(struct gsl_syncobj** out, gsl_devhandle_t handle, gsl_ctxid_t id,
                       uint32_t ts) {
    struct gsl_context* ctx;
    int ret;

    if (!out) return GSL_FAILURE_BADPARAM;
    *out = NULL;

    ctx = gsl_context_get(id);
    if (!ctx) return GSL_FAILURE_BADPARAM;

    ret = gsl_syncobj_from_point(ctx->tl, gsl_context_ts_point(ctx, ts), out);

    gsl_context_put(ctx);
    return ret;
}

void gsl_syncobj_destroy(struct gsl_syncobj* obj) {
    gsl_syncobj_put(obj);
}

int gsl_syncobj_wait(struct gsl_syncobj* obj, uint32_t timeout_ms) {
    struct gsl_device* dev = gsl_device_default();

    if (!obj || !dev) return GSL_FAILURE_BADPARAM;

    return gsl_syncobj_wait_internal(dev, obj, timeout_ms);
}

int gsl_syncobj_wait_multiple(struct gsl_syncobj** objs, int* results, uint64_t count,
                              uint32_t timeout_ms) {
    struct gsl_device* dev = gsl_device_default();
    uint64_t deadline = gsl_abs_timeout_ns(timeout_ms);

    if (!objs || !results || !count || !dev) return GSL_FAILURE_BADPARAM;

    for (;;) {
        struct timespec now;
        int ret = GSL_FAILURE_TIMEOUT;

        for (uint64_t i = 0; i < count; i++) {
            results[i] =
                    objs[i] ? gsl_syncobj_wait_internal(dev, objs[i], 0) : GSL_FAILURE_BADPARAM;
            if (!results[i])
                ret = GSL_SUCCESS;
            else if (results[i] != GSL_FAILURE_TIMEOUT && ret == GSL_FAILURE_TIMEOUT)
                ret = results[i];
        }
        if (ret != GSL_FAILURE_TIMEOUT || !timeout_ms) return ret;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec >= deadline) return ret;

        usleep(1000);
    }
}

int gsl_syncobj_clone(struct gsl_syncobj** out, struct gsl_syncobj* obj) {
    if (!out) return GSL_FAILURE_BADPARAM;

    if (!obj) {
        *out = NULL;
        return GSL_FAILURE_BADPARAM;
    }
    *out = gsl_syncobj_get(obj);

    return GSL_SUCCESS;
}

int gsl_syncobj_merge_multiple(struct gsl_syncobj** out, struct gsl_syncobj** objs,
                               uint64_t count) {
    struct gsl_syncobj* merged = NULL;
    int ret;

    if (!out || !objs || !count) return GSL_FAILURE_BADPARAM;
    *out = NULL;

    for (uint64_t i = 0; i < count; i++) {
        if (!objs[i]) continue;

        ret = gsl_syncobj_merge_into(&merged, objs[i]);
        if (ret) {
            gsl_syncobj_put(merged);
            return ret;
        }
    }
    if (!merged) return GSL_FAILURE_BADPARAM;

    *out = merged;

    return GSL_SUCCESS;
}

int gsl_syncobj_merge(struct gsl_syncobj** out, struct gsl_syncobj* a, struct gsl_syncobj* b) {
    struct gsl_syncobj* objs[] = {a, b};

    return gsl_syncobj_merge_multiple(out, objs, 2);
}

int gsl_syncsource_create(struct gsl_syncsource** out) {
    struct gsl_syncsource* src;

    if (!out) return GSL_FAILURE_BADPARAM;

    src = calloc(1, sizeof(*src));
    if (!src) return GSL_FAILURE_OUTOFMEM;
    src->refs = 1;
    *out = src;

    return GSL_SUCCESS;
}

void gsl_syncsource_destroy(struct gsl_syncsource* src) {
    if (src && !__atomic_sub_fetch(&src->refs, 1, __ATOMIC_ACQ_REL)) free(src);
}

int gsl_syncobj_create_from_source(struct gsl_syncobj** out, struct gsl_syncsource* src) {
    struct gsl_device* dev = gsl_device_default();
    struct gsl_timeline_obj* tl;
    int ret;

    if (!out || !src || !dev) return GSL_FAILURE_BADPARAM;
    *out = NULL;

    tl = gsl_tl_create(dev);
    if (!tl) return GSL_FAILURE_OUTOFMEM;

    ret = gsl_syncobj_from_point(tl, 1, out);
    if (!ret) (*out)->user = true;
    gsl_tl_put(dev, tl);

    return ret;
}

static int gsl_tl_signal(struct gsl_device* dev, struct gsl_timeline_obj* tl, uint64_t value) {
    struct drm_syncobj_timeline_array req = {
            .handles = (uintptr_t)&tl->handle,
            .points = (uintptr_t)&value,
            .count_handles = 1,
    };

    return gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &req);
}

int gsl_syncobj_signal(struct gsl_syncobj* obj) {
    struct gsl_device* dev = gsl_device_default();

    if (!obj || !dev) return GSL_FAILURE_BADPARAM;
    if (!obj->user) return GSL_FAILURE_NOTSUPPORTED;

    for (uint32_t i = 0; i < obj->count; i++) {
        int ret = gsl_tl_signal(dev, obj->points[i].tl, obj->points[i].point);

        if (ret) return ret;
    }

    return GSL_SUCCESS;
}

int gsl_syncobj_create_from_fd(struct gsl_syncobj** out, int fd) {
    struct gsl_syncobj* obj;

    if (!out || fd < 0) {
        GSL_ERR("bad param (%p, %d)", out, fd);
        return GSL_FAILURE_BADPARAM;
    }
    *out = NULL;

    obj = gsl_syncobj_new(0);
    if (!obj) return GSL_FAILURE_OUTOFMEM;
    obj->fd = fd;
    *out = obj;

    return GSL_SUCCESS;
}

static int gsl_point_to_fd(struct gsl_device* dev, const struct gsl_sync_point* point, int* fd) {
    struct drm_syncobj_create create = {0};
    struct drm_syncobj_destroy destroy = {0};
    struct drm_syncobj_transfer transfer;
    struct drm_syncobj_handle export;
    int ret;

    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_CREATE, &create);
    if (ret) return ret;

    transfer = (struct drm_syncobj_transfer){
            .src_handle = point->tl->handle,
            .dst_handle = create.handle,
            .src_point = point->point,
            .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
    };
    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer);
    if (!ret) {
        export = (struct drm_syncobj_handle){
                .handle = create.handle,
                .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
                .fd = -1,
        };
        ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export);
        if (!ret) *fd = export.fd;
    }

    destroy.handle = create.handle;
    gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);

    return ret;
}

static int gsl_signaled_fd(struct gsl_device* dev, int* fd) {
    struct drm_syncobj_create create = {.flags = DRM_SYNCOBJ_CREATE_SIGNALED};
    struct drm_syncobj_destroy destroy = {0};
    struct drm_syncobj_handle export;
    int ret;

    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_CREATE, &create);
    if (ret) return ret;

    export = (struct drm_syncobj_handle){
            .handle = create.handle,
            .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
            .fd = -1,
    };
    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export);
    if (!ret) *fd = export.fd;

    destroy.handle = create.handle;
    gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);

    return ret;
}

int gsl_syncobj_export_fd(struct gsl_device* dev, const struct gsl_syncobj* obj, int* out) {
    int fd = obj->fd >= 0 ? fcntl(obj->fd, F_DUPFD_CLOEXEC, 0) : -1;

    for (uint32_t i = 0; i < obj->count; i++) {
        int point_fd, merged, ret;

        if (!obj->points[i].point) continue;

        ret = gsl_point_to_fd(dev, &obj->points[i], &point_fd);

        if (ret) {
            if (fd >= 0) close(fd);
            return ret;
        }

        merged = gsl_fd_merge(fd, point_fd);
        if (fd >= 0) close(fd);
        close(point_fd);
        if (merged < 0) return GSL_FAILURE_SYSTEMERROR;
        fd = merged;
    }

    if (fd < 0) return gsl_signaled_fd(dev, out);

    *out = fd;

    return GSL_SUCCESS;
}

int gsl_syncobj_dup_fd(struct gsl_syncobj* obj, int* fd) {
    struct gsl_device* dev = gsl_device_default();

    if (!obj || !fd || !dev) return GSL_FAILURE_BADPARAM;
    *fd = -1;

    return gsl_syncobj_export_fd(dev, obj, fd);
}

int gsl_syncobj_external_create(void) {
    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_syncobj_external_signal(void* obj, void* data, gsl_ctxid_t id, uint32_t flags) {
    return GSL_FAILURE_NOTSUPPORTED;
}

static int gsl_ctx_ts_fd(gsl_ctxid_t id, uint32_t ts, int* fd) {
    struct gsl_device* dev = gsl_device_default();
    struct gsl_syncobj* obj;
    int ret;

    if (!fd) return GSL_FAILURE_BADPARAM;
    *fd = -1;
    if (!dev) return GSL_FAILURE_NOTINITIALIZED;

    ret = gsl_syncobj_create(&obj, GSL_HANDLE_3D, id, ts);
    if (ret) return ret;

    ret = gsl_syncobj_export_fd(dev, obj, fd);
    gsl_syncobj_put(obj);

    return ret;
}

int gsl_device_3d_add_fence_event(gsl_ctxid_t id, uint32_t ts, int* fd) {
    return gsl_ctx_ts_fd(id, ts, fd);
}

int gsl_device_add_fence_event(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t ts, int* fd) {
    return gsl_ctx_ts_fd(id, ts, fd);
}

int gsl_timeline_create(gsl_devhandle_t handle, struct gsl_timeline_obj** out, uint64_t initial,
                        uint32_t flags) {
    struct gsl_device* dev = gsl_device_get(handle);
    struct gsl_timeline_obj* tl;
    int ret;

    if (!out) return GSL_FAILURE_BADPARAM;
    *out = NULL;
    if (!dev) return GSL_FAILURE_BADPARAM;

    tl = gsl_tl_create(dev);
    if (!tl) return GSL_FAILURE_OUTOFMEM;

    if (initial) {
        ret = gsl_tl_signal(dev, tl, initial);
        if (ret) {
            gsl_tl_put(dev, tl);
            return ret;
        }
    }
    *out = tl;

    return GSL_SUCCESS;
}

int gsl_timeline_destroy(struct gsl_timeline_obj* tl) {
    if (!tl) return GSL_FAILURE_BADPARAM;

    gsl_tl_put(gsl_device_default(), tl);

    return GSL_SUCCESS;
}

int gsl_timeline_signal(gsl_devhandle_t handle, struct gsl_timeline_obj* tl, uint64_t value) {
    struct gsl_device* dev = gsl_device_get(handle);

    if (!dev || !tl) return GSL_FAILURE_BADPARAM;

    return gsl_tl_signal(dev, tl, value);
}

int gsl_timeline_query(struct gsl_timeline_obj* tl, uint64_t* value) {
    struct gsl_device* dev = gsl_device_default();

    if (!dev || !tl || !value) return GSL_FAILURE_BADPARAM;

    return gsl_tl_query(dev, tl, value);
}

int gsl_multiple_wait_timelines(gsl_devhandle_t handle, struct gsl_timeline_obj** timelines,
                                const uint64_t* values, uint32_t count, uint64_t timeout_ns,
                                uint32_t flags) {
    struct gsl_device* dev = gsl_device_get(handle);
    struct timespec now;
    uint32_t* handles;
    int64_t deadline;
    int ret;

    if (!dev || !timelines || !values || !count) return GSL_FAILURE_BADPARAM;

    handles = calloc(count, sizeof(*handles));
    if (!handles) return GSL_FAILURE_OUTOFMEM;
    for (uint32_t i = 0; i < count; i++) {
        if (!timelines[i]) {
            free(handles);
            return GSL_FAILURE_BADPARAM;
        }
        handles[i] = timelines[i]->handle;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    deadline = timeout_ns > (uint64_t)(INT64_MAX - deadline) ? INT64_MAX
                                                             : deadline + (int64_t)timeout_ns;

    struct drm_syncobj_timeline_wait wait = {
            .handles = (uintptr_t)handles,
            .points = (uintptr_t)values,
            .timeout_nsec = deadline,
            .count_handles = count,
            .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                     (flags == GSL_TIMELINE_WAIT_ANY ? 0 : DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL),
    };

    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait);
    free(handles);

    return ret;
}

int gsl_timeline_export_shared_handle(void) {
    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_aux_command(gsl_devhandle_t handle, gsl_ctxid_t id, uint32_t* ts, uint32_t flags,
                    struct gsl_syncobj* wait, const struct gsl_timeline_signal* signal) {
    if (!ts) return GSL_FAILURE_BADPARAM;

    return gsl_context_marker(handle, id, ts, wait, signal);
}

int gsl_memory_bind_sync(gsl_devhandle_t handle, gsl_ctxid_t id, const struct gsl_membind* binds,
                         uint64_t count, struct gsl_syncobj* wait,
                         const struct gsl_timeline_signal* signal, uint32_t* ts) {
    struct gsl_device* dev = gsl_device_get(handle);
    struct drm_msm_vm_bind_op* ops = NULL;
    struct gsl_sync_deps deps = {0};
    uint32_t nops = 0;
    int ret;

    if (!dev || !binds || !count || !ts) return GSL_FAILURE_BADPARAM;

    for (uint64_t i = 0; i < count; i++) {
        const struct gsl_bo* virt = binds[i].virt ? binds[i].virt->priv : NULL;
        struct drm_msm_vm_bind_op* grown;

        if (!virt || virt->type != GSL_BO_VIRT || !gsl_device_owns(dev, virt->gen) ||
            !binds[i].regions) {
            ret = GSL_FAILURE_BADPARAM;
            goto out;
        }

        grown = realloc(ops, (nops + binds[i].count) * sizeof(*ops));
        if (!grown) {
            ret = GSL_FAILURE_OUTOFMEM;
            goto out;
        }
        ops = grown;

        for (uint64_t r = 0; r < binds[i].count; r++) {
            const struct gsl_membind_region* region = &binds[i].regions[r];
            const struct gsl_bo* phys = region->phys ? region->phys->priv : NULL;
            struct drm_msm_vm_bind_op* op = &ops[nops++];

            if (phys && !gsl_device_owns(dev, phys->gen)) {
                ret = GSL_FAILURE_BADPARAM;
                goto out;
            }

            memset(op, 0, sizeof(*op));
            op->iova = virt->iova + region->target_offset;
            op->range = region->length;
            if (phys) {
                op->op = MSM_VM_BIND_OP_MAP;
                op->handle = phys->handle;
                op->obj_offset = region->child_offset;
            } else {
                op->op = dev->has_prr ? MSM_VM_BIND_OP_MAP_NULL : MSM_VM_BIND_OP_UNMAP;
            }
        }
    }

    ret = gsl_sync_deps_add(dev, &deps, wait);
    if (ret) goto out;

    ret = gsl_bind_ops(dev, ops, nops, deps.in, deps.count);
    if (ret) goto out;

    if (signal && signal->count) ret = gsl_context_marker(handle, id, ts, NULL, signal);

out:
    gsl_sync_deps_fini(dev, &deps);
    free(ops);
    return ret;
}
