# Toolchain wrapper for cross-compiling PrusaSlicer (and its dependencies) for Android.
#
# It pins the Android ABI/API level so that the settings propagate to every
# ExternalProject in deps/ (only CMAKE_TOOLCHAIN_FILE is forwarded there), then
# delegates to the NDK's own toolchain file.
#
# Usage:
#   export ANDROID_NDK_HOME=/path/to/android-ndk-r27c
#   cmake -S deps -B deps/build-android -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/android.cmake
#
# Optional environment overrides: PRUSASLICER_ANDROID_ABI (default arm64-v8a),
# PRUSASLICER_ANDROID_API (default 28).

if (DEFINED ENV{ANDROID_NDK_HOME})
    set(_ndk "$ENV{ANDROID_NDK_HOME}")
elseif (DEFINED ENV{ANDROID_NDK_ROOT})
    set(_ndk "$ENV{ANDROID_NDK_ROOT}")
elseif (ANDROID_NDK)
    set(_ndk "${ANDROID_NDK}")
else ()
    message(FATAL_ERROR "Set ANDROID_NDK_HOME to the Android NDK root directory.")
endif ()

if (DEFINED ENV{PRUSASLICER_ANDROID_ABI})
    set(ANDROID_ABI "$ENV{PRUSASLICER_ANDROID_ABI}" CACHE STRING "" FORCE)
else ()
    set(ANDROID_ABI "arm64-v8a" CACHE STRING "" FORCE)
endif ()
if (DEFINED ENV{PRUSASLICER_ANDROID_API})
    set(_api "$ENV{PRUSASLICER_ANDROID_API}")
else ()
    set(_api 28)
endif ()
set(ANDROID_PLATFORM "android-${_api}" CACHE STRING "" FORCE)
# Everything is linked into a single JNI library, so use the static C++ runtime.
set(ANDROID_STL "c++_static" CACHE STRING "" FORCE)
set(ANDROID_CPP_FEATURES "rtti exceptions" CACHE STRING "" FORCE)

include("${_ndk}/build/cmake/android.toolchain.cmake")

# Used by autotools-based dependencies (GMP, MPFR, OpenSSL, Lua) as --host value.
if (ANDROID_ABI STREQUAL "arm64-v8a")
    set(TOOLCHAIN_PREFIX "aarch64-linux-android")
elseif (ANDROID_ABI STREQUAL "x86_64")
    set(TOOLCHAIN_PREFIX "x86_64-linux-android")
else ()
    message(FATAL_ERROR "Unsupported ANDROID_ABI: ${ANDROID_ABI}")
endif ()

# Target-specific compiler drivers (clang with the --target baked in) for build
# systems that do not understand CMake toolchain files.
set(ANDROID_TARGET_CC  "${ANDROID_TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}${_api}-clang")
set(ANDROID_TARGET_CXX "${ANDROID_TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}${_api}-clang++")
set(ANDROID_TARGET_API "${_api}")

# Dependencies are installed into a custom prefix. Add it to the find root path so that
# headers/libraries are found there and in the NDK sysroot, but never on the build host.
if (CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
endif ()
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
