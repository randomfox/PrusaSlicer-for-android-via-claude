#pragma once

#include "MobileHost.hpp"
#include "MobileRenderCanvas.hpp"

#include "Slic3r/Biz/Preset/IO/BundlePaths.hpp"
#include "Slic3r/Domain/Workbench.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Slic3r::Biz {
class ProjectInteractor;
}

namespace Slic3r::App {
class LoadingRenderModule;
class ProjectSaver;
namespace PrintHost {
class PrusaLinkStorageListener;
}
struct ThumbnailStore;
class ThumbnailStoreUpdater;
namespace Plater {
class PlaterRenderModule;
class ThumbnailImageGenerator;
} // namespace Plater
namespace Preview {
class PreviewRenderModule;
}
namespace Undo {
class Store;
}
} // namespace Slic3r::App

namespace Slic3r::App::Mobile {

class MobileDialogManager;

/// The PrusaSlicer application (plater + preview) hosted by a mobile platform. Mirrors
/// Desktop::DesktopApp without wxWidgets: the host provides an OpenGL ES context and native
/// dialogs (IMobileHost), everything else is drawn by PrusaSlicer itself.
///
/// All methods must be called on the render thread, which is the application's main thread.
/// Requires Slic3r::App::Android::initialize() (paths, logging, app config) to have run.
class MobileApp
{
public:
    explicit MobileApp(IMobileHost& host);
    ~MobileApp();

    MobileApp(const MobileApp&)            = delete;
    MobileApp& operator=(const MobileApp&) = delete;

    /// Called when the GL context exists: initializes the application on first use.
    void on_gl_ready();

    void on_surface_changed(int width_px, int height_px, float ui_scale);

    /// Renders a frame. Returns true when another frame is wanted soon.
    bool render_frame();

    MobileRenderCanvas& canvas() { return *m_canvas; }

    /// Opens model/project files (already copied to local storage by the host).
    void open_files(const std::vector<std::string>& paths);

    /// prusaslicer:// links (Prusa Account login redirect, "Open in PrusaSlicer" downloads).
    void open_app_url(const std::string& url);

    // Web view events (see IMobileHost::open_webview).
    bool on_webview_navigation(int id, const std::string& url, const std::string& current_url);
    void on_webview_loaded(int id, const std::string& url);
    void on_webview_script_message(int id, const std::string& message);
    void on_webview_error(int id, const std::string& description);
    void on_webview_dismissed(int id);

    // Replies to IMobileHost requests.
    void on_question_answered(int request_id, int button_index);
    void on_files_picked(int request_id, const std::vector<std::string>& paths);

    /// Directory where files saved by PrusaSlicer for the user end up.
    std::string output_dir() const;

    bool is_ready() const { return m_init_completed; }

private:
    void init_services();
    void on_preset_updater_forced_state(bool has_forced);
    void finish_init();

    IMobileHost& m_host;
    bool m_services_initialized{false};
    bool m_init_completed{false};

    Domain::Workbench m_workbench;
    std::unique_ptr<MobileRenderCanvas> m_canvas;
    MobileDialogManager* m_dialog_manager{nullptr}; // owned by AppServices

    std::shared_ptr<Plater::ThumbnailImageGenerator> m_thumbnail_image_generator;
    std::unique_ptr<Biz::ProjectInteractor> m_project_interactor;
    Undo::Store* m_undo_store{nullptr};
    std::shared_ptr<ThumbnailStore> m_thumbnail_store;
    std::shared_ptr<ThumbnailStoreUpdater> m_thumbnail_store_updater;
    std::shared_ptr<ProjectSaver> m_project_saver;
    std::unique_ptr<LoadingRenderModule> m_loading_module;
    std::unique_ptr<Plater::PlaterRenderModule> m_plater_module;
    std::unique_ptr<Preview::PreviewRenderModule> m_preview_module;
    std::unique_ptr<PrintHost::PrusaLinkStorageListener> m_prusalink_storage_listener;
    Biz::Preset::IO::BundlePaths m_bundle_paths;
    std::vector<std::string> m_files_to_open;
    std::vector<std::string> m_urls_to_open;
};

} // namespace Slic3r::App::Mobile
