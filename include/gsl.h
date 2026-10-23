/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define GSL_SUCCESS 0
#define GSL_FAILURE -1
#define GSL_FAILURE_SYSTEMERROR -2
#define GSL_FAILURE_DEVICEERROR -3
#define GSL_FAILURE_OUTOFMEM -4
#define GSL_FAILURE_BADPARAM -5
#define GSL_FAILURE_NOTSUPPORTED -6
#define GSL_FAILURE_NOMOREAVAILABLE -7
#define GSL_FAILURE_NOTINITIALIZED -8
#define GSL_FAILURE_ALREADYINITIALIZED -9
#define GSL_FAILURE_TIMEOUT -10
#define GSL_FAILURE_OFFSETINVALID -11
#define GSL_FAILURE_CTXT_DEADLOCK -12
#define GSL_FAILURE_PERFCOUNTER_UNAVAILABLE -13
#define GSL_FAILURE_HANG -14
#define GSL_FAILURE_RETRY -15

#define GSL_DEVICE_3D 1
#define GSL_DEVICE_MAX 3

#define GSL_TIMEOUT_INFINITE 0xffffffffu

#define GSL_MEMFLAGS_PROTECTED 0x00000008u
#define GSL_MEMFLAGS_ALIGN_SHIFT 16
#define GSL_MEMFLAGS_ALIGN_MASK 0x00ff0000u
#define GSL_MEMFLAGS_GPUACCESS_MASK 0x03000000u
#define GSL_MEMFLAGS_GPUREADONLY 0x01000000u
#define GSL_MEMFLAGS_GPUWRITEONLY 0x02000000u
#define GSL_MEMFLAGS_GPUNOACCESS 0x03000000u
#define GSL_MEMFLAGS_CACHEMODE_MASK 0x0c000000u
#define GSL_MEMFLAGS_WRITECOMBINE 0x00000000u
#define GSL_MEMFLAGS_UNCACHED 0x04000000u
#define GSL_MEMFLAGS_WRITETHROUGH 0x08000000u
#define GSL_MEMFLAGS_WRITEBACK 0x0c000000u
#define GSL_MEMFLAGS_USE_CPU_MAP 0x10000000u

#define GSL_CACHEFLAGS_CLEAN 0x1u
#define GSL_CACHEFLAGS_INVALIDATE 0x2u
#define GSL_CACHEFLAGS_FLUSH (GSL_CACHEFLAGS_CLEAN | GSL_CACHEFLAGS_INVALIDATE)

#define GSL_CONTEXT_FLAG_USER_GENERATED_TS 0x00000080u
#define GSL_CONTEXT_FLAG_CLIENT_GENERATED_TS 0x80000000u

enum gsl_timestamp_type {
    GSL_TIMESTAMP_CONSUMED = 1,
    GSL_TIMESTAMP_RETIRED = 2,
    GSL_TIMESTAMP_QUEUED = 3,
};

enum gsl_systemtime_usage {
    GSL_SYSTEMTIME_GENERIC = 0,
    GSL_SYSTEMTIME_CL_PROFILING = 1,
};

typedef uint32_t gsl_devhandle_t;
typedef uint32_t gsl_ctxid_t;
typedef uint32_t gsl_timestamp_t;

struct gsl_memdesc {
    union {
        void* hostptr;
        uint64_t hostptr64;
    };
    uint64_t gpuaddr;
    uint64_t size;
    uint64_t flags;
    void* priv;
};

_Static_assert(sizeof(struct gsl_memdesc) == 0x28, "gsl_memdesc layout");

struct gsl_devinfo {
    uint32_t device_id;
    uint32_t chip_id;
    uint32_t mmu_enabled;
    uint32_t reserved;
    uint64_t gmem_gpubaseaddr;
    uint32_t gpu_id;
    uint32_t gmem_sizebytes;
};

_Static_assert(sizeof(struct gsl_devinfo) == 0x20, "gsl_devinfo layout");

#define GSL_DEVINFO_GMEM_VABASE 0x1u
#define GSL_DEVINFO_UCODE_INFO 0x2u
#define GSL_DEVINFO_BITNESS 0x4u
#define GSL_DEVINFO_HIGHEST_BANK_BIT 0x8u
#define GSL_DEVINFO_UBWC_MODE 0x10u
#define GSL_DEVINFO_MIN_ACCESS_LENGTH 0x20u
#define GSL_DEVINFO_QDSS_STM 0x40u
#define GSL_DEVINFO_QTIMER 0x80u
#define GSL_DEVINFO_MAX_IBS 0x200u
#define GSL_DEVINFO_SECURE_CTXT 0x400u
#define GSL_DEVINFO_SECURE_BUF_ALIGN 0x800u

#define GSL_DEVINFO2_SPEED_BIN 0x1u
#define GSL_DEVINFO2_GAMING_BIN 0x4u
#define GSL_DEVINFO2_GPU_MODEL 0x8u
#define GSL_DEVINFO2_VK_DEVICE_ID 0x10u
#define GSL_DEVINFO2_SECURE_VA_SIZE 0x20u

enum gsl_ubwc_mode {
    GSL_UBWC_1_0 = 0,
    GSL_UBWC_2_0 = 1,
    GSL_UBWC_3_0 = 2,
    GSL_UBWC_4_0 = 4,
    GSL_UBWC_5_0 = 7,
};

struct gsl_devinfo_ext {
    uint32_t device_id;
    uint32_t chip_id;
    uint32_t version;
    uint32_t reserved0;
    uint64_t gmem_gpubaseaddr;
    uint64_t gmem_sizebytes;
    uint64_t flags;
    uint64_t gmem_vabase;
    uint32_t ucode_info[2];
    uint64_t reserved1;
    uint32_t bitness;
    uint32_t highest_bank_bit;
    uint32_t ubwc_mode;
    uint32_t min_access_length;
    uint32_t reserved2;
    uint32_t reserved3;
    uint64_t qdss_stm[2];
    uint64_t qtimer[2];
    uint32_t reserved4;
    uint32_t max_ibs;
    uint32_t secure_ctxt;
    uint32_t secure_buf_align;
    uint32_t size;
    uint32_t reserved5;
    uint64_t flags2;
    uint32_t speed_bin;
    uint32_t reserved6;
    uint32_t gaming_bin;
    char gpu_model[64];
    uint32_t vk_device_id;
    uint64_t secure_va_size;
};

_Static_assert(offsetof(struct gsl_devinfo_ext, flags) == 0x20, "devinfo_ext flags");
_Static_assert(offsetof(struct gsl_devinfo_ext, ucode_info) == 0x30, "devinfo_ext ucode");
_Static_assert(offsetof(struct gsl_devinfo_ext, bitness) == 0x40, "devinfo_ext bitness");
_Static_assert(offsetof(struct gsl_devinfo_ext, qdss_stm) == 0x58, "devinfo_ext qdss");
_Static_assert(offsetof(struct gsl_devinfo_ext, max_ibs) == 0x7c, "devinfo_ext max_ibs");
_Static_assert(offsetof(struct gsl_devinfo_ext, size) == 0x88, "devinfo_ext size");
_Static_assert(offsetof(struct gsl_devinfo_ext, flags2) == 0x90, "devinfo_ext flags2");
_Static_assert(offsetof(struct gsl_devinfo_ext, gaming_bin) == 0xa0, "devinfo_ext gaming");
_Static_assert(offsetof(struct gsl_devinfo_ext, gpu_model) == 0xa4, "devinfo_ext model");
_Static_assert(offsetof(struct gsl_devinfo_ext, vk_device_id) == 0xe4, "devinfo_ext vk id");
_Static_assert(offsetof(struct gsl_devinfo_ext, secure_va_size) == 0xe8, "devinfo_ext va");

struct gsl_features {
    uint64_t size;
    uint32_t version;
    uint32_t device_id;
    uint32_t chip_id;
    uint32_t lpac;
    uint32_t reserved0;
    uint32_t fastblend;
    uint32_t raytracing;
    uint32_t reserved1[11];
    uint64_t uche_trap_base;
};

_Static_assert(offsetof(struct gsl_features, lpac) == 0x14, "features lpac");
_Static_assert(offsetof(struct gsl_features, fastblend) == 0x1c, "features fastblend");
_Static_assert(offsetof(struct gsl_features, raytracing) == 0x20, "features raytracing");
_Static_assert(offsetof(struct gsl_features, uche_trap_base) == 0x50, "features trap");
_Static_assert(sizeof(struct gsl_features) == 0x58, "features size");
