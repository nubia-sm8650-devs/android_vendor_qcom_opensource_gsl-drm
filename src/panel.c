/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <cutils/properties.h>

#include "gsl_os.h"

#define PANEL_SIZE 0x358

#define PANEL_PREROTATION 0xa0
#define PANEL_MEMORY_CLASS 0xa4
#define PANEL_GMEM_SIZE 0xac
#define PANEL_DEBUG_MASK 0x208
#define PANEL_ASSERT_LOG 0x214
#define PANEL_NATIVE_ASSERT 0x218
#define PANEL_LOW_PERF_ASSERT 0x21c

static char panel_name[0x50] = "unknown";
static char panel_buf1[0xf0];
static char panel_buf2[0x100];

/* Defaults of the shipped libgsl, with no adreno_config.txt present */
static uint64_t panel[PANEL_SIZE / sizeof(uint64_t)] = {
        [0x01] = 0x1600812101080000,
        [0x02] = 0xc03010030042,
        [0x03] = 0x3b00000008,
        [0x04] = 0x1000000008,
        [0x05] = 0x1900800,
        [0x06] = 0x3,
        [0x08] = 0x1000000000,
        [0x0a] = 0x1,
        [0x0b] = (uintptr_t)"/data/vendor/gpu/resolve_log.txt",
        [0x0c] = (uintptr_t)"/data/vendor/gpu/glslvs.txt",
        [0x0d] = (uintptr_t)"/data/vendor/gpu/glslfs.txt",
        [0x10] = (uintptr_t)panel_name,
        [0x11] = (uintptr_t)"/data/vendor/gpu/scdevdump.txt",
        [0x13] = (uintptr_t)"/data/vendor/gpu/",
        [0x14] = 0x200000061,
        [0x16] = 0x1400000050,
        [0x17] = 0x2,
        [0x1a] = 0x100000000,
        [0x1d] = 0x2,
        [0x20] = 0x100000000,
        [0x25] = 0x271000000064,
        [0x26] = 0x4b00000032,
        [0x27] = 0x1,
        [0x2e] = 0x100000001,
        [0x34] = (uintptr_t)panel_buf1,
        [0x35] = 0x5000000050,
        [0x36] = 0xfffffff800000200,
        [0x37] = 0x3f80000000000007,
        [0x38] = 0x3f80000000000000,
        [0x39] = 0x3f8000003f800000,
        [0x3b] = 0x200000000000000,
        [0x3c] = 0x2000000,
        [0x3e] = 0x7fffffff00000000,
        [0x3f] = 0x7fffffff00000000,
        [0x40] = 0xffffffffffffffff,
        [0x45] = 0xffffffff00000000,
        [0x46] = 0xff00000000000000,
        [0x47] = 0x4,
        [0x48] = 0xffffffff,
        [0x4a] = (uintptr_t)"/mnt/sdcard/profiler_records",
        [0x4d] = 0x200000002,
        [0x4e] = 0x200000002,
        [0x4f] = 0x2,
        [0x56] = (uintptr_t)"/data/vendor/gpu/perfCounterDump.txt",
        [0x57] = 0x1,
        [0x58] = 0x10,
        [0x59] = 0x4479c000,
        [0x5e] = 0x100000003,
        [0x67] = (uintptr_t)panel_buf2,
        [0x68] = 0xffffffffffffffff,
        [0x69] = 0xffffffff00000000,
        [0x6a] = 0xffffffff,
};

static pthread_once_t panel_once = PTHREAD_ONCE_INIT;

static uint32_t* panel_u32(size_t offset) {
    return (uint32_t*)((uint8_t*)panel + offset);
}

static void panel_init(void) {
    char value[PROPERTY_VALUE_MAX];
    const char* shift;

    property_get("ro.vendor.graphics.memory", value, "high");
    if (!strcmp(value, "low"))
        *panel_u32(PANEL_MEMORY_CLASS) = 0;
    else if (!strcmp(value, "med"))
        *panel_u32(PANEL_MEMORY_CLASS) = 1;
    else if (!strcmp(value, "high"))
        *panel_u32(PANEL_MEMORY_CLASS) = 2;

    shift = getenv("YAMATO_FORCE_GMEM_SHL");
    if (shift) *panel_u32(PANEL_GMEM_SIZE) <<= atoi(shift) & 0x1f;

    g_alogAssertLog = *panel_u32(PANEL_ASSERT_LOG);
    g_alogDebugMask = *panel_u32(PANEL_DEBUG_MASK);
    g_enableNativeAssert = *panel_u32(PANEL_NATIVE_ASSERT);
    g_alogLowPerfAssertFlag = *panel_u32(PANEL_LOW_PERF_ASSERT);

    property_get("vendor.debug.prerotation.disable", value, "0");
    if (atoi(value) == 1) {
        GSL_ERR("vendor.debug.prerotation.disable set. Disable prerotation");
        *panel_u32(PANEL_PREROTATION) = 0;
    }
}

void* get_panel_settings(void) {
    pthread_once(&panel_once, panel_init);

    return panel;
}
