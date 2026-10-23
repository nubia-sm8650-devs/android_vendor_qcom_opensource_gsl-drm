/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dlfcn.h>
#include <errno.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include "gsl_os.h"

uint32_t g_alogDebugMask;
uint32_t g_alogAssertLog;
uint32_t g_alogLowPerfAssertFlag;
uint32_t g_enableNativeAssert;

static const int os_log_prio[] = {
        [OS_LOG_ERROR] = ANDROID_LOG_ERROR,   [OS_LOG_WARN] = ANDROID_LOG_WARN,
        [OS_LOG_INFO] = ANDROID_LOG_DEBUG,    [OS_LOG_NOTICE] = ANDROID_LOG_INFO,
        [OS_LOG_DEBUG] = ANDROID_LOG_VERBOSE,
};

int os_alog(int level, const char* tag, const char* file, int line, const char* func,
            const char* fmt, ...) {
    char msg[512];
    va_list ap;

    if (level < OS_LOG_ERROR || level > OS_LOG_DEBUG) return 0;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (file)
        return __android_log_print(os_log_prio[level], tag, "%s:%d <%s>: %s", file, line, func,
                                   msg);

    return __android_log_print(os_log_prio[level], tag, "<%s:%d>: %s", func, line, msg);
}

void os_logsystem_unbuffered(const char* msg, size_t len) {
    if (strnlen(msg, len) < len) __android_log_print(ANDROID_LOG_INFO, "Adreno", "%s", msg);
}

int os_exit(int code, int line, const char* func) {
    if (func)
        GSL_ERR("Exiting the process %s from function %s and line %d", getprogname(), func, line);

    return raise(SIGKILL);
}

int gfx_os_sleep(uint32_t ms) {
    return usleep(ms * 1000);
}

void* os_malloc(size_t size) {
    return size ? malloc(size) : NULL;
}

void* os_calloc(size_t count, size_t size) {
    return count && size ? calloc(count, size) : NULL;
}

void* os_realloc(void* ptr, size_t size) {
    if (!size) {
        free(ptr);
        return NULL;
    }

    return realloc(ptr, size);
}

void os_free(void* ptr) {
    free(ptr);
}

int os_atoi(const char* str) {
    return atoi(str);
}

int os_memcmp(const void* a, const void* b, size_t len) {
    return memcmp(a, b, len);
}

void* os_memcpy(void* dst, const void* src, size_t len) {
    return memcpy(dst, src, len);
}

void* os_memset(void* dst, int c, size_t len) {
    return memset(dst, c, len);
}

int os_strcmp(const char* a, const char* b) {
    return strcmp(a, b);
}

int os_strncmp(const char* a, const char* b, size_t len) {
    return strncmp(a, b, len);
}

size_t os_strlen(const char* str) {
    return strlen(str);
}

size_t os_strlcpy(char* dst, const char* src, size_t size) {
    return strlcpy(dst, src, size);
}

size_t os_strlcat(char* dst, const char* src, size_t size) {
    return strlcat(dst, src, size);
}

char* os_strstr(const char* haystack, const char* needle) {
    return strstr(haystack, needle);
}

int os_vsnprintf(char* buf, uint32_t size, const char* fmt, va_list ap) {
    return vsnprintf(buf, size, fmt, ap);
}

int os_snprintf(char* buf, uint32_t size, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return ret;
}

void* os_mutex_create(void) {
    struct os_mutex* mutex = malloc(sizeof(*mutex));
    pthread_mutexattr_t attr;

    if (!mutex) return NULL;

    mutex->refs = 0;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(&mutex->lock, &attr)) {
        pthread_mutexattr_destroy(&attr);
        free(mutex);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);

    return mutex;
}

int os_mutex_free(void* handle) {
    struct os_mutex* mutex = handle;

    if (!mutex) return -1;
    if (mutex->refs) return 0;

    pthread_mutex_destroy(&mutex->lock);
    free(mutex);

    return 0;
}

int os_mutex_lock(void* handle) {
    struct os_mutex* mutex = handle;

    return mutex ? pthread_mutex_lock(&mutex->lock) : -1;
}

int os_mutex_unlock(void* handle) {
    struct os_mutex* mutex = handle;

    if (!mutex) return -1;

    pthread_mutex_unlock(&mutex->lock);

    return 0;
}

int os_interlock_incr(int* value) {
    return __atomic_add_fetch(value, 1, __ATOMIC_ACQ_REL);
}

int os_interlock_decr(int* value) {
    return __atomic_sub_fetch(value, 1, __ATOMIC_ACQ_REL);
}

int os_interlock_xchg(int* value, int exchange) {
    return __atomic_exchange_n(value, exchange, __ATOMIC_ACQ_REL);
}

int os_interlock_compxchg(int* value, int exchange, int comparand) {
    __atomic_compare_exchange_n(value, &comparand, exchange, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);

    return comparand;
}

struct os_event {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int count;
    int manual_reset;
};

void* os_event_create(int manual_reset) {
    struct os_event* event = malloc(sizeof(*event));

    if (!event) return NULL;

    pthread_mutex_init(&event->lock, NULL);
    pthread_cond_init(&event->cond, NULL);
    event->count = 0;
    event->manual_reset = manual_reset;

    return event;
}

int os_event_destroy(void* handle) {
    struct os_event* event = handle;

    if (!event) return -1;

    pthread_cond_destroy(&event->cond);
    pthread_mutex_destroy(&event->lock);
    free(event);

    return 0;
}

int os_event_signal(void* handle) {
    struct os_event* event = handle;

    if (!event) return -1;

    pthread_mutex_lock(&event->lock);
    event->count++;
    pthread_cond_broadcast(&event->cond);
    pthread_mutex_unlock(&event->lock);

    return 0;
}

int os_event_wait(void* handle, uint32_t timeout_ms) {
    struct os_event* event = handle;
    struct timespec deadline;
    int ret = 0;

    if (!event) return -1;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&event->lock);
    while (!event->count) {
        int err = timeout_ms == UINT32_MAX
                          ? pthread_cond_wait(&event->cond, &event->lock)
                          : pthread_cond_timedwait(&event->cond, &event->lock, &deadline);

        if (err == ETIMEDOUT) {
            ret = -10;
            break;
        }
        if (err) {
            ret = -5;
            break;
        }
    }
    if (!ret && !event->manual_reset) event->count--;
    pthread_mutex_unlock(&event->lock);

    return ret;
}

struct os_thread_start {
    void (*fn)(void*);
    void* arg;
    pid_t tid;
    sem_t started;
};

static void* os_thread_launcher(void* data) {
    struct os_thread_start* start = data;
    void (*fn)(void*) = start->fn;
    void* arg = start->arg;

    start->tid = gettid();
    sem_post(&start->started);
    fn(arg);

    return NULL;
}

void* os_thread_create(void (*fn)(void*), void* arg, int* tid) {
    struct os_thread_start start = {.fn = fn, .arg = arg};
    pthread_t* thread;

    if (!fn) return NULL;

    thread = malloc(sizeof(*thread));
    if (!thread) return NULL;

    if (sem_init(&start.started, 0, 0)) goto err_free;

    if (pthread_create(thread, NULL, os_thread_launcher, &start)) {
        GSL_ERR("os_thread_create pthread_create failed %d", errno);
        goto err_sem;
    }
    pthread_setname_np(*thread, "AdrenoOsLib");

    while (sem_wait(&start.started) && errno == EINTR);
    sem_destroy(&start.started);

    if (tid) *tid = start.tid;

    return thread;

err_sem:
    sem_destroy(&start.started);
err_free:
    free(thread);
    return NULL;
}

void os_thread_destroy(void* handle) {
    pthread_t* thread = handle;

    if (!thread) return;

    pthread_detach(*thread);
    free(thread);
}

void os_thread_join(void* handle) {
    pthread_t* thread = handle;

    if (!thread) return;

    pthread_join(*thread, NULL);
    free(thread);
}

pthread_t os_thread_getid(void) {
    return pthread_self();
}

int os_thread_is_self(void* handle) {
    pthread_t* thread = handle;

    return thread && pthread_equal(pthread_self(), *thread);
}

int os_tls_alloc(void) {
    pthread_key_t key;

    if (pthread_key_create(&key, NULL)) return -1;

    return (int)key + 1;
}

int os_tls_free(int key) {
    return pthread_key_delete(key - 1) ? -1 : 0;
}

void* os_tls_read(int key) {
    return pthread_getspecific(key - 1);
}

int os_tls_write(int key, void* value) {
    return pthread_setspecific(key - 1, value) ? -1 : 0;
}

void* os_lib_map(const char* path) {
    void* handle = dlopen(path, RTLD_NOW);

    if (!handle) GSL_ERR("  os_lib_map error: %s, on '%s'", dlerror(), path);

    return handle;
}

int os_lib_unmap(void* handle) {
    dlclose(handle);

    return 0;
}

void* os_lib_getaddr(void* handle, const char* name) {
    return dlsym(handle, name);
}

pid_t os_process_getid(void) {
    return getpid();
}

struct os_sysinfo {
    uint64_t reserved[2];
    uint32_t reserved2;
    uint64_t total_ram;
    uint64_t free_ram;
    uint32_t page_size;
    uint32_t page_size_fixed;
    uint64_t reserved3;
};

int os_get_sysinfo(struct os_sysinfo* out) {
    struct sysinfo info;

    if (!out) return -1;

    memset(out, 0, sizeof(*out));
    if (sysinfo(&info)) return -1;

    out->total_ram = (uint64_t)info.totalram * info.mem_unit;
    out->free_ram = (uint64_t)info.freeram * info.mem_unit;
    out->page_size = (uint32_t)sysconf(_SC_PAGESIZE);
    out->page_size_fixed = 0x1000;

    return 0;
}
