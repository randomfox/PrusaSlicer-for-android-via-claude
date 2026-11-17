#include "MobileServices.hpp"

#include "Slic3r/Biz/Emboss/Emboss.hpp"
#include "Slic3r/Biz/Preset/PresetInteractor.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Log.hpp"

#include <boost/filesystem/operations.hpp>

namespace fs = boost::filesystem;

namespace Slic3r::App::Mobile {

// ---- MobileDialogManager --------------------------------------------------------------------

MobileDialogManager::MobileDialogManager(IMobileHost& host, fs::path output_dir) :
    m_host(host),
    m_output_dir(std::move(output_dir))
{
    boost::system::error_code ec;
    fs::create_directories(m_output_dir, ec);
}

int MobileDialogManager::ask(
    const std::string& title,
    const std::string& text,
    std::vector<std::string> buttons,
    std::function<void(int)> on_answer
)
{
    const int id = m_next_request_id++;
    m_pending_questions.emplace(id, std::move(on_answer));
    m_host.ask_question(id, title, text, buttons);
    return id;
}

void MobileDialogManager::on_question_answered(int request_id, int button_index)
{
    auto it = m_pending_questions.find(request_id);
    if (it == m_pending_questions.end()) {
        return;
    }
    auto callback = std::move(it->second);
    m_pending_questions.erase(it);
    if (callback) {
        callback(button_index);
    }
}

void MobileDialogManager::on_files_picked(int request_id, const std::vector<std::string>& paths)
{
    auto it = m_pending_file_requests.find(request_id);
    if (it == m_pending_file_requests.end()) {
        return;
    }
    auto callback = std::move(it->second);
    m_pending_file_requests.erase(it);
    std::vector<fs::path> file_paths(paths.begin(), paths.end());
    if (callback) {
        callback(!file_paths.empty(), file_paths);
    }
}

void MobileDialogManager::show_file_dialog(
    FileDialogType dialog_type,
    const std::string& dialog_title,
    const fs::path& /*default_folder*/,
    const std::string& default_file_name,
    const std::string& wildcards,
    const FileCallback& callback
)
{
    if (dialog_type == FileDialogType::Save) {
        // Android has no shared file system to choose a location in: the file is written into
        // the output directory and the host lets the user save or share it once it exists.
        const std::string name = default_file_name.empty() ? std::string{"output"} : fs::path(default_file_name).filename().string();
        const fs::path target  = m_output_dir / name;
        SPDLOG_INFO("Save dialog \"{}\" -> {}", dialog_title, target.string());
        if (callback) {
            callback(true, {target});
        }
        return;
    }

    const int id = m_next_request_id++;
    m_pending_file_requests.emplace(id, callback);
    m_host.pick_files(
        id,
        dialog_type == FileDialogType::OpenMultiple ? IMobileHost::FileRequestKind::OpenMultiple :
                                                      IMobileHost::FileRequestKind::Open,
        dialog_title,
        wildcards
    );
}

std::string MobileDialogManager::show_input_dialog(const std::string& title, const std::string&, const std::string& default_value)
{
    // Synchronous dialogs cannot block the render thread on Android; keep the default.
    SPDLOG_WARN("Input dialog \"{}\" not supported on mobile, using default value", title);
    return default_value;
}

void MobileDialogManager::show_input_dialog_with_buttons(
    const std::string& title,
    const std::string& text,
    const std::string& default_value,
    const std::vector<ButtonWithCallback>& buttons
)
{
    std::vector<std::string> labels;
    for (const ButtonWithCallback& button : buttons) {
        labels.push_back(button.text);
    }
    ask(title, text, labels, [buttons, default_value](int index) {
        if (index >= 0 && index < int(buttons.size()) && buttons[index].callback) {
            buttons[index].callback(default_value);
        }
    });
}

std::string MobileDialogManager::show_combo_dialog(const std::string& title, const std::string&, const std::vector<std::string>& values)
{
    SPDLOG_WARN("Combo dialog \"{}\" not supported on mobile", title);
    return values.empty() ? std::string{} : values.front();
}

void MobileDialogManager::show_webview_dialog(std::unique_ptr<Browser::AbstractBrowserLogic>&&, Biz::ProjectInteractor*)
{
    m_host.show_message(IMobileHost::MessageKind::Info, "PrusaSlicer", "Web based dialogs (Printables, Prusa Connect) are not available in this version yet.");
}

void MobileDialogManager::show_upload_webview_dialog(
    std::unique_ptr<Browser::AbstractUploadBrowserLogic>&&, Biz::ProjectInteractor*, const UploadCallback& callback
)
{
    show_webview_dialog(nullptr, nullptr);
    if (callback) {
        callback(false, {});
    }
}

void MobileDialogManager::show_yesno_dialog(const std::string& title, const std::string& text, const YesNoCallback& callback)
{
    ask(title, text, {"Yes", "No"}, [callback](int index) {
        if (callback) {
            callback(index == 0);
        }
    });
}

void MobileDialogManager::show_rich_yesno_dialog(
    const std::string& title,
    const std::string& text,
    const std::string& /*check_text*/,
    const YesNoCallback& callback,
    const CheckBoxCheckedCallback& checked_callback
)
{
    if (checked_callback) {
        checked_callback(false);
    }
    show_yesno_dialog(title, text, callback);
}

void MobileDialogManager::show_yesnocancel_dialog(
    const std::string& title, const std::string& text, const Button& yes, const Button& no, const Button& cancel
)
{
    ask(title, text, {yes.first, no.first, cancel.first}, [yes, no, cancel](int index) {
        const Button& chosen = index == 0 ? yes : index == 1 ? no : cancel;
        if (chosen.second) {
            chosen.second();
        }
    });
}

void MobileDialogManager::show_diff_dialog(const Biz::Preset::PresetInteractor&, std::optional<Domain::Preset::PresetKind>)
{
    m_host.show_message(IMobileHost::MessageKind::Info, "Compare presets", "Comparing presets is not available in this version yet.");
}

std::string MobileDialogManager::show_ramming_dialog(const std::string& ramming_parameters)
{
    return ramming_parameters;
}

std::optional<WipingDialogResult> MobileDialogManager::show_wiping_dialog(
    const std::vector<double>&, const std::vector<Domain::ColorRGB>&, double, const std::vector<double>&, bool
)
{
    return std::nullopt;
}

void MobileDialogManager::open_in_browser(const std::string& link, int)
{
    m_host.open_url(link);
}

void MobileDialogManager::show_info_dialog(const std::string& text, const std::string& title, bool)
{
    m_host.show_message(IMobileHost::MessageKind::Info, title, text);
}

void MobileDialogManager::show_warning_dialog(const std::string& text, const std::string& title)
{
    m_host.show_message(IMobileHost::MessageKind::Warning, title, text);
}

void MobileDialogManager::show_error_dialog(const std::string& text, const std::string& title)
{
    m_host.show_message(IMobileHost::MessageKind::Error, title, text);
}

MobileDialogManager::PresetsSwitchStates MobileDialogManager::show_unsaved_changes_dialog(
    const std::string&,
    const Domain::ConfigPack&,
    const Domain::ConfigPack&,
    Domain::ConfigPack*,
    const Biz::Preset::PresetSelectionNames&,
    const Biz::Preset::PresetSelectionNames&,
    const Biz::Preset::PresetInteractor&,
    bool
)
{
    // An empty result keeps the default behaviour (the modified values are carried over).
    return {};
}

std::optional<Biz::StepLoadDialogResult> MobileDialogManager::show_load_step_dialog(const std::string&, double, double, bool)
{
    return std::nullopt;
}

std::string MobileDialogManager::show_save_dialog(
    Domain::Preset::PresetKind, const std::string& original_name, const Biz::Preset::PresetInteractor&
)
{
    SPDLOG_WARN("Saving presets under a new name is not supported on mobile yet ({})", original_name);
    return {};
}

MobileDialogManager::NamesPerKindMap MobileDialogManager::show_save_print_tool_dialog(
    const NamesPerKindMap&, const Biz::Preset::PresetInteractor&
)
{
    return {};
}

// ---- MobileFileExplorerHandler ---------------------------------------------------------------

void MobileFileExplorerHandler::open_folder(const fs::path& path)
{
    SPDLOG_INFO("open_folder({}) is not supported on mobile", path.string());
}

void MobileFileExplorerHandler::open_datadir_folder()
{
    SPDLOG_INFO("open_datadir_folder() is not supported on mobile");
}

// ---- MobileFontManager ----------------------------------------------------------------------

namespace {
void add_fonts_from(Domain::FontList& fonts, const fs::path& dir)
{
    boost::system::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return;
    }
    std::vector<fs::path> files;
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
        const std::string ext = it->path().extension().string();
        if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        fonts.push_back(Domain::FontDescriptor{
            .name = file.stem().string(),
            .path = file.string(),
            .type = Domain::FontDescriptor::Type::file_path,
        });
    }
}
} // namespace

MobileFontManager::MobileFontManager()
{
    add_fonts_from(m_fonts, fs::path(Slic3r::resources_dir()) / "fonts");
    add_fonts_from(m_fonts, "/system/fonts");
}

std::unique_ptr<const Domain::FontFile> MobileFontManager::open(const Domain::FontDescriptor& descriptor)
{
    if (descriptor.type != Domain::FontDescriptor::Type::file_path) {
        return nullptr;
    }
    fs::path path{descriptor.path};
    boost::system::error_code ec;
    if (!fs::exists(path, ec)) {
        // Projects store only the file name, look it up among the known fonts.
        for (const Domain::FontDescriptor& font : m_fonts) {
            if (fs::path(font.path).filename() == path.filename()) {
                path = font.path;
                break;
            }
        }
    }
    return Biz::Emboss::create_font_file(path.string().c_str());
}

Domain::FontList MobileFontManager::create_favorit()
{
    // Mirrors the desktop favorites (NORMAL, SMALL, ITALIC, SWISS, MODERN) with fonts that are
    // always available on mobile: the ones bundled with PrusaSlicer.
    const fs::path fonts_dir = fs::path(Slic3r::resources_dir()) / "fonts";
    const std::pair<const char*, const char*> favorites[] = {
        {"NORMAL", "NotoSans-Regular.ttf"},
        {"SMALL", "Inter-Regular.ttf"},
        {"ITALIC", "NotoSans-Italic.ttf"},
        {"SWISS", "Inter-Bold.ttf"},
        {"MODERN", "NotoSans-Bold.ttf"},
    };
    Domain::FontList result;
    for (const auto& [name, file] : favorites) {
        const fs::path path = fonts_dir / file;
        boost::system::error_code ec;
        if (fs::exists(path, ec)) {
            result.push_back(Domain::FontDescriptor{.name = name, .path = path.string(), .type = Domain::FontDescriptor::Type::file_path});
        }
    }
    if (result.empty() && !m_fonts.empty()) {
        result.push_back(m_fonts.front());
    }
    if (result.empty()) {
        // Callers take the first favorite unconditionally.
        result.push_back(Domain::FontDescriptor{.name = "NORMAL", .path = {}, .type = Domain::FontDescriptor::Type::file_path});
    }
    return result;
}

MobileFontManager::Descriptor MobileFontManager::get_current_descriptor(const Domain::FontDescriptor& other_descriptor)
{
    if (other_descriptor.type == Domain::FontDescriptor::Type::file_path) {
        return other_descriptor;
    }
    return tl::make_unexpected(ConversionError::AnotherOS);
}

} // namespace Slic3r::App::Mobile
