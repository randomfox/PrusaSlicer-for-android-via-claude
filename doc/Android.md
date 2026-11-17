# PrusaSlicer for Android

This is an experimental Android port of PrusaSlicer. It is being done in stages:

1. **Slicing engine on Android (current).** The full PrusaSlicer slicing core, preset
   system and command line interface are cross-compiled for Android (arm64-v8a). A small
   native Android app lets you open an STL/3MF/OBJ model, pick a printer, print and
   filament profile from the bundled Prusa presets, slice it, and save or share the
   resulting G-code (`.gcode` or binary `.bgcode`, depending on the printer profile).
2. **The PrusaSlicer GUI on Android (in progress).** The full PrusaSlicer 3 interface
   (plater, object list, settings sidebar, slicing, G-code preview, export) runs on OpenGL
   ES 3.2 with touch input. Landscape only for now. The stage 1 screen is still available
   as "Quick slice" from the menu.
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

### Stage 2: the GUI

* `SLIC3R_MOBILE` (implied by Android) builds the GUI without wxWidgets. The render library
  uses the desktop (core profile) code path; the desktop shaders are translated to GLSL ES
  3.20 when loaded (`Shader_gl.cpp`), render targets use ES compatible formats (`GLTypes.cpp`).
* `src/slic3r-app-android/src/gui`: `MobileApp` (start-up, the equivalent of the desktop
  `DesktopApp`), `MobileRenderCanvas` (renders into the host's GL context, turns touch into
  mouse input: tap = click, drag = rotate / drag objects, long press = context menu, two
  finger drag = pan, pinch = zoom) and `MobileServices` (dialogs, file pickers, fonts through
  `IMobileHost`).
* Android host: `GuiActivity` + `PrusaSlicerSurfaceView` (GLSurfaceView, soft keyboard,
  Storage Access Framework pickers). Files PrusaSlicer exports are written to an output
  folder; the app offers to save or share each new one.

Testing without a device: configure the main project on Linux with `-DSLIC3R_MOBILE=ON`
(dependencies built with `cmake -S deps -DSLIC3R_MOBILE=ON ...`), build `mobile-headless`
and run it with a script (see `src/slic3r-app-android/test/MobileHeadless.cpp`). It renders
the GUI into an EGL pbuffer with Mesa (OpenGL ES 3.2, llvmpipe), injects touch input and
writes screenshots.

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
* GUI: landscape only; native dialogs that are not supported yet return their default
  answer (e.g. saving presets under a new name, comparing presets); web based features
  (Printables, Prusa Connect login) are not available.
* STEP import, post-processing scripts, printer upload and online preset updates are not
  available.
