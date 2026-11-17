#pragma once

#include <string>
#include <vector>

namespace Slic3r::App::Mobile {

/// Services the native UI needs from the platform hosting it: the Android activity (through
/// JNI), or the headless test harness. All calls are made on the render thread; replies come
/// back through MobileApp methods, also on the render thread.
class IMobileHost
{
public:
    enum class MessageKind { Info, Warning, Error };
    enum class FileRequestKind { Open, OpenMultiple, Save };

    virtual ~IMobileHost() = default;

    /// Asks the host to render another frame soon (MobileApp::render_frame()).
    virtual void request_render() = 0;

    virtual void show_message(MessageKind kind, const std::string& title, const std::string& text) = 0;

    /// Shows a question with 2 or 3 buttons. Answer: MobileApp::on_question_answered(id, index),
    /// index -1 when dismissed.
    virtual void ask_question(
        int request_id,
        const std::string& title,
        const std::string& text,
        const std::vector<std::string>& buttons
    ) = 0;

    /// Lets the user pick files to open. The host copies them into local storage and answers
    /// with MobileApp::on_files_picked(id, local_paths) (empty when cancelled).
    virtual void pick_files(int request_id, FileRequestKind kind, const std::string& title, const std::string& wildcards) = 0;

    /// A file was written by PrusaSlicer for the user (G-code, 3MF, ...), offer to save/share it.
    virtual void offer_output_file(const std::string& path) = 0;

    virtual void open_url(const std::string& url) = 0;

    /// ImGui wants (or no longer wants) text input: show/hide the soft keyboard.
    virtual void set_text_input_active(bool active) = 0;

    // ---- Web views (Prusa Account login, Prusa Connect, Printables) ---------------------
    // PrusaSlicer's browser logic drives an embedded web view: the host shows it and reports
    // its events (MobileApp::on_webview_*), PrusaSlicer answers with commands.

    enum class WebViewCommand {
        LoadUrl,          ///< data = URL
        LoadUrlWithToken, ///< data = URL, extra = access token ("Authorization: External <token>")
        RunScript,        ///< data = JavaScript
        AddUserScript,    ///< data = JavaScript to run at the start of every page
        DeleteCookies,    ///< data = URL whose cookies to delete
        Reload,
        OpenExternal,     ///< data = URL to open in the system browser
        Close,            ///< closes the web view (the session ends)
    };

    /// Opens a web view. `handler_names` are the JavaScript objects with postMessage() the page
    /// uses to talk to PrusaSlicer (reported via MobileApp::on_webview_script_message).
    virtual void open_webview(
        int id, const std::string& title, const std::string& url, const std::vector<std::string>& handler_names
    ) = 0;

    virtual void webview_command(int id, WebViewCommand command, const std::string& data, const std::string& extra) = 0;
};

} // namespace Slic3r::App::Mobile
