/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/msm_drm.h>

#include "gsl_priv.h"

#define GSL_MEMFLAGS_USERMEM_MASK 0x000000e0u
#define GSL_MEMFLAGS_USERMEM_DMABUF 0x00000080u
#define GSL_MEMFLAGS_MEMTYPE_MASK 0x0000ff00u
#define GSL_MEMTYPE_TEXTURE 0x00000600u

#define GSL_MEMTYPE_DMABUF 3

#define GSL_PAGE_SHIFT 12
#define GSL_PAGE_SIZE (1ull << GSL_PAGE_SHIFT)

#define GSL_SVM_ATTEMPTS 16

struct gsl_gem_ref {
    uint32_t handle;
    int refs;
    struct gsl_gem_ref* next;
};

static pthread_mutex_t gsl_import_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gsl_bo* gsl_imports;

static pthread_mutex_t gsl_gem_ref_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gsl_gem_ref* gsl_gem_refs;

static uint64_t gsl_page_align(uint64_t size) {
    return (size + GSL_PAGE_SIZE - 1) & ~(GSL_PAGE_SIZE - 1);
}

static uint32_t gsl_align_shift(uint64_t flags, uint64_t size) {
    uint32_t shift = ((flags & GSL_MEMFLAGS_ALIGN_MASK) >> GSL_MEMFLAGS_ALIGN_SHIFT) & 0x3f;

    if ((flags & GSL_MEMFLAGS_MEMTYPE_MASK) == GSL_MEMTYPE_TEXTURE && size >= 0x10000 && shift < 16)
        shift = 16;

    return shift < GSL_PAGE_SHIFT ? GSL_PAGE_SHIFT : shift;
}

static int gsl_bo_new(struct gsl_device* dev, uint64_t size, uint64_t flags, uint32_t* handle,
                      uint32_t* msm_flags) {
    struct drm_msm_gem_new req = {.size = size};
    int ret;

    if ((flags & GSL_MEMFLAGS_GPUACCESS_MASK) == GSL_MEMFLAGS_GPUREADONLY)
        req.flags |= MSM_BO_GPU_READONLY;

    switch (flags & GSL_MEMFLAGS_CACHEMODE_MASK) {
        case GSL_MEMFLAGS_WRITETHROUGH:
        case GSL_MEMFLAGS_WRITEBACK:
            if (!dev->cached_noncoherent) {
                req.flags |= MSM_BO_CACHED_COHERENT;
                ret = gsl_ioctl(dev, DRM_IOCTL_MSM_GEM_NEW, &req);
                if (ret != GSL_FAILURE_BADPARAM) goto out;
                dev->cached_noncoherent = true;
                req.flags &= ~MSM_BO_CACHED_COHERENT;
            }
            req.flags |= MSM_BO_CACHED;
            break;
        default:
            req.flags |= MSM_BO_WC;
            break;
    }
    ret = gsl_ioctl(dev, DRM_IOCTL_MSM_GEM_NEW, &req);

out:
    if (!ret) {
        *handle = req.handle;
        *msm_flags = req.flags;
    }

    return ret;
}

static void gsl_gem_close(struct gsl_device* dev, uint32_t handle) {
    struct drm_gem_close req = {.handle = handle};

    gsl_ioctl(dev, DRM_IOCTL_GEM_CLOSE, &req);
}

static int gsl_gem_import(struct gsl_device* dev, int fd, uint32_t* handle) {
    struct drm_prime_handle prime = {.fd = fd};
    struct gsl_gem_ref* ref;
    int ret;

    pthread_mutex_lock(&gsl_gem_ref_lock);
    ret = gsl_ioctl(dev, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
    if (ret) goto out;

    for (ref = gsl_gem_refs; ref; ref = ref->next) {
        if (ref->handle == prime.handle) break;
    }
    if (!ref) {
        ref = calloc(1, sizeof(*ref));
        if (!ref) {
            gsl_gem_close(dev, prime.handle);
            ret = GSL_FAILURE_OUTOFMEM;
            goto out;
        }
        ref->handle = prime.handle;
        ref->next = gsl_gem_refs;
        gsl_gem_refs = ref;
    }
    ref->refs++;
    *handle = prime.handle;

out:
    pthread_mutex_unlock(&gsl_gem_ref_lock);
    return ret;
}

static void gsl_gem_ref_put(struct gsl_device* dev, uint32_t handle) {
    struct gsl_gem_ref **link, *ref;
    bool close = true;

    pthread_mutex_lock(&gsl_gem_ref_lock);
    for (link = &gsl_gem_refs; (ref = *link); link = &ref->next) {
        if (ref->handle != handle) continue;

        if (--ref->refs) {
            close = false;
        } else {
            *link = ref->next;
            free(ref);
        }
        break;
    }
    if (close) gsl_gem_close(dev, handle);
    pthread_mutex_unlock(&gsl_gem_ref_lock);
}

static int gsl_bo_mmap_at(struct gsl_device* dev, struct gsl_bo* bo, void* addr) {
    struct drm_msm_gem_info info = {
            .handle = bo->handle,
            .info = MSM_INFO_GET_OFFSET,
    };
    void* map;
    int ret;

    ret = gsl_ioctl(dev, DRM_IOCTL_MSM_GEM_INFO, &info);
    if (ret) return ret;

    map = mmap(addr, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED | (addr ? MAP_FIXED : 0), dev->fd,
               info.value);

    if (map == MAP_FAILED) {
        GSL_ERR("mmap failed errno %d %s", errno, strerror(errno));
        return GSL_FAILURE_OUTOFMEM;
    }
    bo->map = map;

    return GSL_SUCCESS;
}

static int gsl_bo_mmap(struct gsl_device* dev, struct gsl_bo* bo) {
    return gsl_bo_mmap_at(dev, bo, NULL);
}

static int gsl_bo_map_gpu(struct gsl_device* dev, struct gsl_bo* bo, uint64_t size,
                          uint32_t align_shift) {
    int ret;

    bo->iova = gsl_vma_alloc(&dev->vma, size, align_shift);
    if (!bo->iova) return GSL_FAILURE_OUTOFMEM;
    bo->va_size = size;

    if (bo->handle) {
        ret = gsl_bind(dev, MSM_VM_BIND_OP_MAP, bo->handle, 0, bo->iova, size);
        if (ret) {
            gsl_vma_free(&dev->vma, bo->iova, size);
            bo->iova = 0;
            bo->va_size = 0;
            return ret;
        }
    }

    return GSL_SUCCESS;
}

static int gsl_bo_map_svm(struct gsl_device* dev, struct gsl_bo* bo, uint32_t align_shift) {
    uint64_t align = 1ull << align_shift;
    uint64_t span = bo->size + align - GSL_PAGE_SIZE;
    uint64_t va_end = dev->va_start + dev->va_size;
    void* busy[GSL_SVM_ATTEMPTS];
    unsigned int nbusy = 0;
    int ret = GSL_FAILURE_OUTOFMEM;

    while (nbusy < GSL_SVM_ATTEMPTS) {
        void* area =
                mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        uint64_t addr, head, tail;

        if (area == MAP_FAILED) break;

        addr = ((uintptr_t)area + align - 1) & ~(align - 1);
        if (addr < dev->va_start || addr + bo->size > va_end ||
            gsl_vma_reserve(&dev->vma, addr, bo->size)) {
            busy[nbusy++] = area;
            continue;
        }

        head = addr - (uintptr_t)area;
        tail = span - head - bo->size;
        ret = gsl_bo_mmap_at(dev, bo, (void*)(uintptr_t)addr);
        if (ret) {
            gsl_vma_free(&dev->vma, addr, bo->size);
            munmap(area, span);
            break;
        }
        if (head) munmap(area, head);
        if (tail) munmap((void*)(uintptr_t)(addr + bo->size), tail);

        ret = gsl_bind(dev, MSM_VM_BIND_OP_MAP, bo->handle, 0, addr, bo->size);
        if (ret) {
            gsl_vma_free(&dev->vma, addr, bo->size);
            break;
        }
        bo->iova = addr;
        bo->va_size = bo->size;
        break;
    }

    while (nbusy) munmap(busy[--nbusy], span);

    return ret;
}

void gsl_bo_destroy(struct gsl_device* dev, struct gsl_bo* bo) {
    if (bo->va_size) {
        gsl_bind_unmap(dev, bo->iova, bo->va_size);
        gsl_vma_free(&dev->vma, bo->iova, bo->va_size);
    }
    if (bo->map) munmap(bo->map, gsl_page_align(bo->size));
    if (bo->type == GSL_BO_IMPORT)
        gsl_gem_ref_put(dev, bo->handle);
    else if (bo->handle)
        gsl_gem_close(dev, bo->handle);
    if (bo->dmabuf_fd >= 0) close(bo->dmabuf_fd);
    free(bo);
}

void gsl_bo_forget(struct gsl_bo* bo) {
    if (bo->map) munmap(bo->map, gsl_page_align(bo->size));
    if (bo->dmabuf_fd >= 0) close(bo->dmabuf_fd);
    free(bo);
}

void gsl_imports_reset(void) {
    struct gsl_gem_ref* ref;

    pthread_mutex_lock(&gsl_import_lock);
    gsl_imports = NULL;
    pthread_mutex_unlock(&gsl_import_lock);

    pthread_mutex_lock(&gsl_gem_ref_lock);
    ref = gsl_gem_refs;
    gsl_gem_refs = NULL;
    pthread_mutex_unlock(&gsl_gem_ref_lock);

    while (ref) {
        struct gsl_gem_ref* next = ref->next;

        free(ref);
        ref = next;
    }
}

static void gsl_memdesc_clear(struct gsl_memdesc* memdesc) {
    memset(memdesc, 0, sizeof(*memdesc));
}

int gsl_memory_alloc_pure_64(uint64_t size, uint64_t flags, struct gsl_memdesc* memdesc) {
    struct gsl_device* dev = gsl_device_default();
    uint64_t aligned = gsl_page_align(size);
    struct gsl_bo* bo;
    int ret;

    if (!memdesc) return GSL_FAILURE_BADPARAM;
    gsl_memdesc_clear(memdesc);
    if (!dev || !size) return GSL_FAILURE_BADPARAM;

    if (flags & GSL_MEMFLAGS_PROTECTED) {
        GSL_ERR("DRM kernel compiled against doesn't support secure buffers");
        return GSL_FAILURE_BADPARAM;
    }

    bo = calloc(1, sizeof(*bo));
    if (!bo) return GSL_FAILURE_OUTOFMEM;
    bo->type = GSL_BO_NORMAL;
    bo->gen = dev->gen;
    bo->size = aligned;
    bo->refs = 1;
    bo->dmabuf_fd = -1;

    ret = gsl_bo_new(dev, aligned, flags, &bo->handle, &bo->msm_flags);
    if (ret) goto err;

    if (flags & GSL_MEMFLAGS_USE_CPU_MAP) {
        ret = gsl_bo_map_svm(dev, bo, gsl_align_shift(flags, aligned));
        if (ret) goto err;
    } else {
        ret = gsl_bo_mmap(dev, bo);
        if (ret) goto err;

        ret = gsl_bo_map_gpu(dev, bo, aligned, gsl_align_shift(flags, aligned));
        if (ret) goto err;
    }

    memdesc->hostptr = bo->map;
    memdesc->gpuaddr = bo->iova;
    memdesc->size = aligned;
    memdesc->flags = flags;
    memdesc->priv = bo;

    return GSL_SUCCESS;

err:
    GSL_ERR("ERROR: sharedmem alloc failed! Allocation size: (%llu KB); Flags: (0x%llx)",
            (unsigned long long)(size >> 10), (unsigned long long)flags);
    gsl_bo_destroy(dev, bo);
    return ret;
}

int gsl_memory_alloc_pure(uint32_t size, uint32_t flags, struct gsl_memdesc* memdesc) {
    return gsl_memory_alloc_pure_64(size, flags, memdesc);
}

static void gsl_bo_put(struct gsl_device* dev, struct gsl_bo* bo,
                       const struct gsl_sync_point* after) {
    if (bo->type == GSL_BO_IMPORT) {
        struct gsl_bo** link;

        pthread_mutex_lock(&gsl_import_lock);
        if (--bo->refs) {
            pthread_mutex_unlock(&gsl_import_lock);
            return;
        }
        for (link = &gsl_imports; *link; link = &(*link)->next) {
            if (*link == bo) {
                *link = bo->next;
                break;
            }
        }
        pthread_mutex_unlock(&gsl_import_lock);
    }

    gsl_bo_release(dev, bo, after);
}

int gsl_memory_free_after(struct gsl_memdesc* memdesc, const struct gsl_sync_point* after) {
    struct gsl_device* dev = gsl_device_default();
    struct gsl_bo* bo;

    if (!memdesc || !memdesc->priv) {
        GSL_ERR("Must free the memdesc populated by alloc");
        return GSL_FAILURE_BADPARAM;
    }
    bo = memdesc->priv;

    gsl_bo_put(dev, bo, after);
    gsl_memdesc_clear(memdesc);

    return GSL_SUCCESS;
}

int gsl_memory_free_pure(struct gsl_memdesc* memdesc) {
    return gsl_memory_free_after(memdesc, NULL);
}

int gsl_memory_import(uint64_t handle, uint64_t flags, struct gsl_memdesc* memdesc) {
    if (memdesc) gsl_memdesc_clear(memdesc);

    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_memory_map_ext_fd_pure(int fd, uint64_t hostptr, uint64_t len, uint64_t offset,
                               uint32_t memtype, struct gsl_memdesc* memdesc, uint32_t flags) {
    struct gsl_device* dev = gsl_device_default();
    uint64_t gsl_flags = flags | GSL_MEMFLAGS_USERMEM_DMABUF;
    struct gsl_bo* bo;
    uint32_t handle;
    off_t end;
    int ret;

    if (!memdesc || offset || memtype != GSL_MEMTYPE_DMABUF) return GSL_FAILURE_BADPARAM;
    gsl_memdesc_clear(memdesc);
    if (!dev) return GSL_FAILURE_NOTINITIALIZED;

    pthread_mutex_lock(&gsl_import_lock);
    ret = gsl_gem_import(dev, fd, &handle);
    if (ret) {
        pthread_mutex_unlock(&gsl_import_lock);
        return ret;
    }

    for (bo = gsl_imports; bo; bo = bo->next) {
        if (bo->handle == handle) break;
    }

    if (bo) {
        gsl_gem_ref_put(dev, handle);
        if (len > bo->size) {
            GSL_ERR("Import matched GEM handle, but params don't match");
            pthread_mutex_unlock(&gsl_import_lock);
            return GSL_FAILURE_BADPARAM;
        }
        bo->refs++;
        pthread_mutex_unlock(&gsl_import_lock);
        goto done;
    }

    bo = calloc(1, sizeof(*bo));
    if (!bo) {
        pthread_mutex_unlock(&gsl_import_lock);
        gsl_gem_ref_put(dev, handle);
        return GSL_FAILURE_OUTOFMEM;
    }
    bo->type = GSL_BO_IMPORT;
    bo->gen = dev->gen;
    bo->handle = handle;
    bo->refs = 1;
    bo->dmabuf_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (bo->dmabuf_fd < 0) {
        pthread_mutex_unlock(&gsl_import_lock);
        gsl_bo_destroy(dev, bo);
        return GSL_FAILURE_SYSTEMERROR;
    }

    end = lseek(fd, 0, SEEK_END);
    bo->size = end > 0 ? (uint64_t)end : gsl_page_align(len);

    ret = gsl_bo_map_gpu(dev, bo, bo->size, gsl_align_shift(flags, bo->size));
    if (ret) {
        pthread_mutex_unlock(&gsl_import_lock);
        gsl_bo_destroy(dev, bo);
        return ret;
    }

    bo->next = gsl_imports;
    gsl_imports = bo;
    pthread_mutex_unlock(&gsl_import_lock);

done:
    memdesc->hostptr = NULL;
    memdesc->gpuaddr = bo->iova;
    memdesc->size = len;
    memdesc->flags = gsl_flags;
    memdesc->priv = bo;

    return GSL_SUCCESS;
}

int gsl_memory_mmap(struct gsl_memdesc* memdesc, uint64_t offset, uint64_t size, uint64_t flags,
                    void** out) {
    struct gsl_device* dev = gsl_device_default();
    struct gsl_bo* bo;
    int ret;

    if (!out) return GSL_FAILURE_BADPARAM;
    *out = NULL;
    if (!memdesc || !memdesc->priv || !dev) return GSL_FAILURE_BADPARAM;
    if (memdesc->flags & GSL_MEMFLAGS_PROTECTED) {
        GSL_ERR("Can't mmap a PROTECTED buffer");
        return GSL_FAILURE_BADPARAM;
    }

    bo = memdesc->priv;
    if (!memdesc->hostptr) {
        if (bo->type == GSL_BO_IMPORT) {
            pthread_mutex_lock(&gsl_import_lock);
            if (!bo->map) {
                void* map = mmap(NULL, gsl_page_align(bo->size), PROT_READ | PROT_WRITE, MAP_SHARED,
                                 bo->dmabuf_fd, 0);

                if (map == MAP_FAILED) {
                    pthread_mutex_unlock(&gsl_import_lock);
                    GSL_ERR("mmap failed errno %d %s", errno, strerror(errno));
                    return GSL_FAILURE;
                }
                bo->map = map;
            }
            memdesc->hostptr = bo->map;
            pthread_mutex_unlock(&gsl_import_lock);
        } else {
            if (!bo->map) {
                if (!gsl_device_owns(dev, bo->gen)) return GSL_FAILURE_BADPARAM;
                ret = gsl_bo_mmap(dev, bo);
                if (ret) return ret;
            }
            memdesc->hostptr = bo->map;
        }
    }
    *out = (uint8_t*)memdesc->hostptr + offset;

    return GSL_SUCCESS;
}

int gsl_memory_munmap(struct gsl_memdesc* memdesc) {
    struct gsl_bo* bo;

    if (!memdesc || !memdesc->priv) return GSL_FAILURE_BADPARAM;

    bo = memdesc->priv;
    if (bo->type == GSL_BO_IMPORT) memdesc->hostptr = NULL;

    return GSL_SUCCESS;
}

static bool gsl_memdesc_range_ok(const struct gsl_memdesc* memdesc, uint64_t offset,
                                 uint64_t size) {
    return memdesc && memdesc->hostptr && offset + size >= offset && offset + size <= memdesc->size;
}

int gsl_memory_read(const struct gsl_memdesc* memdesc, void* dst, uint32_t size, uint32_t offset) {
    if (!dst || !gsl_memdesc_range_ok(memdesc, offset, size)) {
        GSL_ERR("invalid arg offset %u size %u", offset, size);
        return GSL_FAILURE;
    }

    memcpy(dst, (const uint8_t*)memdesc->hostptr + offset, size);

    return GSL_SUCCESS;
}

int gsl_memory_write(const struct gsl_memdesc* memdesc, const void* src, uint32_t size,
                     uint32_t offset) {
    if (!src || !gsl_memdesc_range_ok(memdesc, offset, size)) {
        GSL_ERR("invalid arg offset %u size %u", offset, size);
        return GSL_FAILURE;
    }

    memcpy((uint8_t*)memdesc->hostptr + offset, src, size);

    return GSL_SUCCESS;
}

int gsl_memory_set(const struct gsl_memdesc* memdesc, uint32_t offset, int value, uint32_t size) {
    if (!gsl_memdesc_range_ok(memdesc, offset, size)) {
        GSL_ERR("invalid arg offset %u size %u", offset, size);
        return GSL_FAILURE;
    }

    memset((uint8_t*)memdesc->hostptr + offset, value, size);

    return GSL_SUCCESS;
}

int gsl_memory_write_multiple(const struct gsl_memdesc* memdesc, const void* src,
                              uint32_t src_stride, uint32_t dst_stride, uint32_t size, int count,
                              uint32_t offset) {
    const uint8_t* in = src;
    int ret = 0;

    for (; count > 0; count--) {
        ret |= gsl_memory_write(memdesc, in, size, offset);
        offset += dst_stride;
        in += src_stride;
    }

    return ret;
}

int gsl_memory_read_multiple(void* dst, const struct gsl_memdesc* memdesc, uint32_t src_stride,
                             uint32_t dst_stride, uint32_t size, int count, uint32_t offset) {
    uint8_t* out = dst;
    int ret = 0;

    for (; count > 0; count--) {
        ret |= gsl_memory_read(memdesc, out, size, offset);
        offset += src_stride;
        out += dst_stride;
    }

    return ret;
}

static uint64_t gsl_dcache_line(void) {
    uint64_t ctr;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));

    return 4ull << ((ctr >> 16) & 0xf);
}

static void gsl_cache_maintain(void* addr, uint64_t size, uint32_t op) {
    uint64_t line = gsl_dcache_line();
    uintptr_t start = (uintptr_t)addr & ~(line - 1);
    uintptr_t end = (uintptr_t)addr + size;

    for (uintptr_t p = start; p < end; p += line) {
        if (op & GSL_CACHEFLAGS_INVALIDATE)
            __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
        else
            __asm__ volatile("dc cvac, %0" ::"r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static int gsl_dmabuf_sync(int fd, uint64_t flags) {
    struct dma_buf_sync sync = {.flags = flags};
    int ret;

    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));

    return ret < 0 ? gsl_errno_status(errno) : GSL_SUCCESS;
}

int gsl_memory_cacheoperation(const struct gsl_memdesc* memdesc, uint64_t offset, uint64_t size,
                              uint32_t op) {
    struct gsl_bo* bo;

    if (!memdesc || !memdesc->priv) return GSL_FAILURE_BADPARAM;
    if (!(op & GSL_CACHEFLAGS_FLUSH)) return GSL_FAILURE_BADPARAM;

    bo = memdesc->priv;
    if (bo->type == GSL_BO_IMPORT) {
        uint64_t mode = memdesc->flags & GSL_MEMFLAGS_CACHEMODE_MASK;
        int ret;

        if (offset >= memdesc->size) return GSL_FAILURE_BADPARAM;
        if (mode != GSL_MEMFLAGS_WRITEBACK && mode != GSL_MEMFLAGS_WRITETHROUGH) return GSL_SUCCESS;
        if (op & GSL_CACHEFLAGS_CLEAN) {
            ret = gsl_dmabuf_sync(bo->dmabuf_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
            if (ret) return ret;
        }
        if (op & GSL_CACHEFLAGS_INVALIDATE)
            return gsl_dmabuf_sync(bo->dmabuf_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);

        return GSL_SUCCESS;
    }
    if (!(bo->msm_flags & MSM_BO_CACHED) || !memdesc->hostptr) return GSL_SUCCESS;

    if (offset >= memdesc->size) return GSL_FAILURE_BADPARAM;
    if (!size || offset + size > memdesc->size) size = memdesc->size - offset;

    gsl_cache_maintain((uint8_t*)memdesc->hostptr + offset, size, op);

    return GSL_SUCCESS;
}

struct gsl_cache_op {
    const struct gsl_memdesc* memdesc;
    uint64_t offset;
    uint64_t size;
    uint32_t op;
    uint32_t reserved;
};

int gsl_memory_cacheoperation_bulk(const struct gsl_cache_op* ops, uint32_t count) {
    if (!ops || !count) return GSL_FAILURE_BADPARAM;

    for (uint32_t i = 0; i < count; i++) {
        int ret;

        if (!ops[i].memdesc || !ops[i].memdesc->priv) continue;

        ret = gsl_memory_cacheoperation(ops[i].memdesc, ops[i].offset, ops[i].size, ops[i].op);

        if (ret) return ret;
    }

    return GSL_SUCCESS;
}

int gsl_memory_getinfo_ext(uint32_t* info, uint32_t size, uint64_t* out_size) {
    if (!info) return GSL_FAILURE_BADPARAM;

    memset(info, 0, size);
    if (size >= sizeof(uint32_t)) info[0] = 1;
    if (size > sizeof(uint32_t)) info[1] = 1;
    if (out_size) *out_size = size < 8 ? size : 8;

    return GSL_SUCCESS;
}

int gsl_memory_set_metainfo(const struct gsl_memdesc* memdesc, uint32_t flags, const char* info,
                            uint32_t len) {
    struct gsl_device* dev = gsl_device_default();
    struct drm_msm_gem_info req = {.info = MSM_INFO_SET_NAME};
    struct gsl_bo* bo;
    char name[32];

    if (!memdesc || !memdesc->priv || !info) return GSL_FAILURE_BADPARAM;
    bo = memdesc->priv;
    if (!gsl_device_owns(dev, bo->gen) || !bo->handle) return GSL_SUCCESS;

    if (len >= sizeof(name)) len = sizeof(name) - 1;
    memcpy(name, info, len);
    name[len] = '\0';

    req.handle = bo->handle;
    req.value = (uintptr_t)name;
    req.len = strlen(name);
    gsl_ioctl(dev, DRM_IOCTL_MSM_GEM_INFO, &req);

    return GSL_SUCCESS;
}

int gsl_memory_notify(const struct gsl_memdesc* memdesc, uint32_t event, uint32_t data) {
    return GSL_SUCCESS;
}

int gsl_memory_bind(void) {
    return GSL_FAILURE_NOTSUPPORTED;
}

int gsl_memory_phys_alloc_pure(uint64_t size, uint64_t reserved, uint64_t* flags,
                               struct gsl_memdesc* memdesc) {
    struct gsl_device* dev = gsl_device_default();
    uint64_t aligned = gsl_page_align(size);
    struct gsl_bo* bo;
    int ret;

    if (!memdesc || !flags) return GSL_FAILURE_BADPARAM;
    gsl_memdesc_clear(memdesc);
    if (!dev || !size) return GSL_FAILURE_BADPARAM;
    if (*flags & GSL_MEMFLAGS_PROTECTED) return GSL_FAILURE_BADPARAM;

    bo = calloc(1, sizeof(*bo));
    if (!bo) return GSL_FAILURE_OUTOFMEM;
    bo->type = GSL_BO_PHYS;
    bo->gen = dev->gen;
    bo->size = aligned;
    bo->refs = 1;
    bo->dmabuf_fd = -1;

    ret = gsl_bo_new(dev, aligned, *flags, &bo->handle, &bo->msm_flags);
    if (ret) {
        free(bo);
        return ret;
    }

    memdesc->size = aligned;
    memdesc->flags = *flags;
    memdesc->priv = bo;

    return GSL_SUCCESS;
}

int gsl_memory_virt_alloc_pure(uint64_t size, uint32_t reserved, uint64_t* flags,
                               struct gsl_memdesc* memdesc) {
    struct gsl_device* dev = gsl_device_default();
    uint64_t aligned = gsl_page_align(size);
    uint32_t shift;
    struct gsl_bo* bo;
    int ret;

    if (!memdesc || !flags) return GSL_FAILURE_BADPARAM;
    gsl_memdesc_clear(memdesc);
    if (!dev || !size) return GSL_FAILURE_BADPARAM;

    bo = calloc(1, sizeof(*bo));
    if (!bo) return GSL_FAILURE_OUTOFMEM;
    bo->type = GSL_BO_VIRT;
    bo->gen = dev->gen;
    bo->size = aligned;
    bo->refs = 1;
    bo->dmabuf_fd = -1;

    shift = gsl_align_shift(*flags, aligned);
    ret = gsl_bo_map_gpu(dev, bo, aligned, shift < 16 ? 16 : shift);
    if (ret) {
        free(bo);
        return ret;
    }

    if (dev->has_prr) gsl_bind(dev, MSM_VM_BIND_OP_MAP_NULL, 0, 0, bo->iova, aligned);

    memdesc->gpuaddr = bo->iova;
    memdesc->size = aligned;
    memdesc->flags = *flags;
    memdesc->priv = bo;

    return GSL_SUCCESS;
}

int gsl_shared_memory_alloc(void) {
    return GSL_FAILURE_NOTSUPPORTED;
}
