#pragma once

#include "MobileHost.hpp"

#include "Slic3r/App/Platform/AbstractRenderCanvas.hpp"
#include "Slic3r/App/Platform/StdMainThreadDispatcher.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace Slic3r::App::Mobile {

/// Main thread dispatcher of the mobile app. The render thread is the "main thread": queued
/// functions run at the start of every frame, and queuing one asks the host for a frame.
class MobileMainThreadDispatcher final : public Platform::StdMainThreadDispatcher
{
public:
    explicit MobileMainThreadDispatcher(std::function<void()> wake) : m_wake(std::move(wake)) {}

protected:
    void on_queue_insert() override
    {
        if (m_wake) {
            m_wake();
        }
    }

private:
    std::function<void()> m_wake;
};

/// Render canvas drawing into the OpenGL ES context current on the calling thread (owned by
/// the host: GLSurfaceView on Android, an EGL pbuffer in the headless test). Translates touch
/// input into the mouse events PrusaSlicer's render modules understand.
class MobileRenderCanvas final : public Platform::AbstractRenderCanvas
{
public:
    enum class TouchAction { Down, Move, Up, Cancel };

    explicit MobileRenderCanvas(IMobileHost& host);
    ~MobileRenderCanvas() override;

    /// Initializes rendering resources. Requires a current OpenGL ES 3.2 context.
    void init_gl();
    bool is_gl_initialized() const { return m_gl_initialized; }

    /// Drawable size in physical pixels and the UI scale (physical pixels per UI pixel).
    void set_surface(int width_px, int height_px, float ui_scale);

    /// Renders one frame (dispatches queued work first). Returns true if another frame is wanted.
    bool render_frame();

    /// Touch input: pointer positions in physical pixels. `pointer_count` includes the pointer
    /// going up for TouchAction::Up.
    void on_touch(TouchAction action, int pointer_count, const float* xs, const float* ys);

    /// Mouse input (physical mouse or emulator), positions in physical pixels.
    void on_mouse_button(int button, bool down, float x, float y);
    void on_mouse_move(float x, float y);
    void on_mouse_wheel(float x, float y, float delta_x, float delta_y);

    /// Text typed on the soft keyboard (UTF-8) and special keys (Android KEYCODE_* values).
    void on_text(const std::string& utf8);
    void on_key(int android_key_code, bool down);

protected:
    bool begin_frame_platform() override;
    void begin_imgui_frame_platform() override {}
    void end_imgui_frame_platform() override;
    void end_frame_platform() override;
    double platform_time() override;
    Render::Device& device() override;
    void on_render_requested() override;

private:
    using Clock = std::chrono::steady_clock;

    struct Point { float x; float y; };

    Point to_logical(float x, float y) const { return {x / m_ui_scale, y / m_ui_scale}; }
    void send_button(Platform::MouseButton button, bool down, Point p);
    void send_move(Point p);
    void set_pointer(Point p);
    void send_wheel(Point p, float delta_x, float delta_y);
    void update_long_press();
    /// Starts scrolling the ImGui window under the finger, if it can scroll in the drag direction.
    bool begin_touch_scroll(Point p);
    void update_touch_scroll(Point p);
    void update_fling();

    IMobileHost& m_host;
    bool m_gl_initialized{false};
    int m_width_px{1};
    int m_height_px{1};
    float m_ui_scale{1.f};
    Clock::time_point m_start_time{Clock::now()};
    bool m_text_input_active{false};

    // Touch gesture state.
    enum class Gesture { None, Pending, Drag, Scroll, TwoFinger, Consumed };
    Gesture m_gesture{Gesture::None};
    Point m_touch_start{};
    Point m_touch_last{};
    Clock::time_point m_touch_start_time{};
    Point m_two_finger_center{};
    float m_two_finger_distance{0.f};

    // Drag to scroll panels (Gesture::Scroll) and the fling after it.
    unsigned m_scroll_window_id{0};
    bool m_scroll_vertical{true};
    float m_scroll_start_value{0.f};
    float m_scroll_velocity{0.f}; // logical px per second, in scroll direction
    Point m_scroll_last_point{};
    Clock::time_point m_scroll_last_time{};
    Clock::time_point m_fling_last_time{};
};

} // namespace Slic3r::App::Mobile
