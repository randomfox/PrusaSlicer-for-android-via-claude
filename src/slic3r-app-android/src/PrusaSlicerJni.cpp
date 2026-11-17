// JNI bridge between the Android app and the PrusaSlicer command line interface.
//
// The Java side (org.prusaslicer.android.NativeSlicer) calls:
//   int  nativeInit(String resourcesDir, String dataDir, String cacheDir)
//   int  nativeRun(String[] args, String logFile)
//
// nativeRun() parses the arguments with the very same CLI11 definition as the desktop
// launcher and then runs Slic3r::App::CLI::run(), so all CLI features (preset selection,
// config overrides, --export-gcode, --query-printer-models, ...) are available. stdout and
// stderr are redirected into logFile for the duration of the call, so the app can show
// what happened.

#include "Slic3r/App/CLI/CLIApp.hpp"
#include "Slic3r/App/Launcher/ReadCLI.hpp"
#include "Slic3r/App/AppConfig.hpp"
#include "Slic3r/App/AppServices.hpp"
#include "Slic3r/App/Init.hpp"
#include "Slic3r/Assert.hpp"
#include "Slic3r/Log.hpp"
#include "Slic3r/Version.hpp"
#include "libslic3r/Utils.hpp"

#include <CLI/CLI.hpp>

#include <android/log.h>
#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char* log_tag = "PrusaSlicer";

std::mutex g_run_mutex;
bool g_initialized = false;

std::string to_string(JNIEnv* env, jstring str)
{
    if (str == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(str, nullptr);
    std::string result{chars};
    env->ReleaseStringUTFChars(str, chars);
    return result;
}

// Redirects stdout/stderr into a file for the lifetime of the object.
class OutputRedirect
{
public:
    explicit OutputRedirect(const std::string& path)
    {
        if (path.empty()) {
            return;
        }
        std::fflush(stdout);
        std::fflush(stderr);
        m_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (m_fd < 0) {
            return;
        }
        m_saved_out = ::dup(STDOUT_FILENO);
        m_saved_err = ::dup(STDERR_FILENO);
        ::dup2(m_fd, STDOUT_FILENO);
        ::dup2(m_fd, STDERR_FILENO);
    }

    ~OutputRedirect()
    {
        if (m_fd < 0) {
            return;
        }
        std::fflush(stdout);
        std::fflush(stderr);
        ::dup2(m_saved_out, STDOUT_FILENO);
        ::dup2(m_saved_err, STDERR_FILENO);
        ::close(m_saved_out);
        ::close(m_saved_err);
        ::close(m_fd);
    }

    OutputRedirect(const OutputRedirect&) = delete;
    OutputRedirect& operator=(const OutputRedirect&) = delete;

private:
    int m_fd{-1};
    int m_saved_out{-1};
    int m_saved_err{-1};
};

int run_cli(std::vector<std::string> args)
{
    args.insert(args.begin(), "prusa-slicer");

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    CLI::App app{std::string{SLIC3R_BUILD_ID} + " for Android", "prusa-slicer"};
    Slic3r::App::InitParams init_params = Slic3r::App::Launcher::read_cli(
        app, SLIC3R_VERSION, static_cast<int>(args.size()), argv.data()
    );
    if (init_params.exit_code.has_value()) {
        return init_params.exit_code.value();
    }

    // Without --datadir, the default data directory is derived from XDG_CONFIG_HOME,
    // which nativeInit() points into the app's private storage.
    Slic3r::App::init_paths(init_params);

    if (init_params.misc.loglevel.has_value()) {
        Slic3r::set_log_level(init_params.misc.loglevel.value());
    }
    if (init_params.misc.threads.has_value()) {
        Slic3r::thread_count = init_params.misc.threads.value();
    }

    if (!init_params.action.has_any_action() || init_params.action.gcode_viewer) {
        std::fprintf(stderr, "No action given. The Android build supports command line actions only, e.g. --export-gcode.\n");
        return EXIT_FAILURE;
    }

    return Slic3r::App::CLI::run(init_params);
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_org_prusaslicer_android_NativeSlicer_nativeInit(
    JNIEnv* env, jclass, jstring resources_dir, jstring data_dir, jstring cache_dir
)
{
    std::lock_guard<std::mutex> lock{g_run_mutex};
    const std::string resources = to_string(env, resources_dir);
    const std::string data      = to_string(env, data_dir);
    const std::string cache     = to_string(env, cache_dir);

    // Directories.cpp resolves the platform directories from these on Linux-like systems.
    ::setenv("PRUSASLICER_RESOURCES_DIR", resources.c_str(), 1);
    ::setenv("HOME", data.c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", data.c_str(), 1);
    ::setenv("XDG_CACHE_HOME", cache.c_str(), 1);
    ::setenv("TMPDIR", cache.c_str(), 1);

    if (g_initialized) {
        return 0;
    }

    try {
        Slic3r::App::init_common();
        Slic3r::App::init_paths(Slic3r::App::InitParams{});
        Slic3r::init_assert();

        auto& app_services{Slic3r::App::AppServices::instance()};
        app_services.set_app_config(Slic3r::App::AppConfig::create_app_config());
    } catch (const std::exception& ex) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "Initialization failed: %s", ex.what());
        return -1;
    }

    g_initialized = true;
    __android_log_print(ANDROID_LOG_INFO, log_tag, "%s initialized, resources: %s", SLIC3R_BUILD_ID, resources.c_str());
    return 0;
}

JNIEXPORT jint JNICALL Java_org_prusaslicer_android_NativeSlicer_nativeRun(
    JNIEnv* env, jclass, jobjectArray jargs, jstring jlog_file
)
{
    std::lock_guard<std::mutex> lock{g_run_mutex};
    if (!g_initialized) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "nativeRun() called before nativeInit()");
        return -1;
    }

    std::vector<std::string> args;
    const jsize count = jargs != nullptr ? env->GetArrayLength(jargs) : 0;
    for (jsize i = 0; i < count; ++i) {
        auto jarg = static_cast<jstring>(env->GetObjectArrayElement(jargs, i));
        args.push_back(to_string(env, jarg));
        env->DeleteLocalRef(jarg);
    }

    OutputRedirect redirect{to_string(env, jlog_file)};
    try {
        return run_cli(std::move(args));
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Error: %s\n", ex.what());
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "Slicing failed: %s", ex.what());
    } catch (...) {
        std::fprintf(stderr, "Error: unknown exception\n");
    }
    return -1;
}

JNIEXPORT jstring JNICALL Java_org_prusaslicer_android_NativeSlicer_nativeVersion(JNIEnv* env, jclass)
{
    return env->NewStringUTF(SLIC3R_BUILD_ID);
}

} // extern "C"
