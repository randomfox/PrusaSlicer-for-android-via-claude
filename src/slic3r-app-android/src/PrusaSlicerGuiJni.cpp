// JNI bridge of the PrusaSlicer GUI (org.prusaslicer.android.GuiNative).
//
// All native methods except nativeCreate must be called on the GLSurfaceView render thread,
// which acts as PrusaSlicer's main thread. The Java host object receives callbacks
// (IMobileHost) on that thread too and forwards them to the UI thread as needed.

#include "AndroidCli.hpp"
#include "gui/MobileApp.hpp"

#include "Slic3r/Log.hpp"

#include <android/log.h>
#include <jni.h>

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

JNIEnv* env_for_thread()
{
    JNIEnv* env = nullptr;
    if (g_vm == nullptr) {
        return nullptr;
    }
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        g_vm->AttachCurrentThread(&env, nullptr);
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
};

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
        Slic3r::App::Android::initialize(to_string(env, resources_dir), to_string(env, data_dir), to_string(env, cache_dir));
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

JNIEXPORT jstring JNICALL Java_org_prusaslicer_android_GuiNative_nativeOutputDir(JNIEnv* env, jclass)
{
    const std::string dir = g_app ? g_app->output_dir() : std::string{};
    return env->NewStringUTF(dir.c_str());
}

} // extern "C"
