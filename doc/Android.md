# PrusaSlicer for Android

This is an experimental Android port of PrusaSlicer. It is being done in stages:

1. **Slicing engine on Android (current).** The full PrusaSlicer slicing core, preset
   system and command line interface are cross-compiled for Android (arm64-v8a). A small
   native Android app lets you open an STL/3MF/OBJ model, pick a printer, print and
   filament profile from the bundled Prusa presets, slice it, and save or share the
   resulting G-code (`.gcode` or binary `.bgcode`, depending on the printer profile).
2. **The PrusaSlicer GUI on Android.** Run the existing OpenGL/ImGui based plater and
   G-code preview on top of OpenGL ES with touch input (not started).
3. **Remaining features.** STEP import (OpenCASCADE), printer connectivity, storage
   integration (not started).

## How it is put together

| Piece | Location | Notes |
| --- | --- | --- |
| Toolchain | `cmake/toolchains/android.cmake` | Wraps the NDK toolchain, pins ABI/API level (default arm64-v8a, API 28) and the static C++ runtime. Used for both the dependencies and PrusaSlicer. |
| Dependencies | `deps/` | Built like on desktop. On Android, desktop-only packages (wxWidgets, GLEW, GLFW, SDL, hidapi, OCCT, sentry, ...) are skipped, and the autotools-based ones (GMP, MPFR, OpenSSL, Lua) are cross-compiled. |
| PrusaSlicer | top-level `CMakeLists.txt` | `ANDROID` behaves like the Emscripten build: no wxWidgets/WebKit/STEP, rendering libraries are compiled against OpenGL ES 3 (`src/platform/android/include/GL/glew.h` is a GLEW stand-in). |
| Native entry points | `src/slic3r-app-android/` | `libprusaslicer.so` (JNI) and a stand-alone `prusa-slicer` executable. Both run the regular PrusaSlicer CLI (`src/slic3r-app-cli`). The JNI library keeps one CLI runtime per process, so the preset bundle is loaded once and reused by all later queries and slices (`ninja prusaslicer-android-session-test` builds a test for this). |
| Android app | `android/` | Kotlin, Material 3. Bundles `resources/` (presets, matdb, ...) as assets and extracts them on first start. |

The resources directory is located through the `PRUSASLICER_RESOURCES_DIR` environment
variable on Android (see `src/slic3r-shared/src/Slic3r/App/Init.cpp`); the data and cache
directories are derived from `XDG_CONFIG_HOME`/`XDG_CACHE_HOME`, which the app points into
its private storage.

## Building

Requirements: Linux host, Android NDK r27c, Android SDK (platform 35, build-tools 35),
JDK 17+, CMake 3.22+, Ninja, autoconf/automake/libtool, texinfo, perl, git.

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r27c

# 1. Dependencies + libprusaslicer.so (takes a few hours the first time).
#    Installs the library into android/app/src/main/jniLibs/arm64-v8a/.
tools/android/build-native.sh build-android

# 2. The APK.
cd android
echo "sdk.dir=/path/to/android-sdk" > local.properties
./gradlew assembleRelease     # -> app/build/outputs/apk/release/app-release.apk
```

The release APK is signed with the debug key so that it can be installed directly; set
up a proper signing config before distributing it.

### Restricted networks

If GitHub's source archive endpoint (`github.com/<owner>/<repo>/archive/...`) is not
reachable but `git` is, pre-fetch those dependencies with git and point the deps build at
them:

```bash
tools/android/mirror-deps.sh /path/to/sources
DEP_SOURCES_DIR=/path/to/sources tools/android/build-native.sh build-android
```

The mirrored archives come from the same pinned tags/commits, but they are not
byte-identical to GitHub's archives, so the `URL_HASH` check is skipped for them.

## Testing without a device

The arm64 binaries can be run on an x86_64 Linux host with `qemu-aarch64-static`, using the
bionic libraries (`/system/bin/linker64`, `/system/lib64`) extracted from an Android
emulator system image as the `-L` prefix. This is how the stage 1 build was verified: the
CLI queries the bundled printer/print/filament profiles and slices an STL into valid G-code.

## Command line usage on a device

The `prusa-slicer` executable (`build-android/app/src/slic3r-app-android/prusa-slicer`)
can be used from `adb shell` or Termux:

```bash
export PRUSASLICER_RESOURCES_DIR=/path/to/resources
prusa-slicer --query-printer-models --output printers.json
prusa-slicer --export-gcode --printer-profile "Original Prusa MK4S HF0.4 nozzle" \
    --print-profile "0.20mm SPEED @PRUSA MK4S HF0.4" --material-profile "Prusament PLA @PG" \
    --output out/ model.stl
```

## Limitations of the current stage

* Needs about 1 GB of free memory: loading the full bundled preset library alone takes
  roughly that much before slicing starts.
* The rendering libraries target OpenGL ES 3.2 (texture buffers); not used yet in stage 1.
* arm64-v8a only (x86_64 can be built with `PRUSASLICER_ANDROID_ABI=x86_64`, untested).
* No 3D view or model editing; models are sliced as loaded (auto-arranged and centered).
* STEP import, post-processing scripts, printer upload and online preset updates are not
  available.
