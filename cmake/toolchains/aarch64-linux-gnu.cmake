# Cross-compile Pixelica (headless) for linux/aarch64 on an x86-64 Linux host.
#
#   sudo apt-get install gcc-aarch64-linux-gnu qemu-user
#   cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
#         -DPIXELICA_PLATFORM=HEADLESS -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-arm64 && ctest --test-dir build-arm64 --output-on-failure
#
# CTest runs the arm64 test executables through qemu-aarch64 (user-mode
# emulation). That checks the portable code on arm64; it is not a Raspberry Pi
# run and says nothing about Pi hardware or speed. Use the same toolchain for an
# SDL3 build only with an aarch64 SDL3 package on CMAKE_FIND_ROOT_PATH.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)

# Run target executables (tests) under qemu-user with the cross sysroot.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64 -L /usr/aarch64-linux-gnu)

# Programs (python, cmake helpers) come from the host; libraries/headers from the target.
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
