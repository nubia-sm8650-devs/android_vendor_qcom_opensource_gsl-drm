/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <linux/dma-buf.h>

#include "mock.h"

struct gsl_mock mock;
struct gsl_device test_device;
uint32_t g_alogDebugMask;
static uint32_t next_handle = 1;

int os_alog(int level, const char* tag, const char* file, int line, const char* func,
            const char* fmt, ...) {
    return 0;
}

void* get_panel_settings(void) {
    return NULL;
}

int clock_gettime(clockid_t clock, struct timespec* ts) {
    if (mock.fake_clock && clock == CLOCK_MONOTONIC) {
        ts->tv_sec = (time_t)(mock.now_ns / 1000000000);
        ts->tv_nsec = (long)(mock.now_ns % 1000000000);
        return 0;
    }
    return (int)syscall(SYS_clock_gettime, clock, ts);
}

int usleep(useconds_t usec) {
    mock.sleeps++;
    return 0;
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    if (mock.fail_dup && cmd == F_DUPFD_CLOEXEC) {
        errno = EMFILE;
        return -1;
    }
    return syscall(SYS_fcntl, fd, cmd, arg);
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    mock.ioctl_calls++;
    if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT) mock.timeline_waits++;
    if (request == DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE) mock.fd_imports++;

    if (request == DRM_IOCTL_SYNCOBJ_CREATE) {
        struct drm_syncobj_create* req = arg;
        if (req->flags & DRM_SYNCOBJ_CREATE_SIGNALED) mock.signaled_creates++;
        req->handle = next_handle++;
    } else if (request == DRM_IOCTL_SYNCOBJ_DESTROY) {
        mock.syncobj_destroys++;
    } else if (request == DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE && mock.fd_import_errno) {
        errno = mock.fd_import_errno;
        return -1;
    } else if (request == DRM_IOCTL_MSM_SUBMITQUEUE_NEW) {
        ((struct drm_msm_submitqueue*)arg)->id = next_handle++;
    } else if (request == DRM_IOCTL_SYNCOBJ_QUERY) {
        struct drm_syncobj_timeline_array* req = arg;
        uint64_t* points = (void*)(uintptr_t)req->points;
        mock.queries++;
        mock.queried_handles += req->count_handles;
        if (mock.query_errno) {
            errno = mock.query_errno;
            return -1;
        }
        for (uint32_t i = 0; i < req->count_handles; i++) points[i] = mock.completed;
    } else if (request == DRM_IOCTL_MSM_GET_PARAM) {
        ((struct drm_msm_param*)arg)->value = 4;
    } else if (request == DRM_IOCTL_MSM_VM_BIND) {
        struct drm_msm_vm_bind* req = arg;
        mock.bind_calls++;
        mock.bind_nin = req->nr_in_syncobjs;
        mock.bind_queue = req->queue_id;
        assert(mock.bind_nin <= 16);
        if (mock.bind_nin)
            memcpy(mock.bind_in, (void*)(uintptr_t)req->in_syncobjs,
                   mock.bind_nin * sizeof(mock.bind_in[0]));
        if (mock.bind_errno) {
            errno = mock.bind_errno;
            return -1;
        }
    } else if (request == DRM_IOCTL_MSM_GEM_SUBMIT) {
        struct drm_msm_gem_submit* req = arg;
        mock.submit_calls++;
        if (mock.on_submit) {
            void (*hook)(void) = mock.on_submit;
            mock.on_submit = NULL;
            hook();
        }
        mock.submit_nin = req->nr_in_syncobjs;
        mock.submit_nout = req->nr_out_syncobjs;
        mock.submit_flags = req->flags;
        mock.submit_fence_fd = req->fence_fd;
        assert(mock.submit_nout <= 16);
        if (mock.submit_nin)
            memcpy(mock.submit_in, (void*)(uintptr_t)req->in_syncobjs,
                   (mock.submit_nin < 16 ? mock.submit_nin : 16) * sizeof(mock.submit_in[0]));
        memcpy(mock.submit_out, (void*)(uintptr_t)req->out_syncobjs,
               mock.submit_nout * sizeof(mock.submit_out[0]));
        if (mock.check_submit) mock.check_submit(req);
        if (mock.submit_timeouts) {
            mock.submit_timeouts--;
            errno = EBUSY;
            return -1;
        }
    } else if (request == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
        ((struct drm_prime_handle*)arg)->handle = 50;
    } else if (request == DMA_BUF_IOCTL_SYNC) {
        if (mock.sync_interrupts) {
            mock.sync_interrupts--;
            errno = EINTR;
            return -1;
        }
        assert(mock.sync_calls < 16);
        mock.sync_flags[mock.sync_calls++] = ((struct dma_buf_sync*)arg)->flags;
        if (mock.sync_errno) {
            errno = mock.sync_errno;
            return -1;
        }
    } else if (request == DRM_IOCTL_SYNCOBJ_TRANSFER) {
        struct drm_syncobj_transfer* req = arg;
        mock.transfers++;
        mock.transfer_flags |= req->flags;
        if (mock.point_pending) {
            errno = EINVAL;
            return -1;
        }
    } else if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT && mock.fence_unsignaled) {
        struct drm_syncobj_timeline_wait* req = arg;
        if (req->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) {
            mock.poll_waits++;
            errno = ETIME;
            return -1;
        }
        mock.block_waits++;
        if (mock.block_timeouts) {
            mock.block_timeouts--;
            errno = ETIME;
            return -1;
        }
        mock.fence_unsignaled = false;
    } else if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT && mock.point_pending) {
        struct drm_syncobj_timeline_wait* req = arg;
        mock.available_waits++;
        mock.wait_flags |= req->flags;
        if (mock.pending_wait_timeouts) {
            mock.pending_wait_timeouts--;
            errno = ETIME;
            return -1;
        }
        mock.point_pending = false;
    } else if (request == DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD) {
        ((struct drm_syncobj_handle*)arg)->fd = dup(fd);
    }
    return 0;
}

void test_init(void) {
    memset(&mock, 0, sizeof(mock));
    memset(&test_device, 0, sizeof(test_device));
    test_device.fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(test_device.fd >= 0);
    test_device.gen = 1;
    test_device.bind_syncobj = next_handle++;
    pthread_mutex_init(&test_device.bind_lock, NULL);
    assert(!gsl_vma_init(&test_device.vma, 0x100000000ull, 0x100000000ull));
    gsl_lib.device = &test_device;
}

void test_fini(void) {
    gsl_contexts_stop();
    gsl_contexts_reset(false);
    gsl_deferred_reset();
    gsl_imports_reset();
    gsl_vma_fini(&test_device.vma);
    pthread_mutex_destroy(&test_device.bind_lock);
    close(test_device.fd);
    gsl_lib.device = NULL;
}
