# CMake toolchain file for the island's static libraries (tools/build-native.sh): aarch64-linux-musl, static only.
# LX_MUSL_TOOLCHAIN (environment) is the musl cross toolchain (patches/musl.md).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(LX_MUSL_TOOLCHAIN "$ENV{LX_MUSL_TOOLCHAIN}" CACHE PATH "The aarch64-linux-musl cross toolchain")
if(NOT EXISTS "${LX_MUSL_TOOLCHAIN}/bin/aarch64-linux-musl-gcc")
  message(FATAL_ERROR "LX_MUSL_TOOLCHAIN must point at the aarch64-linux-musl toolchain")
endif()

set(CMAKE_C_COMPILER "${LX_MUSL_TOOLCHAIN}/bin/aarch64-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "${LX_MUSL_TOOLCHAIN}/bin/aarch64-linux-musl-g++")
set(CMAKE_ASM_COMPILER "${LX_MUSL_TOOLCHAIN}/bin/aarch64-linux-musl-gcc")
set(CMAKE_AR "${LX_MUSL_TOOLCHAIN}/bin/aarch64-linux-musl-ar")
set(CMAKE_RANLIB "${LX_MUSL_TOOLCHAIN}/bin/aarch64-linux-musl-ranlib")
set(CMAKE_SYSROOT "${LX_MUSL_TOOLCHAIN}/aarch64-linux-musl")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Nothing from the build machine: no host libraries through pkg-config.
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(THREADS_PREFER_PTHREAD_FLAG ON)

# The console's code pages are BTI-guarded (landing pads everywhere). OUNSU_ISLAND selects the port's changes in the
# patched sources (realm-core).
set(LX_FLAGS "-mbranch-protection=bti -fno-pie -fno-pic -ffunction-sections -fdata-sections -DOUNSU_ISLAND=1")
set(CMAKE_C_FLAGS_INIT "${LX_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${LX_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -no-pie")
