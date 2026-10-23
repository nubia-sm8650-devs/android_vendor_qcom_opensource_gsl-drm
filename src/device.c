/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/msm_drm.h>

#include "gsl_priv.h"

struct gsl_library gsl_lib = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
};

struct gsl_chip_gpu_id {
    uint8_t core;
    uint8_t major;
    uint16_t minor;
    uint32_t gpu_id;
};

#define GSL_CHIP_ANY_MINOR 0x100

/* Chip to GPU id table of the shipped libgsl, Adreno 6xx and later */
static const struct gsl_chip_gpu_id gsl_gpu_ids[] = {
        {0x07, 0x00, 0x02, 702},      {0x07, 0x01, 0x00, 710},      {0x07, 0x03, 0x00, 730},
        {0x43, 0x01, 0x01, 0x430065}, {0x43, 0x02, 0x00, 0x4300c8}, {0x43, 0x02, 0x01, 0x4300c9},
        {0x43, 0x03, 0x0a, 0x430136}, {0x43, 0x03, 0x0b, 0x430137}, {0x43, 0x03, 0x0c, 0x430138},
        {0x43, 0x05, 0x0a, 0x4301fe}, {0x43, 0x05, 0x0b, 0x4301ff}, {0x43, 0x05, 0x0c, 0x430200},
        {0x43, 0x05, 0x14, 0x430208}, {0x44, 0x01, 0x00, 0x440064}, {0x44, 0x03, 0x00, 0x44012c},
        {0x44, 0x05, 0x00, 0x4401f4}, {0x44, 0x05, 0x01, 0x4401f5}, {0x44, 0x05, 0x0a, 0x4401fe},
        {0x44, 0x07, 0x00, 0x4402bc},
};

struct gsl_chip_name {
    uint32_t chip_first;
    uint32_t chip_last;
    const char* name;
};

static const struct gsl_chip_name gsl_chip_names[] = {
        {0x07000200, 0x07000201, "Adreno (TM) 702"}, {0x07010000, 0x07010000, "Adreno (TM) 710"},
        {0x07030000, 0x07030002, "Adreno (TM) 730"}, {0x43010100, 0x43010100, "Adreno (TM) 711"},
        {0x43020000, 0x43020000, "Adreno (TM) 720"}, {0x43020100, 0x43020100, "Adreno (TM) 720"},
        {0x43030a00, 0x43030a00, "Adreno (TM) 725"}, {0x43030b00, 0x43030c00, "Adreno (TM) 726"},
        {0x43050a00, 0x43050b00, "Adreno (TM) 740"}, {0x43050c00, 0x43050c01, "Adreno (TM) 741"},
        {0x43051400, 0x43051401, "Adreno (TM) 750"}, {0x44010000, 0x44010000, "Adreno (TM) 830"},
        {0x44030000, 0x44030000, "Adreno (TM) 820"}, {0x44050000, 0x44050001, "Adreno (TM) 830"},
        {0x44050100, 0x44050100, "Adreno (TM) 830"}, {0x44050a20, 0x44050a30, "Adreno (TM) 840"},
        {0x44070020, 0x44070020, "Adreno (TM) 600"}, {0x44070030, 0x44070040, "Adreno (TM) 880"},
};

int gsl_errno_status(int err) {
    switch (err) {
        case 0:
            return GSL_SUCCESS;
        case EAGAIN:
            return GSL_FAILURE_RETRY;
        case ENOMEM:
            return GSL_FAILURE_OUTOFMEM;
        case EBUSY:
        case ETIME:
        case ETIMEDOUT:
            return GSL_FAILURE_TIMEOUT;
        case ENODEV:
            return GSL_FAILURE_PERFCOUNTER_UNAVAILABLE;
        case EINVAL:
            return GSL_FAILURE_BADPARAM;
        case ENOTTY:
            return GSL_FAILURE_NOTSUPPORTED;
        case EPIPE:
            return GSL_FAILURE_HANG;
        default:
            return GSL_FAILURE_SYSTEMERROR;
    }
}

int gsl_ioctl(struct gsl_device* dev, unsigned long request, void* arg) {
    int ret;

    do {
        ret = ioctl(dev->fd, request, arg);
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));

    return ret < 0 ? gsl_errno_status(errno) : GSL_SUCCESS;
}

int gsl_get_param(struct gsl_device* dev, uint32_t param, uint64_t* value) {
    struct drm_msm_param req = {
            .pipe = MSM_PIPE_3D0,
            .param = param,
    };
    int ret = gsl_ioctl(dev, DRM_IOCTL_MSM_GET_PARAM, &req);

    if (!ret) *value = req.value;

    return ret;
}

static int gsl_set_param(struct gsl_device* dev, uint32_t param, uint64_t value) {
    struct drm_msm_param req = {
            .pipe = MSM_PIPE_3D0,
            .param = param,
            .value = value,
    };

    return gsl_ioctl(dev, DRM_IOCTL_MSM_SET_PARAM, &req);
}

static int gsl_open_render_node(void) {
    struct dirent* entry;
    DIR* dir = opendir("/dev/dri");
    int fd = -1;

    if (!dir) return -1;

    while ((entry = readdir(dir))) {
        struct drm_version version = {0};
        char path[256], name[16] = {0};

        if (strncmp(entry->d_name, "renderD", 7)) continue;

        snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        version.name = name;
        version.name_len = sizeof(name) - 1;
        if (!ioctl(fd, DRM_IOCTL_VERSION, &version) && !strcmp(name, "msm")) break;

        close(fd);
        fd = -1;
    }
    closedir(dir);

    if (fd < 0) GSL_ERR("Error!! Unable to open DRM device.");

    return fd;
}

int gsl_bind_ops(struct gsl_device* dev, const struct drm_msm_vm_bind_op* ops, uint32_t count,
                 const struct drm_msm_syncobj* in, uint32_t nin) {
    struct drm_msm_syncobj out = {.handle = dev->bind_syncobj};
    struct drm_msm_vm_bind req = {
            .nr_ops = count,
            .queue_id = dev->bind_queue,
            .in_syncobjs = (uintptr_t)in,
            .out_syncobjs = (uintptr_t)&out,
            .nr_in_syncobjs = nin,
            .nr_out_syncobjs = 1,
            .syncobj_stride = sizeof(out),
            .op_stride = sizeof(*ops),
    };
    int ret;

    if (!count) return GSL_SUCCESS;

    if (count == 1)
        req.op = ops[0];
    else
        req.ops = (uintptr_t)ops;

    pthread_mutex_lock(&dev->bind_lock);
    out.point = dev->bind_point + 1;
    ret = gsl_ioctl(dev, DRM_IOCTL_MSM_VM_BIND, &req);
    if (!ret) dev->bind_point = out.point;
    pthread_mutex_unlock(&dev->bind_lock);

    if (ret)
        GSL_ERR("VM_BIND of %u ops at 0x%llx failed %d", count, (unsigned long long)ops[0].iova,
                ret);

    return ret;
}

int gsl_bind(struct gsl_device* dev, uint32_t op, uint32_t handle, uint64_t offset, uint64_t iova,
             uint64_t range) {
    struct drm_msm_vm_bind_op bind = {
            .op = op,
            .handle = handle,
            .obj_offset = offset,
            .iova = iova,
            .range = range,
    };

    return gsl_bind_ops(dev, &bind, 1, NULL, 0);
}

int gsl_bind_unmap(struct gsl_device* dev, uint64_t iova, uint64_t range) {
    return gsl_bind(dev, MSM_VM_BIND_OP_UNMAP, 0, 0, iova, range);
}

static void gsl_device_destroy(struct gsl_device* dev) {
    if (dev->bind_syncobj) {
        struct drm_syncobj_destroy destroy = {.handle = dev->bind_syncobj};

        ioctl(dev->fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    if (dev->bind_queue) {
        uint32_t id = dev->bind_queue;

        ioctl(dev->fd, DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, &id);
    }
    gsl_vma_fini(&dev->vma);
    pthread_mutex_destroy(&dev->bind_lock);
    close(dev->fd);
    free(dev);
}

static struct gsl_device* gsl_device_create(void) {
    struct drm_msm_submitqueue queue = {.flags = MSM_SUBMITQUEUE_VM_BIND};
    struct drm_syncobj_create syncobj = {0};
    struct gsl_device* dev;
    uint64_t value;

    dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;

    pthread_mutex_init(&dev->bind_lock, NULL);
    dev->fd = gsl_open_render_node();
    if (dev->fd < 0) {
        pthread_mutex_destroy(&dev->bind_lock);
        free(dev);
        return NULL;
    }

    if (gsl_set_param(dev, MSM_PARAM_EN_VM_BIND, 1)) {
        GSL_ERR("Failure enabling VM_BIND");
        goto err;
    }

    if (gsl_get_param(dev, MSM_PARAM_CHIP_ID, &value)) {
        GSL_ERR("Failure getting CHIPID from kernel");
        goto err;
    }
    dev->chip_id = (uint32_t)value;

    if (!gsl_get_param(dev, MSM_PARAM_GMEM_SIZE, &value)) dev->gmem_size = (uint32_t)value;
    if (!gsl_get_param(dev, MSM_PARAM_GMEM_BASE, &value)) dev->gmem_base = value;
    if (!gsl_get_param(dev, MSM_PARAM_HIGHEST_BANK_BIT, &value))
        dev->highest_bank_bit = (uint32_t)value;
    if (!gsl_get_param(dev, MSM_PARAM_UBWC_SWIZZLE, &value)) dev->ubwc_swizzle = (uint32_t)value;
    if (!gsl_get_param(dev, MSM_PARAM_MACROTILE_MODE, &value))
        dev->macrotile_mode = (uint32_t)value;
    if (!gsl_get_param(dev, MSM_PARAM_RAYTRACING, &value)) dev->raytracing = (uint32_t)value;
    if (!gsl_get_param(dev, MSM_PARAM_UCHE_TRAP_BASE, &value)) dev->uche_trap_base = value;
    if (!gsl_get_param(dev, MSM_PARAM_HAS_PRR, &value)) dev->has_prr = value;

    if (gsl_get_param(dev, MSM_PARAM_VA_START, &dev->va_start) ||
        gsl_get_param(dev, MSM_PARAM_VA_SIZE, &dev->va_size)) {
        GSL_ERR("Failure getting the GPU VA range from kernel");
        goto err;
    }

    if (gsl_ioctl(dev, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &queue)) {
        GSL_ERR("Failure creating the VM_BIND queue");
        goto err;
    }
    dev->bind_queue = queue.id;

    if (gsl_ioctl(dev, DRM_IOCTL_SYNCOBJ_CREATE, &syncobj)) {
        GSL_ERR("Failure creating the VM_BIND syncobj");
        goto err;
    }
    dev->bind_syncobj = syncobj.handle;

    if (gsl_vma_init(&dev->vma, dev->va_start, dev->va_size)) goto err;

    return dev;

err:
    close(dev->fd);
    pthread_mutex_destroy(&dev->bind_lock);
    free(dev);
    return NULL;
}

struct gsl_device* gsl_device_get(gsl_devhandle_t handle) {
    if (handle != GSL_HANDLE_3D && handle != 0) return NULL;

    return gsl_lib.device;
}

struct gsl_device* gsl_device_default(void) {
    return gsl_lib.device;
}

const char* gsl_library_version(void) {
    return "1.0.10";
}

static void gsl_library_reset(void) {
    if (gsl_lib.device) {
        gsl_reap_deferred(gsl_lib.device, true);
        gsl_device_destroy(gsl_lib.device);
    }
    gsl_lib.device = NULL;
    gsl_lib.refs = 0;
}

int gsl_library_open(uint32_t flags) {
    pid_t pid = getpid();
    int ret = GSL_SUCCESS;

    pthread_mutex_lock(&gsl_lib.lock);
    if (gsl_lib.refs && gsl_lib.pid == pid) {
        gsl_lib.refs++;
        gsl_lib.flags |= (flags >> 8 & 8) << 8;
        goto out;
    }

    if (gsl_lib.refs) {
        GSL_WARN("forked from pid %d", gsl_lib.pid);
        gsl_library_reset();
    }

    get_panel_settings();
    gsl_lib.pid = pid;
    gsl_lib.flags = flags & 0x10002800;
    gsl_lib.device = gsl_device_create();
    if (!gsl_lib.device) {
        ret = GSL_FAILURE_SYSTEMERROR;
        goto out;
    }
    gsl_lib.refs = 1;

out:
    pthread_mutex_unlock(&gsl_lib.lock);
    return ret;
}

int gsl_library_close(void) {
    int ret = GSL_SUCCESS;

    pthread_mutex_lock(&gsl_lib.lock);
    if (!gsl_lib.refs) {
        ret = GSL_FAILURE_NOTINITIALIZED;
    } else if (--gsl_lib.refs == 0 || gsl_lib.pid != getpid()) {
        gsl_library_reset();
    }
    pthread_mutex_unlock(&gsl_lib.lock);

    return ret;
}

gsl_devhandle_t gsl_device_open(uint32_t device_id, uint32_t flags) {
    gsl_devhandle_t handle = 0;

    if (device_id != GSL_DEVICE_3D) return 0;

    pthread_mutex_lock(&gsl_lib.lock);
    if (gsl_lib.device) {
        gsl_lib.device->refs++;
        handle = GSL_HANDLE_3D;
    }
    pthread_mutex_unlock(&gsl_lib.lock);

    return handle;
}

int gsl_device_close(gsl_devhandle_t handle) {
    struct gsl_device* dev;

    pthread_mutex_lock(&gsl_lib.lock);
    dev = gsl_device_get(handle);
    if (dev && dev->refs) dev->refs--;
    pthread_mutex_unlock(&gsl_lib.lock);

    return GSL_SUCCESS;
}

static uint32_t gsl_lookup_gpu_id(uint32_t chip_id) {
    uint8_t core = chip_id >> 24, major = chip_id >> 16, minor = chip_id >> 8;

    for (size_t i = 0; i < sizeof(gsl_gpu_ids) / sizeof(gsl_gpu_ids[0]); i++) {
        const struct gsl_chip_gpu_id* id = &gsl_gpu_ids[i];

        if (id->core == core && id->major == major &&
            (id->minor == GSL_CHIP_ANY_MINOR || id->minor == minor))
            return id->gpu_id;
    }

    return 0;
}

int gsl_device_getinfo(gsl_devhandle_t handle, struct gsl_devinfo* info) {
    struct gsl_device* dev = gsl_device_get(handle);

    memset(info, 0, sizeof(*info));
    if (!dev) return GSL_FAILURE_BADPARAM;

    info->device_id = GSL_DEVICE_3D;
    info->chip_id = dev->chip_id;
    info->mmu_enabled = 1;
    info->gpu_id = gsl_lookup_gpu_id(dev->chip_id);
    info->gmem_sizebytes = dev->gmem_size;

    return GSL_SUCCESS;
}

int gsl_device_getinfo_ext(gsl_devhandle_t handle, struct gsl_devinfo_ext* info) {
    struct gsl_device* dev = gsl_device_get(handle);

    memset(info, 0, offsetof(struct gsl_devinfo_ext, size));
    if (!dev) return GSL_FAILURE_BADPARAM;

    info->device_id = GSL_DEVICE_3D;
    info->chip_id = dev->chip_id;
    info->gmem_sizebytes = dev->gmem_size;
    info->gmem_vabase = dev->gmem_base;
    info->bitness = 48;
    info->flags = GSL_DEVINFO_GMEM_VABASE | GSL_DEVINFO_BITNESS;

    if (dev->highest_bank_bit) {
        info->highest_bank_bit = dev->highest_bank_bit;
        info->flags |= GSL_DEVINFO_HIGHEST_BANK_BIT;
    }

    /* Upstream msm has no UBWC version param, and every Adreno 7xx encodes UBWC 4.0 */
    if (dev->highest_bank_bit && (dev->chip_id >> 24) == 0x43) {
        info->ubwc_mode = GSL_UBWC_4_0;
        info->flags |= GSL_DEVINFO_UBWC_MODE;
    }

    info->min_access_length = 32;
    info->flags |= GSL_DEVINFO_MIN_ACCESS_LENGTH;
    info->max_ibs = 8;
    info->flags |= GSL_DEVINFO_MAX_IBS;
    info->reserved2 = 1;

    if (info->size >= offsetof(struct gsl_devinfo_ext, vk_device_id) + sizeof(uint32_t)) {
        info->flags2 = GSL_DEVINFO2_VK_DEVICE_ID;
        info->vk_device_id = dev->chip_id;
    }

    return GSL_SUCCESS;
}

int gsl_device_getname(gsl_devhandle_t handle, const struct gsl_devinfo_ext* info, char* name,
                       size_t size) {
    const char* found = NULL;
    size_t len;

    for (size_t i = 0; i < sizeof(gsl_chip_names) / sizeof(gsl_chip_names[0]); i++) {
        if (info->chip_id >= gsl_chip_names[i].chip_first &&
            info->chip_id <= gsl_chip_names[i].chip_last) {
            found = gsl_chip_names[i].name;
            break;
        }
    }
    if (!found) return GSL_FAILURE_DEVICEERROR;
    if (!size) return GSL_SUCCESS;

    len = strlen(found);
    if (len > size - 1) len = size - 1;
    memcpy(name, found, len);
    name[len] = '\0';

    return GSL_SUCCESS;
}

int gsl_device_getfeatures(gsl_devhandle_t handle, struct gsl_features* features, uint64_t size) {
    struct gsl_device* dev = gsl_device_get(handle);

    memset(features, 0, sizeof(*features));
    if (!dev) return GSL_FAILURE_BADPARAM;
    if (size < sizeof(*features)) return GSL_SUCCESS;

    features->size = sizeof(*features);
    features->version = 5;
    features->device_id = GSL_DEVICE_3D;
    features->chip_id = dev->chip_id;
    features->raytracing = dev->raytracing;
    features->uche_trap_base = dev->uche_trap_base ? dev->uche_trap_base : 0x1fffffffff000ull;

    return GSL_SUCCESS;
}

uint64_t gsl_get_system_time(uint32_t usage) {
    struct timespec ts;
    clockid_t clock;

    switch (usage) {
        case GSL_SYSTEMTIME_GENERIC:
            clock = CLOCK_MONOTONIC;
            break;
        case GSL_SYSTEMTIME_CL_PROFILING:
            clock = CLOCK_MONOTONIC_RAW;
            break;
        default:
            GSL_ERR("Invalid system time usage %d", usage);
            return 0;
    }

    if (clock_gettime(clock, &ts)) return 0;

    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

enum {
    GSL_TIME_DOMAIN_DEVICE = 0,
    GSL_TIME_DOMAIN_MONOTONIC = 1,
    GSL_TIME_DOMAIN_MONOTONIC_RAW = 2,
};

static uint64_t gsl_clock_ns(clockid_t clock) {
    struct timespec ts;

    clock_gettime(clock, &ts);

    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

int gsl_read_timestamp(gsl_devhandle_t handle, uint32_t count, const uint32_t* domains,
                       uint64_t* timestamps, uint64_t* deviation) {
    struct gsl_device* dev = gsl_device_get(handle);
    uint64_t start, end;

    if (!dev || !domains || !timestamps) return GSL_FAILURE_BADPARAM;

    start = gsl_clock_ns(CLOCK_MONOTONIC_RAW);
    for (uint32_t i = 0; i < count; i++) {
        switch (domains[i]) {
            case GSL_TIME_DOMAIN_DEVICE:
                if (gsl_get_param(dev, MSM_PARAM_TIMESTAMP, &timestamps[i]))
                    return GSL_FAILURE_DEVICEERROR;
                break;
            case GSL_TIME_DOMAIN_MONOTONIC:
                timestamps[i] = gsl_clock_ns(CLOCK_MONOTONIC);
                break;
            case GSL_TIME_DOMAIN_MONOTONIC_RAW:
                timestamps[i] = gsl_clock_ns(CLOCK_MONOTONIC_RAW);
                break;
            default:
                return GSL_FAILURE_BADPARAM;
        }
    }
    end = gsl_clock_ns(CLOCK_MONOTONIC_RAW);

    if (deviation) *deviation = end - start;

    return GSL_SUCCESS;
}

int gsl_present(void) {
    return GSL_SUCCESS;
}

int gsl_set_constraint(gsl_devhandle_t handle, void* constraint) {
    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_set_pwrctrl(gsl_devhandle_t handle, uint32_t enable) {
    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_get_secure_memory_info(uint64_t* info) {
    return GSL_FAILURE_NOTSUPPORTED;
}

uint32_t gsl_ion_heap_id_mask(void) {
    return 0;
}

int gsl_get_fault_info(gsl_devhandle_t handle, gsl_ctxid_t ctx, void* info) {
    return GSL_FAILURE_NOTSUPPORTED;
}
