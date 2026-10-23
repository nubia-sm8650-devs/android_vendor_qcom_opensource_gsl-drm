/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define GSL_LOG_TAG "Adreno-GSL"

enum {
    OS_LOG_ERROR = 1,
    OS_LOG_WARN = 2,
    OS_LOG_INFO = 3,
    OS_LOG_NOTICE = 4,
    OS_LOG_DEBUG = 5,
};

#define GSL_DEBUG_MASK_VERBOSE (1u << 6)

extern uint32_t g_alogDebugMask;
extern uint32_t g_alogAssertLog;
extern uint32_t g_alogLowPerfAssertFlag;
extern uint32_t g_enableNativeAssert;

int os_alog(int level, const char* tag, const char* file, int line, const char* func,
            const char* fmt, ...) __attribute__((format(printf, 6, 7)));

#define GSL_LOG(level, ...) os_alog(level, GSL_LOG_TAG, NULL, __LINE__, __func__, __VA_ARGS__)
#define GSL_ERR(...) GSL_LOG(OS_LOG_ERROR, __VA_ARGS__)
#define GSL_WARN(...) GSL_LOG(OS_LOG_WARN, __VA_ARGS__)
#define GSL_DBG(...)                                                                      \
    do {                                                                                  \
        if (g_alogDebugMask & GSL_DEBUG_MASK_VERBOSE) GSL_LOG(OS_LOG_DEBUG, __VA_ARGS__); \
    } while (0)

struct os_mutex {
    uint32_t reserved;
    uint32_t refs;
    pthread_mutex_t lock;
};

void* os_mutex_create(void);
int os_mutex_free(void* mutex);
int os_mutex_lock(void* mutex);
int os_mutex_unlock(void* mutex);

int os_interlock_incr(int* value);
int os_interlock_decr(int* value);
int os_interlock_xchg(int* value, int exchange);
int os_interlock_compxchg(int* value, int exchange, int comparand);

void* os_event_create(int manual_reset);
int os_event_destroy(void* event);
int os_event_signal(void* event);
int os_event_wait(void* event, uint32_t timeout_ms);

void* os_malloc(size_t size);
void* os_calloc(size_t count, size_t size);
void* os_realloc(void* ptr, size_t size);
void os_free(void* ptr);
