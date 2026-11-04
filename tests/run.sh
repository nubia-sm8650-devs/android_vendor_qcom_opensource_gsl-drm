#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
android_root=$(CDPATH= cd -- "$repo/../../../.." && pwd)
build_dir=$(mktemp -d /tmp/opencode/gsl-drm-tests.XXXXXX)
cc=${CC:-aarch64-linux-gnu-gcc}
uapi=${DRM_UAPI:-$android_root/bionic/libc/kernel/uapi/drm}

mkdir "$build_dir/include"
ln -s "$uapi" "$build_dir/include/drm"

for test in "$repo"/tests/sync_fd.c "$repo"/tests/cache.c \
            "$repo"/tests/deferred.c "$repo"/tests/sparse.c \
            "$repo"/tests/wait.c "$repo"/tests/lock.c \
            "$repo"/tests/submit.c "$repo"/tests/timestamp.c \
            "$repo"/tests/dependencies.c "$repo"/tests/merge.c; do
    name=$(basename "$test" .c)
    "$cc" ${TEST_CFLAGS:-} -D_GNU_SOURCE -std=gnu11 -O2 -g -Wall -Wextra -Werror \
        -Wno-unused-parameter -Wno-format-truncation \
        -I"$build_dir/include" -I"$repo/include" \
        -I"$repo/src" -pthread "$test" "$repo/tests/mock.c" "$repo/tests/allocation.c" \
        "$repo/src/context.c" "$repo/src/device.c" "$repo/src/memory.c" \
        "$repo/src/sync.c" "$repo/src/sync_core.c" "$repo/src/vma.c" \
        -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc -o "$build_dir/$name"
    qemu-aarch64 -L "${AARCH64_SYSROOT:-/usr/aarch64-linux-gnu}" "$build_dir/$name"
done
