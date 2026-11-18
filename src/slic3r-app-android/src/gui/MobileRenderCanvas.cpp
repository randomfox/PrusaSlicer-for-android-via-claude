#include "MobileRenderCanvas.hpp"

#include "Slic3r/App/AppConfig.hpp"
#include "Slic3r/App/AppServices.hpp"
#include "Slic3r/App/Platform/PlatformError.hpp"
#include "Slic3r/App/Render/Context.hpp"
#include "Slic3r/App/Render/Device.hpp"
#include "Slic3r/App/Render/Init.hpp"
#include "Slic3r/App/Theme.hpp"
#include "Slic3r/Log.hpp"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <imgui/backends/imgui_impl_opengl3.h>

#include <GL/glew.h>

#include <algorithm>
#include <cmath>

namespace Slic3r::App::Mobile {

using Platform::KeyboardEvent;
using Platform::KeyCode;
using Platform::KeyModifier;
using Platform::KeyModifiers;
using Platform::MouseButton;
using Platform::MouseEvent;

namespace {
// Gesture tuning, in UI (logical) pixels and milliseconds.
constexpr float touch_slop          = 8.f;
constexpr int long_press_ms         = 450;
constexpr float pinch_wheel_factor  = 600.f; // wheel units (120 = one notch) per doubling of the finger distance

int imgui_button(MouseButton button)
{
    switch (button) {
    case MouseButton::Left: return ImGuiMouseButton_Left;
    case MouseButton::Right: return ImGuiMouseButton_Right;
    case MouseButton::Middle: return ImGuiMouseButton_Middle;
    default: return -1;
    }
}

// android.view.KeyEvent key codes.
struct AndroidKey
{
    int code;
    ImGuiKey imgui;
    KeyCode slic3r;
};
constexpr AndroidKey android_keys[] = {
    {66, ImGuiKey_Enter, KeyCode::Return},      // KEYCODE_ENTER
    {67, ImGuiKey_Backspace, KeyCode::Backspace}, // KEYCODE_DEL
    {112, ImGuiKey_Delete, KeyCode::Delete},    // KEYCODE_FORWARD_DEL
    {111, ImGuiKey_Escape, KeyCode::Escape},    // KEYCODE_ESCAPE
    {4, ImGuiKey_Escape, KeyCode::Escape},      // KEYCODE_BACK
    {61, ImGuiKey_Tab, KeyCode::Tab},           // KEYCODE_TAB
    {21, ImGuiKey_LeftArrow, KeyCode::Left},    // KEYCODE_DPAD_LEFT
    {22, ImGuiKey_RightArrow, KeyCode::Right},  // KEYCODE_DPAD_RIGHT
    {19, ImGuiKey_UpArrow, KeyCode::Up},        // KEYCODE_DPAD_UP
    {20, ImGuiKey_DownArrow, KeyCode::Down},    // KEYCODE_DPAD_DOWN
    {122, ImGuiKey_Home, KeyCode::Home},        // KEYCODE_MOVE_HOME
    {123, ImGuiKey_End, KeyCode::End},          // KEYCODE_MOVE_END
};
} // namespace

MobileRenderCanvas::MobileRenderCanvas(IMobileHost& host) : m_host(host) {}

MobileRenderCanvas::~MobileRenderCanvas()
{
    if (m_gl_initialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
    }
}

void MobileRenderCanvas::init_gl()
{
    if (m_gl_initialized) {
        return;
    }
    glGetError();

    Render::initialize_render();

    const Semver gl_version = Render::Context::instance().gl_version();
    SPDLOG_INFO(
        "OpenGL: {} / {} / GLSL {}",
        Render::Context::instance().gl_version_string(),
        Render::Context::instance().gl_renderer_string(),
        Render::Context::instance().glsl_version_string()
    );
    if (gl_version.maj() < 3 || (gl_version.maj() == 3 && gl_version.min() < 2)) {
        throw Platform::PlatformError("OpenGL ES 3.2 or newer is required");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.BackendPlatformName = "slic3r-mobile";
    ImGui::StyleColorsDark();
    AppServices::instance().theme().initialize_imgui_style();

    ImGui_ImplOpenGL3_Init("#version 300 es");
    m_gl_initialized = true;
}

void MobileRenderCanvas::set_surface(int width_px, int height_px, float ui_scale)
{
    m_width_px  = std::max(width_px, 1);
    m_height_px = std::max(height_px, 1);
    m_ui_scale  = ui_scale > 0.1f ? ui_scale : 1.f;
    AbstractRenderCanvas::request_render();
}

bool MobileRenderCanvas::render_frame()
{
    m_main_thread_dispatcher.dispatch_enqueued();
    update_long_press();
    update_fling();
    get_and_reset_render_requested();
    render();

    if (ImGui::GetCurrentContext() != nullptr) {
        const bool wants_text = ImGui::GetIO().WantTextInput;
        if (wants_text != m_text_input_active) {
            m_text_input_active = wants_text;
            m_host.set_text_input_active(wants_text);
        }
    }

    const bool more = m_render_request_count > 0 || m_gesture == Gesture::Pending || m_scroll_velocity != 0.f;
    return more;
}

bool MobileRenderCanvas::begin_frame_platform()
{
    ImGuiIO& io = ImGui::GetIO();
    const float w = float(m_width_px) / m_ui_scale;
    const float h = float(m_height_px) / m_ui_scale;
    io.DisplaySize             = ImVec2(w, h);
    io.DisplayFramebufferScale = ImVec2(m_ui_scale, m_ui_scale);

    const float font_size_pt = float(AppServices::instance().app_config().get<int>("font_size"));
    const float font_size_px = font_size_pt * 1.3333f;
    ImGuiStyle& style        = ImGui::GetStyle();
    style.FontScaleDpi       = 1;
    style.FontScaleMain      = 1;
    style.FontSizeBase       = font_size_px;
    // Fingers are less precise than a mouse: widgets react a little outside their bounds.
    style.TouchExtraPadding = ImVec2(4.f, 4.f);

    const int dpi = int(96.f * m_ui_scale + 0.5f);
    set_screen_size({size_t(m_width_px), size_t(m_height_px), m_ui_scale, dpi, font_size_px, font_size_pt});
    return true;
}

void MobileRenderCanvas::end_imgui_frame_platform() {}

void MobileRenderCanvas::end_frame_platform()
{
    // The host presents the frame (eglSwapBuffers by GLSurfaceView, or a read-back).
    glFlush();
}

double MobileRenderCanvas::platform_time()
{
    return std::chrono::duration<double>(Clock::now() - m_start_time).count();
}

Render::Device& MobileRenderCanvas::device()
{
    return Render::Context::instance().device();
}

void MobileRenderCanvas::on_render_requested()
{
    m_host.request_render();
}

// ---- Input ----------------------------------------------------------------------------------

void MobileRenderCanvas::set_pointer(Point p)
{
    m_mouse_x = int(p.x);
    m_mouse_y = int(p.y);
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    }
}

void MobileRenderCanvas::send_move(Point p)
{
    set_pointer(p);
    enqueue_mouse(MouseEvent{MouseEvent::Type::Move, MouseButton::NoButton, int(p.x), int(p.y), 0.f, 0.f, m_key_modifiers});
    AbstractRenderCanvas::request_render();
}

void MobileRenderCanvas::send_button(MouseButton button, bool down, Point p)
{
    // Only the position, no scene Move event: a Move without actual motion right before a
    // button press keeps click detectors (e.g. bed selection) probing, and the scene tools
    // would then not see the press (the camera would not rotate).
    set_pointer(p);
    if (ImGui::GetCurrentContext() != nullptr) {
        if (const int b = imgui_button(button); b >= 0) {
            ImGui::GetIO().AddMouseButtonEvent(b, down);
        }
    }
    enqueue_mouse(MouseEvent{
        down ? MouseEvent::Type::ButtonDown : MouseEvent::Type::ButtonUp,
        button, int(p.x), int(p.y), 0.f, 0.f, m_key_modifiers
    });
    AbstractRenderCanvas::request_render();
}

void MobileRenderCanvas::send_wheel(Point p, float delta_x, float delta_y)
{
    set_pointer(p);
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().AddMouseWheelEvent(delta_x / 120.f, delta_y / 120.f);
    }
    enqueue_mouse(MouseEvent{MouseEvent::Type::Wheel, MouseButton::NoButton, int(p.x), int(p.y), delta_x, delta_y, m_key_modifiers});
    AbstractRenderCanvas::request_render();
}

void MobileRenderCanvas::update_long_press()
{
    if (m_gesture != Gesture::Pending) {
        return;
    }
    const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m_touch_start_time);
    if (held.count() >= long_press_ms) {
        // Long press = right click (context menus).
        send_button(MouseButton::Right, true, m_touch_start);
        send_button(MouseButton::Right, false, m_touch_start);
        m_gesture = Gesture::Consumed;
    } else {
        AbstractRenderCanvas::request_render(); // keep the clock ticking
    }
}

bool MobileRenderCanvas::begin_touch_scroll(Point p)
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (g == nullptr) {
        return false;
    }
    const bool vertical = std::abs(p.y - m_touch_start.y) >= std::abs(p.x - m_touch_start.x);
    // The innermost window under the finger that can scroll that way (child windows first).
    for (ImGuiWindow* window = g->HoveredWindow; window != nullptr;
         window = (window->Flags & ImGuiWindowFlags_ChildWindow) ? window->ParentWindow : nullptr)
    {
        const float scroll_max = vertical ? window->ScrollMax.y : window->ScrollMax.x;
        if (scroll_max > 0.5f) {
            m_scroll_window_id  = window->ID;
            m_scroll_vertical   = vertical;
            m_scroll_start_value = vertical ? window->Scroll.y : window->Scroll.x;
            m_scroll_velocity   = 0.f;
            m_scroll_last_point = p;
            m_scroll_last_time  = Clock::now();
            return true;
        }
    }
    return false;
}

void MobileRenderCanvas::update_touch_scroll(Point p)
{
    ImGuiWindow* window = ImGui::FindWindowByID(m_scroll_window_id);
    if (window == nullptr) {
        return;
    }
    const float moved = m_scroll_vertical ? p.y - m_touch_start.y : p.x - m_touch_start.x;
    const float max   = m_scroll_vertical ? window->ScrollMax.y : window->ScrollMax.x;
    const float value = std::clamp(m_scroll_start_value - moved, 0.f, max);
    m_scroll_vertical ? ImGui::SetScrollY(window, value) : ImGui::SetScrollX(window, value);

    // Finger velocity (smoothed) for the fling.
    const auto now = Clock::now();
    const float dt = std::chrono::duration<float>(now - m_scroll_last_time).count();
    if (dt > 0.001f) {
        const float delta    = m_scroll_vertical ? p.y - m_scroll_last_point.y : p.x - m_scroll_last_point.x;
        const float velocity = -delta / dt;
        m_scroll_velocity    = 0.6f * velocity + 0.4f * m_scroll_velocity;
        m_scroll_last_point  = p;
        m_scroll_last_time   = now;
    }
    AbstractRenderCanvas::request_render();
}

void MobileRenderCanvas::update_fling()
{
    if (m_scroll_velocity == 0.f || m_gesture == Gesture::Scroll) {
        return;
    }
    ImGuiWindow* window = ImGui::GetCurrentContext() ? ImGui::FindWindowByID(m_scroll_window_id) : nullptr;
    const auto now      = Clock::now();
    const float dt      = std::min(std::chrono::duration<float>(now - m_fling_last_time).count(), 0.05f);
    m_fling_last_time   = now;
    if (window == nullptr || std::abs(m_scroll_velocity) < 30.f) {
        m_scroll_velocity = 0.f;
        return;
    }
    const float current = m_scroll_vertical ? window->Scroll.y : window->Scroll.x;
    const float max     = m_scroll_vertical ? window->ScrollMax.y : window->ScrollMax.x;
    const float value   = std::clamp(current + m_scroll_velocity * dt, 0.f, max);
    m_scroll_vertical ? ImGui::SetScrollY(window, value) : ImGui::SetScrollX(window, value);
    if (value <= 0.f || value >= max) {
        m_scroll_velocity = 0.f;
    } else {
        m_scroll_velocity *= std::exp(-dt * 3.5f); // friction
    }
    AbstractRenderCanvas::request_render();
}

void MobileRenderCanvas::on_touch(TouchAction action, int pointer_count, const float* xs, const float* ys)
{
    if (pointer_count <= 0) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    }
    const Point p0 = to_logical(xs[0], ys[0]);

    if (action == TouchAction::Cancel) {
        if (m_gesture == Gesture::Drag) {
            send_button(MouseButton::Left, false, m_touch_last);
        } else if (m_gesture == Gesture::TwoFinger) {
            send_button(MouseButton::Middle, false, m_two_finger_center);
        }
        m_gesture = Gesture::None;
        return;
    }

    if (pointer_count >= 2 && action != TouchAction::Up) {
        const Point p1     = to_logical(xs[1], ys[1]);
        const Point center = {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
        const float dist   = std::hypot(p1.x - p0.x, p1.y - p0.y);
        if (m_gesture != Gesture::TwoFinger) {
            // Second finger: end a one finger drag and start pan / pinch.
            if (m_gesture == Gesture::Drag) {
                send_button(MouseButton::Left, false, m_touch_last);
            }
            m_gesture             = Gesture::TwoFinger;
            m_two_finger_center   = center;
            m_two_finger_distance = std::max(dist, 1.f);
            send_button(MouseButton::Middle, true, center);
            return;
        }
        // Pan with the middle button, zoom with the wheel.
        send_move(center);
        m_two_finger_center = center;
        if (dist > 1.f) {
            const float ratio = dist / m_two_finger_distance;
            if (std::abs(ratio - 1.f) > 0.02f) {
                send_wheel(center, 0.f, std::log2(ratio) * pinch_wheel_factor);
                m_two_finger_distance = dist;
            }
        }
        return;
    }

    switch (action) {
    case TouchAction::Down:
        m_scroll_velocity = 0.f; // a touch stops a fling
        if (m_gesture == Gesture::None) {
            m_gesture          = Gesture::Pending;
            m_touch_start      = p0;
            m_touch_last       = p0;
            m_touch_start_time = Clock::now();
            set_pointer(p0);
            AbstractRenderCanvas::request_render(); // let ImGui see what is under the finger
        }
        break;

    case TouchAction::Move:
        if (m_gesture == Gesture::Pending
            && std::hypot(p0.x - m_touch_start.x, p0.y - m_touch_start.y) > touch_slop)
        {
            if (begin_touch_scroll(p0)) {
                m_gesture = Gesture::Scroll;
            } else {
                send_button(MouseButton::Left, true, m_touch_start);
                m_gesture = Gesture::Drag;
            }
        }
        if (m_gesture == Gesture::Drag) {
            send_move(p0);
        } else if (m_gesture == Gesture::Scroll) {
            update_touch_scroll(p0);
        }
        m_touch_last = p0;
        break;

    case TouchAction::Up:
        if (pointer_count > 1) {
            // One of two fingers lifted: finish the pan, ignore the rest of the gesture.
            if (m_gesture == Gesture::TwoFinger) {
                send_button(MouseButton::Middle, false, m_two_finger_center);
                m_gesture = Gesture::Consumed;
            }
            break;
        }
        if (m_gesture == Gesture::Pending) {
            // Tap.
            send_button(MouseButton::Left, true, m_touch_start);
            send_button(MouseButton::Left, false, m_touch_start);
        } else if (m_gesture == Gesture::Drag) {
            send_button(MouseButton::Left, false, p0);
        } else if (m_gesture == Gesture::Scroll) {
            // Fling if the finger was still moving when lifted.
            const float idle = std::chrono::duration<float>(Clock::now() - m_scroll_last_time).count();
            if (idle > 0.08f) {
                m_scroll_velocity = 0.f;
            }
            m_fling_last_time = Clock::now();
        } else if (m_gesture == Gesture::TwoFinger) {
            send_button(MouseButton::Middle, false, m_two_finger_center);
        }
        m_gesture = Gesture::None;
        // Move the "cursor" away so that nothing stays hovered after the finger is lifted.
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        break;

    case TouchAction::Cancel:
        break;
    }
}

void MobileRenderCanvas::on_mouse_button(int button, bool down, float x, float y)
{
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    }
    const MouseButton b = button == 1 ? MouseButton::Right : button == 2 ? MouseButton::Middle : MouseButton::Left;
    send_button(b, down, to_logical(x, y));
}

void MobileRenderCanvas::on_mouse_move(float x, float y)
{
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    }
    send_move(to_logical(x, y));
}

void MobileRenderCanvas::on_mouse_wheel(float x, float y, float delta_x, float delta_y)
{
    send_wheel(to_logical(x, y), delta_x * 120.f, delta_y * 120.f);
}

void MobileRenderCanvas::on_text(const std::string& utf8)
{
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().AddInputCharactersUTF8(utf8.c_str());
    }
    AbstractRenderCanvas::request_render();
}

void MobileRenderCanvas::on_key(int android_key_code, bool down)
{
    for (const AndroidKey& key : android_keys) {
        if (key.code != android_key_code) {
            continue;
        }
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::GetIO().AddKeyEvent(key.imgui, down);
        }
        const auto type = down ? KeyboardEvent::Type::KeyDown : KeyboardEvent::Type::KeyUp;
        update_key_modifiers(type, key.slic3r);
        enqueue_keyboard(KeyboardEvent{type, key.slic3r, m_key_modifiers, false});
        break;
    }
    AbstractRenderCanvas::request_render();
}

} // namespace Slic3r::App::Mobile
