cmake_minimum_required(VERSION 3.18)
project(PgTgPiGpio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Locate libgpiod via pkg-config.
# When cross-compiling, PKG_CONFIG_LIBDIR is redirected by the toolchain file
# to the arm64 .pc files so we find the correct target-architecture library.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED libgpiod>=2.0)

add_executable(PgTgPiGpio PgTgPiGpio.c)

target_compile_options(PgTgPiGpio PRIVATE
    ${GPIOD_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)
target_include_directories(PgTgPiGpio PRIVATE ${GPIOD_INCLUDE_DIRS})
target_link_libraries(PgTgPiGpio PRIVATE ${GPIOD_LIBRARIES})
target_link_directories(PgTgPiGpio PRIVATE ${GPIOD_LIBRARY_DIRS})

install(TARGETS PgTgPiGpio DESTINATION bin)

# Install systemd service unit (default prefix: /usr/local/lib/systemd/system/)
install(FILES PgTgPiGpio.service DESTINATION lib/systemd/system)

# Install example config (default prefix: /usr/local/etc/PgTgPiGpio/)
install(FILES PgTgPiGpio.conf DESTINATION etc/PgTgPiGpio)
