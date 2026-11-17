// JNI bridge of the PrusaSlicer GUI (org.prusaslicer.android.GuiNative).
//
// All native methods except nativeCreate must be called on the GLSurfaceView render thread,
// which acts as PrusaSlicer's main thread. The Java host object receives callbacks
// (IMobileHost) on that thread too and forwards them to the UI thread as needed.

#include "AndroidCli.hpp"
#include "gui/MobileApp.hpp"

#include "Slic3r/Log.hpp"
#include "Slic3r/Version.hpp"

#include <android/log.h>
#include <jni.h>
#include <pthread.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdio>
#include <cstring>

#include <exception>
#include <memory>
#include <string>
#include <vector>

using Slic3r::App::Mobile::IMobileHost;
using Slic3r::App::Mobile::MobileApp;
using Slic3r::App::Mobile::MobileRenderCanvas;

namespace {

constexpr const char* log_tag = "PrusaSlicerGui";

JavaVM* g_vm = nullptr;
pthread_key_t g_detach_key;
pthread_once_t g_detach_key_once = PTHREAD_ONCE_INIT;

// Native threads (job workers, TBB) that call into Java get attached to the VM. Android aborts
// the process when an attached thread exits without detaching, so detach on thread exit.
void detach_current_thread(void*)
{
    if (g_vm != nullptr) {
        g_vm->DetachCurrentThread();
    }
}

JNIEnv* env_for_thread()
{
    JNIEnv* env = nullptr;
    if (g_vm == nullptr) {
        return nullptr;
    }
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return nullptr;
        }
        pthread_once(&g_detach_key_once, []() { pthread_key_create(&g_detach_key, detach_current_thread); });
        pthread_setspecific(g_detach_key, env); // non-null value -> destructor runs at thread exit
    }
    return env;
}

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

std::vector<std::string> to_strings(JNIEnv* env, jobjectArray array)
{
    std::vector<std::string> result;
    const jsize count = array != nullptr ? env->GetArrayLength(array) : 0;
    for (jsize i = 0; i < count; ++i) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        result.push_back(to_string(env, item));
        env->DeleteLocalRef(item);
    }
    return result;
}

/// IMobileHost forwarding to the Java GuiHost object.
class JniHost final : public IMobileHost
{
public:
    JniHost(JNIEnv* env, jobject host) : m_host(env->NewGlobalRef(host))
    {
        jclass cls          = env->GetObjectClass(host);
        m_request_render    = env->GetMethodID(cls, "requestRender", "()V");
        m_show_message      = env->GetMethodID(cls, "showMessage", "(ILjava/lang/String;Ljava/lang/String;)V");
        m_ask_question      = env->GetMethodID(cls, "askQuestion", "(ILjava/lang/String;Ljava/lang/String;[Ljava/lang/String;)V");
        m_pick_files        = env->GetMethodID(cls, "pickFiles", "(IILjava/lang/String;Ljava/lang/String;)V");
        m_offer_output_file = env->GetMethodID(cls, "offerOutputFile", "(Ljava/lang/String;)V");
        m_open_url          = env->GetMethodID(cls, "openUrl", "(Ljava/lang/String;)V");
        m_set_text_input    = env->GetMethodID(cls, "setTextInputActive", "(Z)V");
        m_open_webview      = env->GetMethodID(cls, "openWebView", "(ILjava/lang/String;Ljava/lang/String;[Ljava/lang/String;)V");
        m_webview_command   = env->GetMethodID(cls, "webViewCommand", "(IILjava/lang/String;Ljava/lang/String;)V");
        env->DeleteLocalRef(cls);
    }

    void request_render() override { call(m_request_render); }

    void show_message(MessageKind kind, const std::string& title, const std::string& text) override
    {
        with_env([&](JNIEnv* env) {
            jstring jtitle = env->NewStringUTF(title.c_str());
            jstring jtext  = env->NewStringUTF(text.c_str());
            env->CallVoidMethod(m_host, m_show_message, jint(kind), jtitle, jtext);
            env->DeleteLocalRef(jtitle);
            env->DeleteLocalRef(jtext);
        });
    }

    void ask_question(int id, const std::string& title, const std::string& text, const std::vector<std::string>& buttons) override
    {
        with_env([&](JNIEnv* env) {
            jclass string_cls  = env->FindClass("java/lang/String");
            jobjectArray array = env->NewObjectArray(jsize(buttons.size()), string_cls, nullptr);
            for (size_t i = 0; i < buttons.size(); ++i) {
                jstring s = env->NewStringUTF(buttons[i].c_str());
                env->SetObjectArrayElement(array, jsize(i), s);
                env->DeleteLocalRef(s);
            }
            jstring jtitle = env->NewStringUTF(title.c_str());
            jstring jtext  = env->NewStringUTF(text.c_str());
            env->CallVoidMethod(m_host, m_ask_question, jint(id), jtitle, jtext, array);
            env->DeleteLocalRef(jtitle);
            env->DeleteLocalRef(jtext);
            env->DeleteLocalRef(array);
            env->DeleteLocalRef(string_cls);
        });
    }

    void pick_files(int id, FileRequestKind kind, const std::string& title, const std::string& wildcards) override
    {
        with_env([&](JNIEnv* env) {
            jstring jtitle = env->NewStringUTF(title.c_str());
            jstring jwild  = env->NewStringUTF(wildcards.c_str());
            env->CallVoidMethod(m_host, m_pick_files, jint(id), jint(kind), jtitle, jwild);
            env->DeleteLocalRef(jtitle);
            env->DeleteLocalRef(jwild);
        });
    }

    void offer_output_file(const std::string& path) override { call_string(m_offer_output_file, path); }
    void open_url(const std::string& url) override { call_string(m_open_url, url); }

    void set_text_input_active(bool active) override
    {
        with_env([&](JNIEnv* env) { env->CallVoidMethod(m_host, m_set_text_input, jboolean(active)); });
    }

    void open_webview(int id, const std::string& title, const std::string& url, const std::vector<std::string>& handler_names) override
    {
        with_env([&](JNIEnv* env) {
            jclass string_cls  = env->FindClass("java/lang/String");
            jobjectArray names = env->NewObjectArray(jsize(handler_names.size()), string_cls, nullptr);
            for (size_t i = 0; i < handler_names.size(); ++i) {
                jstring s = env->NewStringUTF(handler_names[i].c_str());
                env->SetObjectArrayElement(names, jsize(i), s);
                env->DeleteLocalRef(s);
            }
            jstring jtitle = env->NewStringUTF(title.c_str());
            jstring jurl   = env->NewStringUTF(url.c_str());
            env->CallVoidMethod(m_host, m_open_webview, jint(id), jtitle, jurl, names);
            env->DeleteLocalRef(jtitle);
            env->DeleteLocalRef(jurl);
            env->DeleteLocalRef(names);
            env->DeleteLocalRef(string_cls);
        });
    }

    void webview_command(int id, WebViewCommand command, const std::string& data, const std::string& extra) override
    {
        with_env([&](JNIEnv* env) {
            jstring jdata  = env->NewStringUTF(data.c_str());
            jstring jextra = env->NewStringUTF(extra.c_str());
            env->CallVoidMethod(m_host, m_webview_command, jint(id), jint(command), jdata, jextra);
            env->DeleteLocalRef(jdata);
            env->DeleteLocalRef(jextra);
        });
    }

private:
    template<typename Fn> void with_env(Fn&& fn)
    {
        if (JNIEnv* env = env_for_thread(); env != nullptr) {
            fn(env);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
        }
    }
    void call(jmethodID method)
    {
        with_env([&](JNIEnv* env) { env->CallVoidMethod(m_host, method); });
    }
    void call_string(jmethodID method, const std::string& value)
    {
        with_env([&](JNIEnv* env) {
            jstring s = env->NewStringUTF(value.c_str());
            env->CallVoidMethod(m_host, method, s);
            env->DeleteLocalRef(s);
        });
    }

    jobject m_host;
    jmethodID m_request_render;
    jmethodID m_show_message;
    jmethodID m_ask_question;
    jmethodID m_pick_files;
    jmethodID m_offer_output_file;
    jmethodID m_open_url;
    jmethodID m_set_text_input;
    jmethodID m_open_webview;
    jmethodID m_webview_command;
};

// ---- Crash reporting ---------------------------------------------------------------------
// Writes the signal and a backtrace (library + offset, resolvable against the unstripped
// library) into <data dir>/last_crash.txt, which the app offers to share on the next start.

int g_crash_fd = -1;
struct sigaction g_old_actions[NSIG];

void write_str(const char* s) { if (g_crash_fd >= 0) (void) !::write(g_crash_fd, s, strlen(s)); }

_Unwind_Reason_Code unwind_callback(struct _Unwind_Context* context, void* arg)
{
    int& depth = *static_cast<int*>(arg);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc == 0 || depth++ > 64) {
        return _URC_END_OF_STACK;
    }
    char line[512];
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(pc), &info) != 0 && info.dli_fname != nullptr) {
        const char* lib = strrchr(info.dli_fname, '/');
        snprintf(line, sizeof(line), "  #%02d pc %08zx  %s (%s)\n", depth, size_t(pc - uintptr_t(info.dli_fbase)),
                 lib != nullptr ? lib + 1 : info.dli_fname, info.dli_sname != nullptr ? info.dli_sname : "?");
    } else {
        snprintf(line, sizeof(line), "  #%02d pc %016zx\n", depth, size_t(pc));
    }
    write_str(line);
    return _URC_NO_REASON;
}

void crash_handler(int sig, siginfo_t* info, void* ucontext)
{
    char line[128];
    snprintf(line, sizeof(line), "Fatal signal %d (code %d, fault addr %p)\n", sig, info != nullptr ? info->si_code : 0,
             info != nullptr ? info->si_addr : nullptr);
    write_str(line);
    int depth = 0;
    _Unwind_Backtrace(unwind_callback, &depth);
    if (g_crash_fd >= 0) {
        ::fsync(g_crash_fd);
    }
    // Hand over to the previous handler (Android's debuggerd) to produce the usual tombstone.
    sigaction(sig, &g_old_actions[sig], nullptr);
    raise(sig);
}

void install_crash_handler(const std::string& data_dir)
{
    const std::string path = data_dir + "/last_crash.txt";
    g_crash_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    // Empty file = no crash; it is only non-empty when the handler wrote into it.
    struct sigaction action{};
    action.sa_sigaction = crash_handler;
    action.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
        sigaction(sig, &action, &g_old_actions[sig]);
    }
}

std::unique_ptr<JniHost> g_host;
std::unique_ptr<MobileApp> g_app;

template<typename Fn> void run_guarded(const char* what, Fn&& fn)
{
    try {
        fn();
    } catch (const std::exception& ex) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "%s failed: %s", what, ex.what());
        SPDLOG_ERROR("{} failed: {}", what, ex.what());
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "%s failed with an unknown exception", what);
    }
}

template<typename R, typename Fn> R guarded(const char* what, Fn&& fn, R fallback)
{
    R result = fallback;
    run_guarded(what, [&]() { result = fn(); });
    return result;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_org_prusaslicer_android_GuiNative_nativeCreate(
    JNIEnv* env, jclass, jstring resources_dir, jstring data_dir, jstring cache_dir, jobject host
)
{
    env->GetJavaVM(&g_vm);
    return guarded("nativeCreate", [&]() -> jint {
        if (g_app) {
            return 0;
        }
        const std::string data = to_string(env, data_dir);
        install_crash_handler(data);
        Slic3r::App::Android::initialize(to_string(env, resources_dir), data, to_string(env, cache_dir));
        SPDLOG_INFO("{} GUI starting", SLIC3R_BUILD_ID);
        g_host = std::make_unique<JniHost>(env, host);
        g_app  = std::make_unique<MobileApp>(*g_host);
        return 0;
    }, jint(-1));
}

JNIEXPORT jint JNICALL Java_org_prusaslicer_android_GuiNative_nativeSurfaceCreated(JNIEnv*, jclass)
{
    return guarded("nativeSurfaceCreated", [&]() -> jint {
        if (!g_app) return -1;
        g_app->on_gl_ready();
        return 0;
    }, jint(-1));
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeSurfaceChanged(JNIEnv*, jclass, jint width, jint height, jfloat scale)
{
    run_guarded("nativeSurfaceChanged", [&]() { if (g_app) g_app->on_surface_changed(width, height, scale); });
}

JNIEXPORT jboolean JNICALL Java_org_prusaslicer_android_GuiNative_nativeDrawFrame(JNIEnv*, jclass)
{
    return guarded("nativeDrawFrame", [&]() -> jboolean { return g_app && g_app->render_frame(); }, jboolean(false));
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeTouch(
    JNIEnv* env, jclass, jint action, jfloatArray jxs, jfloatArray jys
)
{
    run_guarded("nativeTouch", [&]() {
        if (!g_app) return;
        const jsize count = env->GetArrayLength(jxs);
        std::vector<float> xs(count), ys(count);
        env->GetFloatArrayRegion(jxs, 0, count, xs.data());
        env->GetFloatArrayRegion(jys, 0, count, ys.data());
        g_app->canvas().on_touch(MobileRenderCanvas::TouchAction(action), int(count), xs.data(), ys.data());
    });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeMouseButton(JNIEnv*, jclass, jint button, jboolean down, jfloat x, jfloat y)
{
    run_guarded("nativeMouseButton", [&]() { if (g_app) g_app->canvas().on_mouse_button(button, down, x, y); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeMouseMove(JNIEnv*, jclass, jfloat x, jfloat y)
{
    run_guarded("nativeMouseMove", [&]() { if (g_app) g_app->canvas().on_mouse_move(x, y); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeMouseWheel(JNIEnv*, jclass, jfloat x, jfloat y, jfloat dx, jfloat dy)
{
    run_guarded("nativeMouseWheel", [&]() { if (g_app) g_app->canvas().on_mouse_wheel(x, y, dx, dy); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeText(JNIEnv* env, jclass, jstring text)
{
    run_guarded("nativeText", [&]() { if (g_app) g_app->canvas().on_text(to_string(env, text)); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeKey(JNIEnv*, jclass, jint key_code, jboolean down)
{
    run_guarded("nativeKey", [&]() { if (g_app) g_app->canvas().on_key(key_code, down); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeOpenFiles(JNIEnv* env, jclass, jobjectArray paths)
{
    run_guarded("nativeOpenFiles", [&]() { if (g_app) g_app->open_files(to_strings(env, paths)); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeQuestionAnswered(JNIEnv*, jclass, jint id, jint index)
{
    run_guarded("nativeQuestionAnswered", [&]() { if (g_app) g_app->on_question_answered(id, index); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeFilesPicked(JNIEnv* env, jclass, jint id, jobjectArray paths)
{
    run_guarded("nativeFilesPicked", [&]() { if (g_app) g_app->on_files_picked(id, to_strings(env, paths)); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeOpenUrl(JNIEnv* env, jclass, jstring url)
{
    run_guarded("nativeOpenUrl", [&]() { if (g_app) g_app->open_app_url(to_string(env, url)); });
}

JNIEXPORT jboolean JNICALL Java_org_prusaslicer_android_GuiNative_nativeWebViewNavigation(
    JNIEnv* env, jclass, jint id, jstring url, jstring current_url
)
{
    return guarded("nativeWebViewNavigation", [&]() -> jboolean {
        return !g_app || g_app->on_webview_navigation(id, to_string(env, url), to_string(env, current_url));
    }, jboolean(true));
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeWebViewLoaded(JNIEnv* env, jclass, jint id, jstring url)
{
    run_guarded("nativeWebViewLoaded", [&]() { if (g_app) g_app->on_webview_loaded(id, to_string(env, url)); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeWebViewScriptMessage(JNIEnv* env, jclass, jint id, jstring message)
{
    run_guarded("nativeWebViewScriptMessage", [&]() { if (g_app) g_app->on_webview_script_message(id, to_string(env, message)); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeWebViewError(JNIEnv* env, jclass, jint id, jstring description)
{
    run_guarded("nativeWebViewError", [&]() { if (g_app) g_app->on_webview_error(id, to_string(env, description)); });
}

JNIEXPORT void JNICALL Java_org_prusaslicer_android_GuiNative_nativeWebViewDismissed(JNIEnv*, jclass, jint id)
{
    run_guarded("nativeWebViewDismissed", [&]() { if (g_app) g_app->on_webview_dismissed(id); });
}

JNIEXPORT jstring JNICALL Java_org_prusaslicer_android_GuiNative_nativeOutputDir(JNIEnv* env, jclass)
{
    const std::string dir = g_app ? g_app->output_dir() : std::string{};
    return env->NewStringUTF(dir.c_str());
}

} // extern "C"
