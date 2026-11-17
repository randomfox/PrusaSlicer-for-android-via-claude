# JNI library used by the Android app (android/). It exposes the PrusaSlicer command line
# interface (the same code path as `prusa-slicer --export-gcode ...`) to Java/Kotlin.

find_package(CLI11 CONFIG REQUIRED)

add_library(prusaslicer-jni SHARED
    src/PrusaSlicerJni.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../slic3r-app-launcher/src/Slic3r/App/Launcher/ReadCLI.cpp
)

target_include_directories(prusaslicer-jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../slic3r-app-launcher/src
)

target_link_libraries(prusaslicer-jni PRIVATE
    slic3r-app-cli
    CLI11::CLI11
    log
    android
)

# Only the JNI entry points need to be exported.
target_link_options(prusaslicer-jni PRIVATE
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
    -Wl,-z,max-page-size=16384  # Android 15+ 16 KB page size support
)
set_target_properties(prusaslicer-jni PROPERTIES OUTPUT_NAME prusaslicer)

install(TARGETS prusaslicer-jni LIBRARY DESTINATION lib)
