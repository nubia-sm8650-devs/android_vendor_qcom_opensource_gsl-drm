# DRM backend regression tests

Run `tests/run.sh` from this checkout. It cross-compiles the production
context, device, memory, sync and VMA sources for AArch64 and executes each
test with qemu-aarch64. DRM ioctls and fd duplication are mocked. The tests
do not validate GPU execution, exporter cache maintenance or frame timing
on hardware.

The defaults require `aarch64-linux-gnu-gcc`, `qemu-aarch64`, the AArch64
glibc sysroot at `/usr/aarch64-linux-gnu`, and DRM headers from the sibling
Android tree's `bionic/libc/kernel/uapi/drm`. Build products go into a new
directory under `/tmp/opencode`.

Set `CC`, `TEST_CFLAGS`, `DRM_UAPI` or `AARCH64_SYSROOT` to override these
defaults. `DRM_UAPI` names the directory containing `msm_drm.h` and
`drm.h`, not its parent. The headers must include MSM VM_BIND support.

Clang can also run the tests with undefined-behavior traps enabled.

```sh
CC=/path/to/clang \
TEST_CFLAGS='--target=aarch64-linux-gnu --gcc-toolchain=/usr --sysroot=/usr/aarch64-linux-gnu -Wno-unknown-warning-option -fsanitize=undefined -fsanitize-trap=undefined' \
tests/run.sh
```
