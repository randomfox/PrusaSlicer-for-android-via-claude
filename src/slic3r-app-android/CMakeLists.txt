# Mobile (Android) entry points to PrusaSlicer.
#
# Android:
#  - libprusaslicer.so (prusaslicer-jni): JNI library used by the Android app in android/.
#    Contains the command line bridge (stage 1) and the full GUI (stage 2).
#  - prusa-slicer (prusaslicer-android-cli): stand-alone command line executable.
# Linux with -DSLIC3R_MOBILE=ON (testing):
#  - mobile-headless: runs the mobile GUI headless (EGL pbuffer) driven by a script.

find_package(CLI11 CONFIG REQUIRED)

# Command line bridge: runs the PrusaSlicer CLI (the code path of `prusa-slicer --export-gcode`).
add_library(slic3r-app-android-core STATIC
    src/AndroidCli.cpp
    src/AndroidCli.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../slic3r-app-launcher/src/Slic3r/App/Launcher/ReadCLI.cpp
)
target_include_directories(slic3r-app-android-core PUBLIC
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../slic3r-app-launcher/src
)
target_link_libraries(slic3r-app-android-core PUBLIC slic3r-app-cli CLI11::CLI11)
if (ANDROID)
    target_link_libraries(slic3r-app-android-core PUBLIC log)
endif ()

# The GUI hosted by a mobile platform (plater + preview on an OpenGL ES surface).
add_library(slic3r-mobile-gui STATIC
    src/gui/MobileApp.cpp
    src/gui/MobileApp.hpp
    src/gui/MobileForm.cpp
    src/gui/MobilePresetDialogs.cpp
    src/gui/MobilePresetDialogs.hpp
    src/gui/MobileHost.hpp
    src/gui/MobileRenderCanvas.cpp
    src/gui/MobileRenderCanvas.hpp
    src/gui/MobileServices.cpp
    src/gui/MobileServices.hpp
    src/gui/MobileWebView.cpp
    src/gui/MobileWebView.hpp
)
target_include_directories(slic3r-mobile-gui PUBLIC src)
target_link_libraries(slic3r-mobile-gui PUBLIC slic3r-shared slic3r-app-android-core)

if (ANDROID)
    set(_android_link_options
        -Wl,--gc-sections
        -Wl,-z,max-page-size=16384  # Android 15+ 16 KB page size support
    )

    add_library(prusaslicer-jni SHARED
        src/PrusaSlicerJni.cpp
        src/PrusaSlicerGuiJni.cpp
    )
    target_link_libraries(prusaslicer-jni PRIVATE slic3r-mobile-gui slic3r-app-android-core)
    # Only the JNI entry points need to be exported.
    target_link_options(prusaslicer-jni PRIVATE ${_android_link_options} -Wl,--exclude-libs,ALL)
    set_target_properties(prusaslicer-jni PROPERTIES OUTPUT_NAME prusaslicer)

    add_executable(prusaslicer-android-cli src/CliMain.cpp)
    target_link_libraries(prusaslicer-android-cli PRIVATE slic3r-app-android-core)
    target_link_options(prusaslicer-android-cli PRIVATE ${_android_link_options})
    set_target_properties(prusaslicer-android-cli PROPERTIES OUTPUT_NAME prusa-slicer)

    # Session reuse test (not part of the default build).
    add_executable(prusaslicer-android-session-test EXCLUDE_FROM_ALL test/SessionTest.cpp)
    target_link_libraries(prusaslicer-android-session-test PRIVATE slic3r-app-android-core)
    target_link_options(prusaslicer-android-session-test PRIVATE ${_android_link_options})
    set_target_properties(prusaslicer-android-session-test PROPERTIES OUTPUT_NAME session-test)

    install(TARGETS prusaslicer-jni LIBRARY DESTINATION lib)
    install(TARGETS prusaslicer-android-cli RUNTIME DESTINATION bin)
else ()
    add_executable(mobile-headless test/MobileHeadless.cpp)
    target_link_libraries(mobile-headless PRIVATE slic3r-mobile-gui stb_image EGL GLESv2)
endif ()
