// Headless driver for the mobile UI layer (Linux test build, -DSLIC3R_MOBILE=ON).
//
// Runs the same MobileApp as the Android app, rendering into an EGL pbuffer (Mesa llvmpipe
// works fine), and executes a small script to interact with it and take screenshots:
//
//   mobile-headless [--size WxH] [--scale S] [--skip-welcome] <script>
//
// Script commands (one per line, coordinates in physical pixels, '#' starts a comment):
//   frames N               render N frames
//   wait_ready             render until the plater is up (presets loaded)
//   sleep MS               render frames for MS milliseconds
//   tap X Y                single finger tap
//   long_press X Y         single finger long press
//   drag X1 Y1 X2 Y2 [N]   single finger drag in N steps
//   pan X1 Y1 X2 Y2 [N]    two finger drag
//   pinch X Y D1 D2 [N]    two finger pinch around X,Y from distance D1 to D2
//   open PATH              open a model/project file
//   pick PATH              answer the next file picker with PATH
//   answer N               answer the next question with button N (default 0)
//   text STRING            type text
//   key CODE               press and release an Android key code
//   shot FILE.png          save a screenshot

#include "AndroidCli.hpp"
#include "gui/MobileApp.hpp"

#include "Slic3r/App/AppConfig.hpp"
#include "Slic3r/App/AppServices.hpp"
#include "Slic3r/Log.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Slic3r::App::Mobile::IMobileHost;
using Slic3r::App::Mobile::MobileApp;
using Slic3r::App::Mobile::MobileRenderCanvas;
using TouchAction = MobileRenderCanvas::TouchAction;

namespace {

class HeadlessHost final : public IMobileHost
{
public:
    bool render_requested{true};
    std::deque<std::string> pick_answers;
    std::deque<int> question_answers;
    std::vector<std::pair<int, std::vector<std::string>>> pending_picks;
    std::vector<std::pair<int, int>> pending_answers;

    void request_render() override { render_requested = true; }

    void show_message(MessageKind kind, const std::string& title, const std::string& text) override
    {
        std::printf("[host] message (%d) %s: %s\n", int(kind), title.c_str(), text.c_str());
    }

    void ask_question(int id, const std::string& title, const std::string& text, const std::vector<std::string>& buttons) override
    {
        const int answer = question_answers.empty() ? 0 : question_answers.front();
        if (!question_answers.empty()) {
            question_answers.pop_front();
        }
        std::printf("[host] question %s: %s -> %s\n", title.c_str(), text.c_str(),
                    answer >= 0 && answer < int(buttons.size()) ? buttons[answer].c_str() : "dismissed");
        pending_answers.emplace_back(id, answer);
    }

    void pick_files(int id, FileRequestKind, const std::string& title, const std::string&) override
    {
        std::vector<std::string> files;
        if (!pick_answers.empty()) {
            files.push_back(pick_answers.front());
            pick_answers.pop_front();
        }
        std::printf("[host] file picker \"%s\" -> %s\n", title.c_str(), files.empty() ? "(cancelled)" : files[0].c_str());
        pending_picks.emplace_back(id, files);
    }

    void offer_output_file(const std::string& path) override { std::printf("[host] output file %s\n", path.c_str()); }
    void open_url(const std::string& url) override { std::printf("[host] open url %s\n", url.c_str()); }
    void set_text_input_active(bool active) override { std::printf("[host] soft keyboard %s\n", active ? "shown" : "hidden"); }
};

struct Headless
{
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLSurface surface{EGL_NO_SURFACE};
    EGLContext context{EGL_NO_CONTEXT};
    int width{1080};
    int height{2400};

    void init()
    {
        auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        EGLint major = 0, minor = 0;
        if (!eglInitialize(display, &major, &minor)) {
            throw std::runtime_error("eglInitialize failed");
        }
        const EGLint config_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                         EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count == 0) {
            throw std::runtime_error("eglChooseConfig failed");
        }
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint context_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
        const EGLint surface_attribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, surface_attribs);
        if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE) {
            throw std::runtime_error("EGL context/surface creation failed");
        }
        eglMakeCurrent(display, surface, surface, context);
    }

    void screenshot(const std::string& file) const
    {
        std::vector<unsigned char> pixels(size_t(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        stbi_flip_vertically_on_write(1);
        stbi_write_png(file.c_str(), width, height, 4, pixels.data(), width * 4);
        std::printf("[headless] screenshot %s\n", file.c_str());
    }
};

} // namespace

int main(int argc, char** argv)
{
    Headless gl;
    float scale = 2.75f;
    bool skip_welcome = false;
    std::string script_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--size" && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &gl.width, &gl.height);
        } else if (arg == "--skip-welcome") {
            skip_welcome = true;
        } else if (arg == "--scale" && i + 1 < argc) {
            scale = std::stof(argv[++i]);
        } else {
            script_path = arg;
        }
    }
    std::ifstream script_file(script_path);
    if (!script_file) {
        std::fprintf(stderr, "usage: %s [--size WxH] [--scale S] <script>\n", argv[0]);
        return 2;
    }

    const char* home = std::getenv("HOME");
    const std::string data = home != nullptr ? home : ".";
    Slic3r::App::Android::initialize(std::getenv("PRUSASLICER_RESOURCES_DIR"), data + "/.config", data + "/.cache");

    if (skip_welcome) {
        // Pretend the first-run wizard was completed.
        Slic3r::App::AppServices::instance().app_config().set("initialized", true);
    }

    gl.init();
    HeadlessHost host;
    MobileApp app(host);
    app.on_gl_ready();
    app.on_surface_changed(gl.width, gl.height, scale);

    auto frame = [&]() {
        host.render_requested = false;
        app.render_frame();
        // Deliver the host's answers like the Android side does: on the next frame.
        auto picks = std::move(host.pending_picks);
        host.pending_picks.clear();
        for (auto& [id, files] : picks) {
            app.on_files_picked(id, files);
        }
        auto answers = std::move(host.pending_answers);
        host.pending_answers.clear();
        for (auto& [id, index] : answers) {
            app.on_question_answered(id, index);
        }
    };
    auto frames = [&](int n) { for (int i = 0; i < n; ++i) frame(); };
    auto touch = [&](TouchAction action, std::vector<float> xs, std::vector<float> ys) {
        app.canvas().on_touch(action, int(xs.size()), xs.data(), ys.data());
        frame();
    };

    std::string line;
    while (std::getline(script_file, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream in(line);
        std::string cmd;
        if (!(in >> cmd)) {
            continue;
        }
        std::printf("[headless] %s\n", line.c_str());
        std::fflush(stdout);
        if (cmd == "frames") {
            int n = 1; in >> n; frames(n);
        } else if (cmd == "wait_ready") {
            const auto start = std::chrono::steady_clock::now();
            while (!app.is_ready() && std::chrono::steady_clock::now() - start < std::chrono::seconds(120)) {
                frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            frames(10);
            std::printf("[headless] ready=%d\n", int(app.is_ready()));
        } else if (cmd == "sleep") {
            int ms = 0; in >> ms;
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < end) {
                frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        } else if (cmd == "tap") {
            float x, y; in >> x >> y;
            touch(TouchAction::Down, {x}, {y});
            touch(TouchAction::Up, {x}, {y});
            frames(3);
        } else if (cmd == "long_press") {
            float x, y; in >> x >> y;
            touch(TouchAction::Down, {x}, {y});
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
            while (std::chrono::steady_clock::now() < end) { frame(); std::this_thread::sleep_for(std::chrono::milliseconds(15)); }
            touch(TouchAction::Up, {x}, {y});
            frames(3);
        } else if (cmd == "drag" || cmd == "pan") {
            float x1, y1, x2, y2; int n = 10; in >> x1 >> y1 >> x2 >> y2 >> n;
            const float off = cmd == "pan" ? 150.f : 0.f;
            auto pts = [&](float t) {
                const float x = x1 + (x2 - x1) * t, y = y1 + (y2 - y1) * t;
                return cmd == "pan" ? std::pair{std::vector<float>{x - off, x + off}, std::vector<float>{y, y}}
                                    : std::pair{std::vector<float>{x}, std::vector<float>{y}};
            };
            auto [xs0, ys0] = pts(0);
            touch(TouchAction::Down, {xs0[0]}, {ys0[0]});
            if (cmd == "pan") touch(TouchAction::Down, xs0, ys0);
            for (int i = 1; i <= n; ++i) { auto [xs, ys] = pts(float(i) / n); touch(TouchAction::Move, xs, ys); }
            auto [xs1, ys1] = pts(1);
            touch(TouchAction::Up, xs1, ys1);
            if (cmd == "pan") touch(TouchAction::Up, {xs1[0]}, {ys1[0]});
            frames(3);
        } else if (cmd == "pinch") {
            float x, y, d1, d2; int n = 10; in >> x >> y >> d1 >> d2 >> n;
            auto pts = [&](float d) { return std::pair{std::vector<float>{x - d / 2, x + d / 2}, std::vector<float>{y, y}}; };
            auto [xs0, ys0] = pts(d1);
            touch(TouchAction::Down, {xs0[0]}, {ys0[0]});
            touch(TouchAction::Down, xs0, ys0);
            for (int i = 1; i <= n; ++i) { auto [xs, ys] = pts(d1 + (d2 - d1) * i / n); touch(TouchAction::Move, xs, ys); }
            auto [xs1, ys1] = pts(d2);
            touch(TouchAction::Up, xs1, ys1);
            touch(TouchAction::Up, {xs1[0]}, {ys1[0]});
            frames(3);
        } else if (cmd == "open") {
            std::string path; in >> path; app.open_files({path}); frames(5);
        } else if (cmd == "pick") {
            std::string path; in >> path; host.pick_answers.push_back(path);
        } else if (cmd == "answer") {
            int n = 0; in >> n; host.question_answers.push_back(n);
        } else if (cmd == "text") {
            std::string rest; std::getline(in, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            app.canvas().on_text(rest); frames(3);
        } else if (cmd == "key") {
            int code = 0; in >> code; app.canvas().on_key(code, true); app.canvas().on_key(code, false); frames(3);
        } else if (cmd == "shot") {
            std::string file; in >> file; frames(2); gl.screenshot(file);
        } else {
            std::printf("[headless] unknown command %s\n", cmd.c_str());
        }
        std::fflush(stdout);
    }
    std::fflush(nullptr);
    std::_Exit(0);
}
