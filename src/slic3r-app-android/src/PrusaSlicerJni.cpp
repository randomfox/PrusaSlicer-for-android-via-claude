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

#include "AndroidCli.hpp"
#include "Slic3r/Version.hpp"

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

    try {
        Slic3r::App::Android::initialize(resources, data, cache);
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
        return Slic3r::App::Android::run(std::move(args));
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
