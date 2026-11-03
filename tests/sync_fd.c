/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "mock.h"

int main(void) {
    test_init();
    struct gsl_syncobj obj = {.refs = 1, .fd = open("/dev/null", O_RDONLY)};
    int fd = -1;

    assert(obj.fd >= 0);
    mock.fail_dup = true;
    assert(gsl_syncobj_export_fd(&test_device, &obj, &fd) == GSL_FAILURE_SYSTEMERROR);
    assert(fd == -1 && mock.signaled_creates == 0);

    mock.fail_dup = false;
    assert(!gsl_syncobj_export_fd(&test_device, &obj, &fd));
    assert(fd >= 0 && fd != obj.fd);
    close(fd);
    close(obj.fd);

    obj.fd = -1;
    assert(!gsl_syncobj_export_fd(&test_device, &obj, &fd));
    assert(fd >= 0 && mock.signaled_creates == 1);
    close(fd);

    struct gsl_timeline_obj tl = {.refs = 1, .handle = 7};
    struct gsl_sync_point point = {.tl = &tl, .point = 3};
    obj = (struct gsl_syncobj){.refs = 1, .fd = -1, .count = 1, .points = &point};

    assert(!gsl_syncobj_export_fd(&test_device, &obj, &fd));
    assert(fd >= 0 && mock.transfers == 1 && mock.available_waits == 0);
    assert(!(mock.transfer_flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT));
    close(fd);

    mock.transfers = 0;
    mock.point_pending = true;
    mock.pending_wait_timeouts = 2;
    assert(!gsl_syncobj_export_fd(&test_device, &obj, &fd));
    assert(fd >= 0 && mock.transfers == 2 && mock.available_waits == 3);
    assert(mock.wait_flags == DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
    assert(!(mock.transfer_flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT));
    assert(mock.signaled_creates == 1);
    close(fd);
    test_fini();
    puts("sync fd tests passed");
    return 0;
}
