#pragma once

#include "MobileHost.hpp"

#include "Slic3r/App/IDialogManager.hpp"
#include "Slic3r/App/Platform/IFileExplorerHandler.hpp"
#include "Slic3r/Biz/Emboss/IFontManager.hpp"

#include <boost/filesystem/path.hpp>

#include <functional>
#include <map>
#include <string>

namespace Slic3r::App::Mobile {

/// IDialogManager on top of IMobileHost. Native dialogs of the desktop app (file pickers,
/// message boxes) are forwarded to Android; dialogs that only make sense on desktop return
/// their default answer.
class MobileDialogManager final : public IDialogManager
{
public:
    MobileDialogManager(IMobileHost& host, boost::filesystem::path output_dir);

    // Replies from the host.
    void on_question_answered(int request_id, int button_index);
    void on_files_picked(int request_id, const std::vector<std::string>& paths);

    /// Files PrusaSlicer is asked to save go into this directory; the host watches it and
    /// offers each new file to the user (see IMobileHost::offer_output_file).
    const boost::filesystem::path& output_dir() const { return m_output_dir; }

    void show_file_dialog(
        FileDialogType dialog_type,
        const std::string& dialog_title,
        const boost::filesystem::path& default_folder,
        const std::string& default_file_name,
        const std::string& wildcards,
        const FileCallback& callback
    ) override;

    std::string show_input_dialog(const std::string& title, const std::string& text, const std::string& default_value) override;
    void show_input_dialog_with_buttons(
        const std::string& title,
        const std::string& text,
        const std::string& default_value,
        const std::vector<ButtonWithCallback>& buttons
    ) override;
    std::string show_combo_dialog(const std::string& title, const std::string& text, const std::vector<std::string>& values) override;
    void show_webview_dialog(std::unique_ptr<Browser::AbstractBrowserLogic>&& logic, Biz::ProjectInteractor* project_interactor) override;
    void show_upload_webview_dialog(
        std::unique_ptr<Browser::AbstractUploadBrowserLogic>&& logic,
        Biz::ProjectInteractor* project_interactor,
        const UploadCallback& callback
    ) override;
    void show_yesno_dialog(const std::string& title, const std::string& text, const YesNoCallback& callback) override;
    void show_rich_yesno_dialog(
        const std::string& title,
        const std::string& text,
        const std::string& check_text,
        const YesNoCallback& callback,
        const CheckBoxCheckedCallback& checked_callback
    ) override;
    void show_yesnocancel_dialog(const std::string& title, const std::string& text, const Button& yes, const Button& no, const Button& cancel) override;
    void show_diff_dialog(const Biz::Preset::PresetInteractor& preset_interactor, std::optional<Domain::Preset::PresetKind> kind) override;
    std::string show_ramming_dialog(const std::string& ramming_parameters) override;
    std::optional<WipingDialogResult> show_wiping_dialog(
        const std::vector<double>& matrix,
        const std::vector<Domain::ColorRGB>& extruder_colours,
        double printer_purging_volume,
        const std::vector<double>& filament_purging_multipliers,
        bool use_custom_matrix
    ) override;
    void open_in_browser(const std::string& link, int flag) override;
    void show_info_dialog(const std::string& text, const std::string& title, bool is_marked) override;
    void show_warning_dialog(const std::string& text, const std::string& title) override;
    void show_error_dialog(const std::string& text, const std::string& title) override;
    PresetsSwitchStates show_unsaved_changes_dialog(
        const std::string& dialog_name,
        const Domain::ConfigPack& config_original,
        const Domain::ConfigPack& config_selected,
        Domain::ConfigPack* config_new_selected,
        const Biz::Preset::PresetSelectionNames& preset_names,
        const Biz::Preset::PresetSelectionNames& preset_names_new,
        const Biz::Preset::PresetInteractor& preset_interactor,
        bool new_printer_has_multiple_extruders
    ) override;
    std::optional<Biz::StepLoadDialogResult> show_load_step_dialog(
        const std::string& filename, double linear_precision, double angle_precision, bool multiple
    ) override;
    std::string show_save_dialog(
        Domain::Preset::PresetKind kind, const std::string& original_name, const Biz::Preset::PresetInteractor& preset_interactor
    ) override;
    NamesPerKindMap show_save_print_tool_dialog(
        const NamesPerKindMap& original_names_per_kind, const Biz::Preset::PresetInteractor& preset_interactor
    ) override;

private:
    int ask(const std::string& title, const std::string& text, std::vector<std::string> buttons, std::function<void(int)> on_answer);

    IMobileHost& m_host;
    boost::filesystem::path m_output_dir;
    int m_next_request_id{1};
    std::map<int, std::function<void(int)>> m_pending_questions;
    std::map<int, FileCallback> m_pending_file_requests;
};

/// There is no file manager to show folders in on Android.
class MobileFileExplorerHandler final : public Platform::IFileExplorerHandler
{
public:
    void open_folder(const boost::filesystem::path& path) override;
    void open_datadir_folder() override;
};

/// Fonts for the text (emboss) tool: the fonts bundled with PrusaSlicer plus the system fonts.
class MobileFontManager final : public Biz::Emboss::IFontManager
{
public:
    MobileFontManager();

    const Domain::FontList& get_fonts() override { return m_fonts; }
    std::unique_ptr<const Domain::FontFile> open(const Domain::FontDescriptor& descriptor) override;
    Descriptor get_current_descriptor(const Domain::FontDescriptor& other_descriptor) override;
    Domain::FontDescriptor::Type get_current_type() const override { return Domain::FontDescriptor::Type::file_path; }
    /// Default text styles (the desktop uses the system GUI fonts). Must not be empty.
    Domain::FontList create_favorit() override;

private:
    Domain::FontList m_fonts;
};

} // namespace Slic3r::App::Mobile
