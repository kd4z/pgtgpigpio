# cmake/aarch64-rpi4-toolchain.cmake
#
# Cross-compilation toolchain for:
#   Host:   x86_64 Debian Trixie running in WSL2
#   Target: aarch64 Raspberry Pi 4, Debian Trixie
#
# Prerequisites (run once in WSL2):
#   sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu cmake pkg-config ninja-build
#   sudo dpkg --add-architecture arm64
#   sudo apt update
#   sudo apt install libgpiod-dev:arm64 libgpiod2:arm64
#
# Usage:
#   mkdir build-aarch64 && cd build-aarch64
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-rpi4-toolchain.cmake \
#            -DCMAKE_BUILD_TYPE=Release -G Ninja
#   ninja
#
# Verify the output binary:
#   file ./blink                                      # → ELF 64-bit, ARM aarch64
#   aarch64-linux-gnu-readelf -d ./blink | grep NEEDED  # → libgpiod.so.3, libc.so.6

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross-compiler binaries (installed by gcc-aarch64-linux-gnu package)
set(CMAKE_C_COMPILER   /usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)

# Restrict CMake's search paths so it only finds target-architecture files,
# never host (x86_64) programs, libraries, or headers.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Redirect pkg-config to the arm64 .pc files.
# Without this, pkg-config finds the host x86_64 libgpiod and passes wrong
# library paths to the aarch64 linker.
set(ENV{PKG_CONFIG_DIR}         "")
set(ENV{PKG_CONFIG_LIBDIR}      "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "/usr/aarch64-linux-gnu")
