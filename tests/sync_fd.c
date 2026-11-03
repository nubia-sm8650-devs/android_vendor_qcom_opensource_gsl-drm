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
    test_fini();
    puts("sync fd tests passed");
    return 0;
}
