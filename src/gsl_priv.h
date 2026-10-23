/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

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
    bool cached_coherent;

    uint32_t bind_queue;
    uint32_t bind_syncobj;
    uint64_t bind_point;
    pthread_mutex_t bind_lock;

    struct gsl_vma vma;
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

int gsl_ioctl(struct gsl_device* dev, unsigned long request, void* arg);
int gsl_errno_status(int err);
int gsl_get_param(struct gsl_device* dev, uint32_t param, uint64_t* value);

int gsl_vma_init(struct gsl_vma* vma, uint64_t start, uint64_t size);
void gsl_vma_fini(struct gsl_vma* vma);
uint64_t gsl_vma_alloc(struct gsl_vma* vma, uint64_t size, uint32_t align_shift);
void gsl_vma_free(struct gsl_vma* vma, uint64_t addr, uint64_t size);

int gsl_bind(struct gsl_device* dev, uint32_t op, uint32_t handle, uint64_t offset, uint64_t iova,
             uint64_t range);
