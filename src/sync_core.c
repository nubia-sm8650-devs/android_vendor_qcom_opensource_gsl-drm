/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>

#include "gsl_priv.h"

uint64_t gsl_abs_timeout_ns(uint32_t timeout_ms) {
    struct timespec now;

    if (timeout_ms == GSL_TIMEOUT_INFINITE) return INT64_MAX;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec +
           (uint64_t)timeout_ms * 1000000;
}

struct gsl_timeline_obj* gsl_tl_create(struct gsl_device* dev) {
    struct drm_syncobj_create req = {0};
    struct gsl_timeline_obj* tl;

    tl = calloc(1, sizeof(*tl));
    if (!tl) return NULL;

    if (gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_CREATE, &req)) {
        free(tl);
        return NULL;
    }
    tl->handle = req.handle;
    tl->gen = dev->gen;
    tl->refs = 1;

    return tl;
}

struct gsl_timeline_obj* gsl_tl_get(struct gsl_timeline_obj* tl) {
    __atomic_add_fetch(&tl->refs, 1, __ATOMIC_ACQ_REL);

    return tl;
}

void gsl_tl_put(struct gsl_device* dev, struct gsl_timeline_obj* tl) {
    struct drm_syncobj_destroy req;

    if (!tl || __atomic_sub_fetch(&tl->refs, 1, __ATOMIC_ACQ_REL)) return;

    if (!dev) dev = gsl_device_default();

    if (gsl_device_owns(dev, tl->gen)) {
        req.handle = tl->handle;
        req.pad = 0;
        gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_DESTROY, &req);
    }
    free(tl);
}

int gsl_tl_query(struct gsl_device* dev, struct gsl_timeline_obj* tl, uint64_t* value) {
    struct drm_syncobj_timeline_array req = {
            .handles = (uintptr_t)&tl->handle,
            .points = (uintptr_t)value,
            .count_handles = 1,
    };

    *value = 0;

    return gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_QUERY, &req);
}

void gsl_tl_mark_available(struct gsl_timeline_obj* tl, uint64_t point) {
    uint64_t cur = __atomic_load_n(&tl->available, __ATOMIC_RELAXED);

    while (cur < point && !__atomic_compare_exchange_n(&tl->available, &cur, point, true,
                                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
}

bool gsl_tl_available(const struct gsl_timeline_obj* tl, uint64_t point) {
    return __atomic_load_n(&tl->available, __ATOMIC_ACQUIRE) >= point;
}

bool gsl_syncobj_points_available(const struct gsl_syncobj* obj) {
    for (uint32_t i = 0; obj && i < obj->count; i++) {
        if (obj->points[i].point && !gsl_tl_available(obj->points[i].tl, obj->points[i].point))
            return false;
    }

    return true;
}

struct gsl_syncobj* gsl_syncobj_new(uint32_t count) {
    struct gsl_syncobj* obj = calloc(1, sizeof(*obj));

    if (!obj) return NULL;

    obj->refs = 1;
    obj->fd = -1;
    if (count) {
        obj->points = calloc(count, sizeof(*obj->points));
        if (!obj->points) {
            free(obj);
            return NULL;
        }
    }
    obj->count = count;

    return obj;
}

struct gsl_syncobj* gsl_syncobj_get(struct gsl_syncobj* obj) {
    __atomic_add_fetch(&obj->refs, 1, __ATOMIC_ACQ_REL);

    return obj;
}

void gsl_syncobj_put(struct gsl_syncobj* obj) {
    struct gsl_device* dev = gsl_device_default();

    if (!obj || __atomic_sub_fetch(&obj->refs, 1, __ATOMIC_ACQ_REL)) return;

    for (uint32_t i = 0; i < obj->count; i++) gsl_tl_put(dev, obj->points[i].tl);
    if (obj->fd >= 0) close(obj->fd);
    free(obj->points);
    free(obj);
}

int gsl_fd_merge(int a, int b) {
    struct sync_merge_data merge = {.name = "gsl", .fd2 = b};

    if (a < 0) return b >= 0 ? fcntl(b, F_DUPFD_CLOEXEC, 0) : -1;
    if (b < 0) return fcntl(a, F_DUPFD_CLOEXEC, 0);

    if (ioctl(a, SYNC_IOC_MERGE, &merge) < 0) return -1;

    return merge.fence;
}

int gsl_sync_points_signaled(struct gsl_device* dev, const struct gsl_sync_point* points,
                             uint32_t count, bool* signaled) {
    *signaled = true;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t value;
        int ret = gsl_tl_query(dev, points[i].tl, &value);

        if (ret) return ret;
        if (value < points[i].point) {
            *signaled = false;
            break;
        }
    }

    return GSL_SUCCESS;
}

static int gsl_import_sync_file(struct gsl_device* dev, int fd, uint32_t* handle) {
    struct drm_syncobj_create create = {0};
    struct drm_syncobj_handle import = {
            .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
            .fd = fd,
    };
    struct drm_syncobj_destroy destroy = {0};
    int ret;

    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_CREATE, &create);
    if (ret) return ret;

    import.handle = create.handle;
    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import);
    if (ret) {
        destroy.handle = create.handle;
        gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
        return ret;
    }
    *handle = create.handle;

    return GSL_SUCCESS;
}

int gsl_sync_deps_add_point(struct gsl_sync_deps* deps, uint32_t handle, uint64_t point) {
    struct drm_msm_syncobj* in = realloc(deps->in, (deps->count + 1) * sizeof(*in));

    if (!in) return GSL_FAILURE_OUTOFMEM;

    in[deps->count].handle = handle;
    in[deps->count].flags = 0;
    in[deps->count].point = point;
    deps->in = in;
    deps->count++;

    return GSL_SUCCESS;
}

int gsl_sync_deps_add_points(struct gsl_sync_deps* deps, const struct gsl_syncobj* obj) {
    for (uint32_t i = 0; obj && i < obj->count; i++) {
        int ret;

        if (!obj->points[i].point) continue;

        ret = gsl_sync_deps_add_point(deps, obj->points[i].tl->handle, obj->points[i].point);
        if (ret) return ret;
    }

    return GSL_SUCCESS;
}

int gsl_sync_deps_add(struct gsl_device* dev, struct gsl_sync_deps* deps,
                      const struct gsl_syncobj* obj) {
    int ret;

    if (!obj) return GSL_SUCCESS;

    ret = gsl_sync_deps_add_points(deps, obj);
    if (ret) return ret;

    if (obj->fd >= 0) {
        uint32_t handle, *temps;

        ret = gsl_import_sync_file(dev, obj->fd, &handle);
        if (ret) return ret;

        temps = realloc(deps->temps, (deps->ntemps + 1) * sizeof(*temps));
        if (!temps) {
            struct drm_syncobj_destroy destroy = {.handle = handle};

            gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
            return GSL_FAILURE_OUTOFMEM;
        }
        temps[deps->ntemps++] = handle;
        deps->temps = temps;

        ret = gsl_sync_deps_add_point(deps, handle, 0);
        if (ret) return ret;
    }

    return GSL_SUCCESS;
}

void gsl_sync_deps_fini(struct gsl_device* dev, struct gsl_sync_deps* deps) {
    for (uint32_t i = 0; i < deps->ntemps; i++) {
        struct drm_syncobj_destroy destroy = {.handle = deps->temps[i]};

        gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    free(deps->temps);
    free(deps->in);
    memset(deps, 0, sizeof(*deps));
}

int gsl_sync_deps_wait(struct gsl_device* dev, const struct gsl_sync_deps* deps, uint32_t flags,
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
            .flags = flags,
    };
    ret = gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait);

    free(handles);
    free(points);
    return ret;
}

static int gsl_fd_wait(int fd, uint64_t deadline) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    for (;;) {
        uint64_t now = gsl_abs_timeout_ns(0), left = deadline > now ? deadline - now : 0;
        struct timespec timeout = {
                .tv_sec = (time_t)(left / 1000000000),
                .tv_nsec = (long)(left % 1000000000),
        };
        int ret = ppoll(&pfd, 1, deadline == INT64_MAX ? NULL : &timeout, NULL);

        if (ret > 0) return pfd.revents & POLLNVAL ? GSL_FAILURE_BADPARAM : GSL_SUCCESS;
        if (!ret) return GSL_FAILURE_TIMEOUT;
        if (errno != EINTR && errno != EAGAIN) return gsl_errno_status(errno);
    }
}

int gsl_syncobj_wait_internal(struct gsl_device* dev, struct gsl_syncobj* obj,
                              uint32_t timeout_ms) {
    uint64_t deadline = gsl_abs_timeout_ns(timeout_ms);
    struct gsl_sync_deps deps = {0};
    int ret;

    ret = gsl_sync_deps_add_points(&deps, obj);
    if (!ret)
        ret = gsl_sync_deps_wait(
                dev, &deps,
                DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, deadline);
    gsl_sync_deps_fini(dev, &deps);

    if (!ret && obj->fd >= 0) ret = gsl_fd_wait(obj->fd, deadline);

    return ret;
}

int gsl_syncobj_add_pending(struct gsl_device* dev, const struct gsl_syncobj* obj,
                            struct gsl_sync_deps* deps) {
    for (uint32_t i = 0; i < obj->count; i++) {
        const struct gsl_sync_point* point = &obj->points[i];
        uint64_t value;
        int ret;

        if (!point->point) continue;

        ret = gsl_tl_query(dev, point->tl, &value);
        if (ret) return ret;
        if (value < point->point)
            return gsl_sync_deps_add_point(deps, point->tl->handle, point->point);
    }

    if (obj->fd >= 0) {
        struct gsl_syncobj fence = {.fd = obj->fd};

        return gsl_sync_deps_add(dev, deps, &fence);
    }

    return GSL_SUCCESS;
}
