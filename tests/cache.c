/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#include "mock.h"

int gsl_memory_cacheoperation(const struct gsl_memdesc* memdesc, uint64_t offset, uint64_t size,
                              uint32_t op);
int gsl_memory_map_ext_fd_pure(int fd, uint64_t hostptr, uint64_t len, uint64_t offset,
                               uint32_t memtype, struct gsl_memdesc* memdesc, uint32_t flags);

int main(void) {
    test_init();
    struct gsl_memdesc memdesc;
    int fd = memfd_create("gsl-cache", MFD_CLOEXEC);

    assert(fd >= 0 && !ftruncate(fd, 4096));
    mock.fail_dup = true;
    assert(gsl_memory_map_ext_fd_pure(fd, 0, 4096, 0, 3, &memdesc, GSL_MEMFLAGS_WRITEBACK) ==
           GSL_FAILURE_SYSTEMERROR);
    assert(!memdesc.priv && !mock.bind_calls);
    mock.fail_dup = false;
    assert(!gsl_memory_map_ext_fd_pure(fd, 0, 4096, 0, 3, &memdesc, GSL_MEMFLAGS_WRITEBACK));

    mock.sync_interrupts = 2;
    assert(!gsl_memory_cacheoperation(&memdesc, 16, 32, GSL_CACHEFLAGS_CLEAN));
    assert(mock.sync_calls == 1);
    assert(mock.sync_flags[0] == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW));

    assert(!gsl_memory_cacheoperation(&memdesc, 0, 0, GSL_CACHEFLAGS_INVALIDATE));
    assert(mock.sync_calls == 2);
    assert(mock.sync_flags[1] == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW));

    assert(!gsl_memory_cacheoperation(&memdesc, 0, 4096, GSL_CACHEFLAGS_FLUSH));
    assert(mock.sync_calls == 4);
    assert(mock.sync_flags[2] == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW));
    assert(mock.sync_flags[3] == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW));

    mock.sync_errno = EIO;
    assert(gsl_memory_cacheoperation(&memdesc, 0, 4096, GSL_CACHEFLAGS_FLUSH) ==
           GSL_FAILURE_SYSTEMERROR);
    assert(mock.sync_calls == 5);
    assert(gsl_memory_cacheoperation(&memdesc, 4096, 1, GSL_CACHEFLAGS_CLEAN) ==
           GSL_FAILURE_BADPARAM);

    assert(!gsl_memory_free_pure(&memdesc));
    close(fd);
    test_fini();
    puts("cache tests passed");
    return 0;
}
