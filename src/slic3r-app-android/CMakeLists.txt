# Android entry points to PrusaSlicer:
#  - libprusaslicer.so (prusaslicer-jni): JNI library used by the Android app in android/.
#  - prusa-slicer (prusaslicer-android-cli): stand-alone command line executable.
# Both run the PrusaSlicer command line interface (the same code path as
# `prusa-slicer --export-gcode ...` on desktop).

find_package(CLI11 CONFIG REQUIRED)

add_library(slic3r-app-android-core STATIC
    src/AndroidCli.cpp
    src/AndroidCli.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../slic3r-app-launcher/src/Slic3r/App/Launcher/ReadCLI.cpp
)
target_include_directories(slic3r-app-android-core PUBLIC
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../slic3r-app-launcher/src
)
target_link_libraries(slic3r-app-android-core PUBLIC slic3r-app-cli CLI11::CLI11 log)

set(_android_link_options
    -Wl,--gc-sections
    -Wl,-z,max-page-size=16384  # Android 15+ 16 KB page size support
)

add_library(prusaslicer-jni SHARED src/PrusaSlicerJni.cpp)
target_link_libraries(prusaslicer-jni PRIVATE slic3r-app-android-core)
# Only the JNI entry points need to be exported.
target_link_options(prusaslicer-jni PRIVATE ${_android_link_options} -Wl,--exclude-libs,ALL)
set_target_properties(prusaslicer-jni PROPERTIES OUTPUT_NAME prusaslicer)

add_executable(prusaslicer-android-cli src/CliMain.cpp)
target_link_libraries(prusaslicer-android-cli PRIVATE slic3r-app-android-core)
target_link_options(prusaslicer-android-cli PRIVATE ${_android_link_options})
set_target_properties(prusaslicer-android-cli PROPERTIES OUTPUT_NAME prusa-slicer)

install(TARGETS prusaslicer-jni LIBRARY DESTINATION lib)
install(TARGETS prusaslicer-android-cli RUNTIME DESTINATION bin)

# Session reuse test (not part of the default build).
add_executable(prusaslicer-android-session-test EXCLUDE_FROM_ALL test/SessionTest.cpp)
target_link_libraries(prusaslicer-android-session-test PRIVATE slic3r-app-android-core)
target_link_options(prusaslicer-android-session-test PRIVATE ${_android_link_options})
set_target_properties(prusaslicer-android-session-test PROPERTIES OUTPUT_NAME session-test)
