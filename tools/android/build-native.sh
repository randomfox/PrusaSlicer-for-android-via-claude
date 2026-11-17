#!/usr/bin/env bash
# Cross-compiles the PrusaSlicer dependencies and the JNI library (libprusaslicer.so) for
# Android and places the library where the Gradle project in android/ picks it up.
#
# Requirements: Android NDK r27+ (ANDROID_NDK_HOME), CMake >= 3.22, Ninja, autotools,
# texinfo, perl, git.
#
# Usage: tools/android/build-native.sh [build_dir]
#   build_dir defaults to <repo>/build-android
#
# Environment:
#   ANDROID_NDK_HOME          NDK location (required)
#   PRUSASLICER_ANDROID_ABI   arm64-v8a (default) or x86_64
#   PRUSASLICER_ANDROID_API   minimum API level (default 28)
#   DEP_SOURCES_DIR           optional directory with pre-fetched source archives,
#                             see tools/android/mirror-deps.sh
#   JOBS                      parallel jobs for the PrusaSlicer build (default: nproc)
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${1:-${repo_root}/build-android}"
abi="${PRUSASLICER_ANDROID_ABI:-arm64-v8a}"
jobs="${JOBS:-$(nproc)}"
toolchain="${repo_root}/cmake/toolchains/android.cmake"

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to the Android NDK directory}"
export LC_ALL=C.UTF-8 LANG=C.UTF-8  # needed to extract the Boost archive

deps_build="${build_dir}/deps"
deps_prefix="${deps_build}/destdir/usr/local"
app_build="${build_dir}/app"

echo "==> Building dependencies (${abi}) in ${deps_build}"
deps_args=()
if [[ -n "${DEP_SOURCES_DIR:-}" ]]; then
    deps_args+=("-DDEP_SOURCES_DIR=${DEP_SOURCES_DIR}")
fi
cmake -S "${repo_root}/deps" -B "${deps_build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    "${deps_args[@]}"
# Each dependency builds in parallel internally; keep the top level serial.
cmake --build "${deps_build}" -- -j1

echo "==> Building libprusaslicer.so in ${app_build}"
cmake -S "${repo_root}" -B "${app_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    -DCMAKE_PREFIX_PATH="${deps_prefix}" \
    -DSLIC3R_PCH=OFF \
    -DSLIC3R_RELEASE_DEBUG_SYMBOLS=OFF
cmake --build "${app_build}" --target prusaslicer-jni -- -j"${jobs}"

jni_dir="${repo_root}/android/app/src/main/jniLibs/${abi}"
mkdir -p "${jni_dir}"
"${ANDROID_NDK_HOME}"/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-unneeded \
    -o "${jni_dir}/libprusaslicer.so" "${app_build}/src/slic3r-app-android/libprusaslicer.so"
echo "==> Installed ${jni_dir}/libprusaslicer.so ($(du -h "${jni_dir}/libprusaslicer.so" | cut -f1))"

# Configuring PrusaSlicer copied the material database to resources/matdb, from where the
# Gradle build bundles it together with the other resources.
