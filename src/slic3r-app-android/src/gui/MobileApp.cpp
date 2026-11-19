#include "MobileApp.hpp"
#include "Slic3r/Biz/Utils/ReleaseMemory.hpp"
#include "Slic3r/App/CommandBindingManager.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include "MobileServices.hpp"

#include "Slic3r/App/AppConfig.hpp"
#include "Slic3r/App/AppConfigInteractor.hpp"
#include "Slic3r/App/AppConfigProvider.hpp"
#include "Slic3r/App/AppServices.hpp"
#include "Slic3r/App/LoadingRenderModule.hpp"
#include "Slic3r/App/Localization.hpp"
#include "Slic3r/App/Mouse3D/Mouse3DController.hpp"
#include "Slic3r/App/Navigator.hpp"
#include "Slic3r/App/Plater/PlaterRenderModule.hpp"
#include "Slic3r/Biz/GCodeViewer/GCodeFileLoader.hpp"

#include <algorithm>
#include "Slic3r/App/Plater/ThumbnailImageGenerator.hpp"
#include "Slic3r/App/PopNotification/PopNotificationCenter.hpp"
#include "Slic3r/App/PresetUpdater/PresetUpdaterController.hpp"
#include "Slic3r/App/Preview/PreviewRenderModule.hpp"
#include "Slic3r/App/ProjectSaver.hpp"
#include "Slic3r/App/PrusaLinkStorageListener.hpp"
#include "Slic3r/App/Render/TextureManager.hpp"
#include "Slic3r/App/ResourceResolver.hpp"
#include "Slic3r/App/Theme.hpp"
#include "Slic3r/App/ThumbnailStore.hpp"
#include "Slic3r/App/ThumbnailStoreUpdater.hpp"
#include "Slic3r/App/Undo/Store.hpp"
#include "Slic3r/Biz/Algorithms/StringUtils.hpp"
#include "Slic3r/Biz/AppInstance/AppInstanceMessageHandlerFactory.hpp"
#include "Slic3r/Biz/FileLoadingLogic.hpp"
#include "Slic3r/Biz/FileOpen/FileOpeningInteractor.hpp"
#include "Slic3r/Biz/Platform/JobManager/JobManager.hpp"
#include "Slic3r/Biz/Platform/PlatformServices.hpp"
#include "Slic3r/Biz/ProjectInteractor.hpp"
#include "Slic3r/Biz/Scene/BedGeometry.hpp"
#include "Slic3r/Biz/SecretStoreDummy.hpp"
#include "Slic3r/Biz/UserAccount/UserAccountInteractor.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Log.hpp"

#include <boost/filesystem/exception.hpp>
#include <nlohmann/json.hpp>
#include <boost/filesystem/path.hpp>

namespace fs = boost::filesystem;

namespace Slic3r::App::Mobile {

using Biz::Platform::PlatformServices;
using Biz::Platform::JobManager::JobManager;

MobileApp::MobileApp(IMobileHost& host) : m_host(host) {}

MobileApp::~MobileApp()
{
    AppServices& app_services = AppServices::instance();
    if (app_services.has_preset_updater_controller()) {
        app_services.preset_updater_controller().shutdown();
        app_services.navigator().shutdown();
    }
    if (m_project_interactor) {
        m_project_interactor->file_opening_interactor().set_archive_dialog(nullptr);
    }
}

std::string MobileApp::output_dir() const
{
    return m_dialog_manager != nullptr ? m_dialog_manager->output_dir().string() : std::string{};
}

void MobileApp::on_gl_ready()
{
    if (!m_services_initialized) {
        init_services();
    }
    m_canvas->init_gl();
}

void MobileApp::on_surface_changed(int width_px, int height_px, float ui_scale)
{
    m_canvas->set_surface(width_px, height_px, ui_scale);
}

bool MobileApp::render_frame()
{
    if (!m_canvas || !m_canvas->is_gl_initialized()) {
        return false;
    }
    if (m_init_completed && !m_files_to_open.empty()) {
        std::vector<std::string> files = std::move(m_files_to_open);
        m_files_to_open.clear();
        open_files(files);
    }
    if (m_init_completed && !m_urls_to_open.empty()) {
        std::vector<std::string> urls = std::move(m_urls_to_open);
        m_urls_to_open.clear();
        for (const std::string& url : urls) {
            open_app_url(url);
        }
    }
    bool more = m_canvas->render_frame();
    if (m_dialog_manager != nullptr) {
        m_dialog_manager->collect_closed_webviews();
    }
    // Deferred release of freed memory (closed project, replaced preview); keep drawing until
    // it happened, frames are only drawn on demand.
    Biz::Utils::release_free_memory_after_frame();
    more = more || Biz::Utils::pending_memory_releases().load() > 0;
    return more;
}

bool MobileApp::execute_command(const std::string& name)
{
    // The preview has its own menu (e.g. "Export toolpaths as OBJ"); try it when it is shown.
    for (Platform::AbstractRenderModule* module : {static_cast<Platform::AbstractRenderModule*>(m_plater_module.get()),
                                         static_cast<Platform::AbstractRenderModule*>(m_preview_module.get())}) {
        if (module == nullptr) {
            continue;
        }
        // Menu commands, then the ones bound to keys (e.g. the layers slider's).
        for (const auto* commands :
             {&module->commands(), &module->command_binding_manager().main_command_registry().commands()}) {
            const auto it = commands->find(name);
            if (it != commands->end() && it->second && it->second->enabled()) {
                it->second->execute();
                return true;
            }
        }
    }
    return false;
}

void MobileApp::on_question_answered(int request_id, int button_index)
{
    if (m_dialog_manager != nullptr) {
        m_dialog_manager->on_question_answered(request_id, button_index);
    }
    m_host.request_render();
}

void MobileApp::on_files_picked(int request_id, const std::vector<std::string>& paths)
{
    if (m_dialog_manager != nullptr) {
        m_dialog_manager->on_files_picked(request_id, paths);
    }
    m_host.request_render();
}

void MobileApp::open_app_url(const std::string& url)
{
    if (!m_init_completed) {
        m_urls_to_open.push_back(url);
        return;
    }
    if (url.rfind("prusaslicer://login", 0) == 0) {
        // Same path as a login redirect forwarded from another desktop instance.
        nlohmann::json message;
        message["type"] = "LOGIN";
        message["data"] = Biz::Algorithms::escape_strings_cstyle({url});
        PlatformServices::instance().app_instance_message_handler().handle_message(message.dump());
    } else if (url.rfind("prusaslicer://open", 0) == 0) {
        m_project_interactor->on_download_models({url});
    } else {
        SPDLOG_WARN("Unsupported link: {}", url);
    }
    m_host.request_render();
}

bool MobileApp::on_webview_navigation(int id, const std::string& url, const std::string& current_url)
{
    MobileWebViewSession* session = m_dialog_manager != nullptr ? m_dialog_manager->webview(id) : nullptr;
    const bool allow = session == nullptr || session->on_navigation(url, current_url);
    m_host.request_render();
    return allow;
}

void MobileApp::on_webview_loaded(int id, const std::string& url)
{
    if (MobileWebViewSession* session = m_dialog_manager != nullptr ? m_dialog_manager->webview(id) : nullptr) {
        session->on_loaded(url);
    }
    m_host.request_render();
}

void MobileApp::on_webview_script_message(int id, const std::string& message)
{
    if (MobileWebViewSession* session = m_dialog_manager != nullptr ? m_dialog_manager->webview(id) : nullptr) {
        session->on_script_message(message);
    }
    m_host.request_render();
}

void MobileApp::on_webview_error(int id, const std::string& description)
{
    if (MobileWebViewSession* session = m_dialog_manager != nullptr ? m_dialog_manager->webview(id) : nullptr) {
        session->on_error(description);
    }
}

void MobileApp::on_webview_dismissed(int id)
{
    if (MobileWebViewSession* session = m_dialog_manager != nullptr ? m_dialog_manager->webview(id) : nullptr) {
        session->on_dismissed();
    }
    m_host.request_render();
}

void MobileApp::open_files(const std::vector<std::string>& paths)
{
    if (!m_init_completed) {
        // Opened as soon as the plater exists.
        m_files_to_open.insert(m_files_to_open.end(), paths.begin(), paths.end());
        return;
    }
    std::vector<fs::path> supported;
    const auto is_config_file = [](const std::string& path) { return boost::algorithm::iends_with(path, ".ini"); };
    for (const std::string& path : paths) {
        if (is_config_file(path)) {
            // 2.x config: a new project with its presets and the current objects.
            if (const auto result = m_project_interactor->import_config(path); !result) {
                m_host.show_message(IMobileHost::MessageKind::Error, "PrusaSlicer", result.error());
            }
            continue;
        }
        if (Biz::GCodeViewer::is_gcode_file(path)) {
            // G-code viewer: the file opens in a new project's preview.
            if (const auto error = m_project_interactor->open_gcode_preview(path)) {
                m_host.show_message(IMobileHost::MessageKind::Error, "PrusaSlicer", *error);
            } else {
                AppServices::instance().navigator().navigate_to_module_type(Render::ModuleType::Preview);
            }
            continue;
        }
        if (Biz::FileLoadingLogic::is_supported_file(path) || Biz::FileLoadingLogic::is_archive_file(path)) {
            supported.emplace_back(path);
        } else {
            SPDLOG_WARN("Unsupported file: {}", path);
        }
    }
    if (supported.empty()) {
        if (std::ranges::any_of(paths, [&](const std::string& p) { return !Biz::GCodeViewer::is_gcode_file(p) && !is_config_file(p); })) {
            m_host.show_message(IMobileHost::MessageKind::Warning, "PrusaSlicer", "The file type is not supported.");
        }
        return;
    }
    const bool single_project = supported.size() == 1 && Biz::FileLoadingLogic::is_project_file(supported.front().string());
    m_project_interactor->file_opening_interactor().open(std::move(supported));
    if (!single_project) {
        AppServices::instance().navigator().navigate_to_module_type(Render::ModuleType::Plater);
    }
    m_host.request_render();
}

void MobileApp::init_services()
{
    m_services_initialized = true;

    auto& app_services      = AppServices::instance();
    auto& platform_services = PlatformServices::instance();

    Render::TextureManager::set_resource_resolver(std::make_unique<ResourceResolver>(resources_dir(), data_dir()));
    Biz::Scene::BedGeometry::set_resolver(
        [](const std::string& p) -> std::string { return Render::TextureManager::resource_resolver().resolve(p); }
    );

    // Same as Desktop::DesktopApp::init_translations(): select the configured language, or the
    // best match for the system language (English as the fallback).
    const std::string language = app_services.app_config().get<std::string>("translation_language");
    if (!localization().set_language(language)) {
        SPDLOG_WARN("Switching to language \"{}\" failed", language);
    }
    SPDLOG_INFO("Language: {} ({} available)", localization().active_language(), localization().languages().size());

    const Theme::Style style = app_services.app_config().get<Theme::Style>("theme");
    app_services.set_theme(std::make_unique<Theme>(style));
    app_services.theme().set_transparent_window_alpha(app_services.app_config().get<double>("transparent_window_alpha"));

    platform_services.set_main_thread_dispatcher(
        std::make_unique<MobileMainThreadDispatcher>([this]() { m_host.request_render(); })
    );
    platform_services.set_secret_store(std::make_unique<Biz::SecretStoreDummy>());
    platform_services.set_job_manager(std::make_unique<JobManager>(platform_services.main_thread_dispatcher()));
    platform_services.set_app_config_provider(std::make_unique<AppConfigProvider>());
    platform_services.set_app_instance_message_handler(
        Biz::AppInstance::create_app_instance_message_handler(platform_services.main_thread_dispatcher())
    );

    m_thumbnail_image_generator = std::make_shared<Plater::ThumbnailImageGenerator>();
    m_project_interactor = std::make_unique<Biz::ProjectInteractor>(
        m_workbench, platform_services.main_thread_dispatcher(), *m_thumbnail_image_generator
    );

    auto undo_store_ptr = std::make_unique<Undo::Store>(*m_project_interactor);
    m_undo_store        = undo_store_ptr.get();
    m_project_interactor->set_undo_provider(std::move(undo_store_ptr));

    m_thumbnail_store         = std::make_shared<ThumbnailStore>(*m_project_interactor);
    m_thumbnail_store_updater = std::make_shared<ThumbnailStoreUpdater>(*m_thumbnail_image_generator, m_thumbnail_store);

    auto dialog_manager = std::make_unique<MobileDialogManager>(m_host, fs::path(data_dir()) / "output");
    m_dialog_manager    = dialog_manager.get();
    app_services.set_dialog_manager(std::move(dialog_manager));

    app_services.set_navigator(std::make_unique<Navigator>());
    Navigator& navigator = app_services.navigator();

    app_services.set_pop_notification_center(std::make_unique<PopNotification::PopNotificationCenter>(*m_project_interactor));
    app_services.set_mouse_3d_controller(std::make_unique<Mouse3D::Mouse3DController>(
        app_services.app_config(), app_services.app_config_interactor(), platform_services.main_thread_dispatcher()
    ));
    app_services.set_file_explorer_handler(std::make_unique<MobileFileExplorerHandler>());
    app_services.file_explorer_handler().add_listener<Platform::IFileExplorerErrorListener>(&app_services.pop_notification_center());
    platform_services.job_manager().add_listener<Biz::Platform::JobManager::IJobManagerStatusChangedListener>(
        &app_services.pop_notification_center()
    );
    m_project_interactor->user_account_interactor().add_listener<Biz::UserAccount::IUserAccountListener>(
        &app_services.pop_notification_center()
    );
    m_project_interactor->connect_message_handler().add_listener<Biz::Connect::IConnectHandlerListener>(
        &app_services.pop_notification_center()
    );
    m_project_interactor->physical_printer_interactor().printer_preset_selector().add_listener<Biz::Preset::IPrinterPresetSelectorListener>(
        &app_services.pop_notification_center()
    );

    PresetUpdater::ControllerEnvironment environment;
    environment.timer_queue    = &platform_services.timer_queue();
    environment.online_allowed = []() { return AppServices::instance().app_config().get<bool>("enable_preset_update"); };
    environment.request_render = [this]() { m_host.request_render(); };
    app_services.set_preset_updater_controller(
        std::make_unique<PresetUpdater::PresetUpdaterController>(m_project_interactor->preset_updater_interactor(), std::move(environment))
    );
    PresetUpdater::PresetUpdaterController& preset_updater_controller = app_services.preset_updater_controller();
    preset_updater_controller.set_forced_state_callback([this](bool has_forced) { on_preset_updater_forced_state(has_forced); });
    preset_updater_controller.set_show_dialog_callback([this, &navigator]() {
        if (m_init_completed) {
            navigator.set_modal_dialog(ModalDialog::PresetUpdater);
        } else {
            m_loading_module->set_opened_preset_updater(true);
        }
    });
    preset_updater_controller.set_presets_installed_callback([this]() {
        if (m_init_completed) {
            m_project_interactor->preset_interactor().load_preset_bundle(m_bundle_paths);
            return;
        }
        AppServices::instance().preset_updater_controller().start();
    });

    m_loading_module = std::make_unique<LoadingRenderModule>(preset_updater_controller, navigator);
    m_project_saver  = std::make_shared<ProjectSaver>(*m_project_interactor, *m_thumbnail_store);

    m_canvas = std::make_unique<MobileRenderCanvas>(m_host);
    platform_services.set_render_request_handler(m_canvas.get());
    preset_updater_controller.start();
    navigator.set_canvas(*m_canvas);
    m_canvas->set_render_module(m_loading_module.get());
}

void MobileApp::on_preset_updater_forced_state(bool has_forced)
{
    if (!has_forced) {
        finish_init();
        return;
    }
    if (!m_init_completed) {
        m_loading_module->set_opened_preset_updater(true);
    }
}

void MobileApp::finish_init()
{
    if (m_init_completed) {
        return;
    }
    m_init_completed = true;

    auto& app_services      = AppServices::instance();
    auto& platform_services = PlatformServices::instance();
    auto& preset_interactor = m_project_interactor->preset_interactor();

    preset_interactor.print_tool_cbi().set_app_config_cbol(app_services.app_config_interactor().app_config_cbi().config_box_list());

    m_bundle_paths = Biz::Preset::IO::BundlePaths::make_standard_runtime();
    try {
        preset_interactor.load_preset_bundle(m_bundle_paths);
    } catch (const fs::filesystem_error& error) {
        m_host.show_message(IMobileHost::MessageKind::Error, "PrusaSlicer - Loading presets failed", error.what());
        return;
    }

    m_project_interactor->matdb_interactor().init_client();

    m_plater_module = std::make_unique<Plater::PlaterRenderModule>(
        m_workbench, *m_project_interactor, *m_undo_store, m_thumbnail_store, m_thumbnail_store_updater,
        m_thumbnail_image_generator, std::make_unique<MobileFontManager>(), m_project_saver
    );
    m_preview_module = std::make_unique<Preview::PreviewRenderModule>(
        m_workbench, *m_project_interactor, *m_undo_store, m_thumbnail_store, m_thumbnail_store_updater,
        m_thumbnail_image_generator, m_plater_module.get(), &m_plater_module->plugin_system(), m_project_saver
    );

    m_project_interactor->removable_drive_service().add_status_listener(&app_services.pop_notification_center());
    m_project_interactor->set_dialog_provider(&app_services.dialog_manager());
    preset_interactor.set_dialog_manager(&app_services.dialog_manager());

    m_project_interactor->new_project();

    Navigator& navigator = app_services.navigator();
    navigator.on_init(*m_plater_module, *m_preview_module, *m_canvas, m_project_interactor.get());
    m_project_interactor->file_opening_interactor().set_archive_dialog(
        [&navigator](
            std::vector<Biz::FileOpen::ArchiveFile> files, Biz::FileOpen::OpenFilesOptions options,
            Biz::FileOpen::ArchiveSelectionDone done
        ) { return navigator.confirm_open_files(std::move(files), options, std::move(done)); }
    );

    m_loading_module->set_opened_preset_updater(false);
    m_canvas->set_next_render_module(m_plater_module.get());
    m_canvas->request_render();

    m_project_interactor->user_account_interactor().init(app_services.app_config().is_prusa_account_enabled());

    m_prusalink_storage_listener = std::make_unique<PrintHost::PrusaLinkStorageListener>(*m_project_interactor);
    platform_services.job_manager().add_listener<Biz::Platform::JobManager::IJobManagerStatusChangedListener>(
        m_prusalink_storage_listener.get()
    );

    m_project_interactor->backup_store().start_crash_detection();
    app_services.preset_updater_controller().start_background_check();
    SPDLOG_INFO("Mobile application initialized");
}

} // namespace Slic3r::App::Mobile
