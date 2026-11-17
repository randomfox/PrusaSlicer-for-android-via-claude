#include "MobileServices.hpp"
#include "MobilePresetDialogs.hpp"

#include "Slic3r/Biz/Emboss/Emboss.hpp"
#include "Slic3r/Biz/Preset/PresetInteractor.hpp"
#include "Slic3r/Biz/Preset/PresetSelectionCheck.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Log.hpp"

#include "Slic3r/Biz/I18N/I18N.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <sstream>
#include <fmt/ranges.h>
#include <cmath>
#include <cstdlib>

namespace fs = boost::filesystem;

namespace Slic3r::App::Mobile {

namespace {
/// Parses a number typed by the user ("," accepted as the decimal separator).
std::optional<double> parse_number(std::string text)
{
    std::replace(text.begin(), text.end(), ',', '.');
    boost::algorithm::trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    char* end           = nullptr;
    const double result = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}
} // namespace

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

// Modal dialogs: the render thread blocks in IMobileHost::show_form() while the host's UI
// thread shows the form.

std::string MobileDialogManager::show_input_dialog(const std::string& title, const std::string& text, const std::string& default_value)
{
    MobileForm form;
    form.title   = title;
    form.fields  = {{.id = "value", .type = MobileForm::Field::Type::Text, .label = text, .value = default_value}};
    form.buttons = {Biz::_u8L("OK"), Biz::_u8L("Cancel")};
    const MobileFormResult result = m_host.show_form(form);
    // Like wxGetTextFromUser: an empty string when cancelled.
    return result.button == 0 ? result.value("value") : std::string{};
}

void MobileDialogManager::show_input_dialog_with_buttons(
    const std::string& title,
    const std::string& text,
    const std::string& default_value,
    const std::vector<ButtonWithCallback>& buttons
)
{
    MobileForm form;
    form.title  = title;
    form.fields = {{.id = "value", .type = MobileForm::Field::Type::Text, .label = text, .value = default_value}};
    for (const ButtonWithCallback& button : buttons) {
        form.buttons.push_back(button.text);
    }
    const MobileFormResult result = m_host.show_form(form);
    if (result.button >= 0 && result.button < int(buttons.size()) && buttons[result.button].callback) {
        buttons[result.button].callback(result.value("value", default_value));
    }
}

std::string MobileDialogManager::show_combo_dialog(const std::string& title, const std::string& text, const std::vector<std::string>& values)
{
    if (values.empty()) {
        return {};
    }
    MobileForm form;
    form.title   = title;
    form.fields  = {{.id = "value", .type = MobileForm::Field::Type::Choice, .label = text, .value = values.front(), .options = values}};
    form.buttons = {Biz::_u8L("OK"), Biz::_u8L("Cancel")};
    const MobileFormResult result = m_host.show_form(form);
    return result.button == 0 ? result.value("value") : std::string{};
}

void MobileDialogManager::show_webview_dialog(std::unique_ptr<Browser::AbstractBrowserLogic>&& logic, Biz::ProjectInteractor* project_interactor)
{
    const int id = m_next_request_id++;
    auto session = std::make_unique<MobileWebViewSession>(id, m_host, std::move(logic), project_interactor, nullptr);
    MobileWebViewSession* ptr = session.get();
    m_webviews.emplace(id, std::move(session));
    ptr->open();
}

void MobileDialogManager::show_upload_webview_dialog(
    std::unique_ptr<Browser::AbstractUploadBrowserLogic>&& logic, Biz::ProjectInteractor* project_interactor, const UploadCallback& callback
)
{
    // Unlike on the desktop the dialog is not modal: the callback runs when the web view closes.
    Browser::AbstractUploadBrowserLogic* upload_logic = logic.get();
    const int id = m_next_request_id++;
    auto session = std::make_unique<MobileWebViewSession>(id, m_host, std::move(logic), project_interactor,
        [upload_logic, callback](bool) {
            if (callback) {
                callback(upload_logic->success(), upload_logic->result_data());
            }
        });
    MobileWebViewSession* ptr = session.get();
    m_webviews.emplace(id, std::move(session));
    ptr->open();
}

MobileWebViewSession* MobileDialogManager::webview(int id)
{
    auto it = m_webviews.find(id);
    return it == m_webviews.end() || it->second->is_closed() ? nullptr : it->second.get();
}

void MobileDialogManager::collect_closed_webviews()
{
    std::erase_if(m_webviews, [](const auto& item) { return item.second->is_closed(); });
}

void MobileDialogManager::show_yesno_dialog(const std::string& title, const std::string& text, const YesNoCallback& callback)
{
    ask(title, text, {Biz::_u8L("Yes"), Biz::_u8L("No")}, [callback](int index) {
        if (callback) {
            callback(index == 0);
        }
    });
}

void MobileDialogManager::show_rich_yesno_dialog(
    const std::string& title,
    const std::string& text,
    const std::string& check_text,
    const YesNoCallback& callback,
    const CheckBoxCheckedCallback& checked_callback
)
{
    MobileForm form;
    form.title   = title;
    form.text    = text;
    form.fields  = {{.id = "check", .type = MobileForm::Field::Type::Check, .label = check_text, .value = "0"}};
    form.buttons = {Biz::_u8L("Yes"), Biz::_u8L("No")};
    const MobileFormResult result = m_host.show_form(form);
    if (checked_callback) {
        checked_callback(result.button >= 0 && result.value("check") == "1");
    }
    if (callback) {
        callback(result.button == 0);
    }
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

void MobileDialogManager::show_diff_dialog(const Biz::Preset::PresetInteractor& preset_interactor, std::optional<Domain::Preset::PresetKind> kind)
{
    using Domain::Preset::PresetKind;
    // Compares two print or two material presets of the selected printer (the desktop dialog
    // can also compare printers and presets of other printers).
    const auto& selected = preset_interactor.selected_printer_preset();
    const bool materials = kind == PresetKind::FdmMaterial || kind == PresetKind::SlaMaterial;

    struct Entry
    {
        std::string name;
        const Domain::ConfigBox* box;
    };
    std::vector<Entry> entries;
    auto add = [&](const auto& preset, bool is_runtime) {
        const Domain::ConfigBox* box = std::visit([](const auto& values) -> const Domain::ConfigBox* { return &values; }, preset.values);
        entries.push_back({(is_runtime ? Biz::_u8L("(From 3mf) ") : std::string{}) + preset.name, box});
    };
    std::string current;
    if (materials) {
        for (const auto [ref, is_runtime] :
             preset_interactor.get_material_presets(selected.hw_config.id, selected.printer.id, selected.print.id, 0)) {
            add(ref.get(), is_runtime);
        }
        current = selected.materials.empty() ? std::string{} : selected.materials.front().name;
    } else {
        for (const auto [ref, is_runtime] : preset_interactor.get_print_presets(selected.hw_config.id, selected.printer.id)) {
            add(ref.get(), is_runtime);
        }
        current = selected.print.name;
    }
    if (entries.size() < 2) {
        m_host.show_message(IMobileHost::MessageKind::Info, Biz::_u8L("Compare presets"), Biz::_u8L("There is nothing to compare with."));
        return;
    }
    std::vector<std::string> names;
    size_t current_index = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        names.push_back(entries[i].name);
        if (entries[i].name == current) {
            current_index = i;
        }
    }
    const Domain::PrinterTechnology technology = selected.technology();

    MobileForm pick;
    pick.title   = materials ? Biz::_u8L("Compare material presets") : Biz::_u8L("Compare print presets");
    pick.fields  = {
        {.id = "left", .type = MobileForm::Field::Type::Choice, .label = Biz::_u8L("Preset"), .value = names[current_index], .options = names},
        {.id = "right", .type = MobileForm::Field::Type::Choice, .label = Biz::_u8L("Compare with"),
         .value = names[(current_index + 1) % names.size()], .options = names},
    };
    pick.buttons = {Biz::_u8L("Compare"), Biz::_u8L("Close")};
    for (;;) {
        const MobileFormResult chosen = m_host.show_form(pick);
        if (chosen.button != 0) {
            return;
        }
        pick.fields[0].value = chosen.value("left", pick.fields[0].value);
        pick.fields[1].value = chosen.value("right", pick.fields[1].value);
        const auto find = [&](const std::string& name) {
            const auto it = std::find(names.begin(), names.end(), name);
            return entries[it == names.end() ? 0 : size_t(it - names.begin())];
        };
        const Entry left = find(pick.fields[0].value), right = find(pick.fields[1].value);

        std::vector<std::string> keys = left.box->diff_keys(*right.box);
        Biz::Preset::PresetSelectionCheck::filter_diff_keys(*left.box, keys);
        std::string text;
        std::vector<std::string> seen;
        for (const std::string& key : keys) {
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                continue;
            }
            seen.push_back(key);
            text += fmt::format("{}\n    {}  |  {}\n", option_display_name(*left.box, key, technology), display_value(left.box, key),
                                display_value(right.box, key));
        }
        MobileForm result;
        result.title   = pick.title;
        result.text    = fmt::format("\"{}\"  |  \"{}\"", left.name, right.name);
        result.fields  = {{.id = "diff", .type = MobileForm::Field::Type::Info, .label = {},
                           .value = seen.empty() ? Biz::_u8L("Presets are the same.") : text}};
        result.buttons = {Biz::_u8L("Compare others"), Biz::_u8L("Close")};
        if (m_host.show_form(result).button != 0) {
            return;
        }
    }
}

std::string MobileDialogManager::show_ramming_dialog(const std::string& ramming_parameters)
{
    // "<line width %> <line spacing %> <speed every 0.25 s> ...| <chart points x y> ...".
    // The slicer only reads the part before '|'; the points only drive the desktop chart.
    std::istringstream in(ramming_parameters.substr(0, ramming_parameters.find('|')));
    int width = 100, spacing = 100;
    in >> width >> spacing;
    std::vector<std::string> speeds;
    for (std::string speed; in >> speed;) {
        speeds.push_back(speed);
    }

    MobileForm form;
    form.title = Biz::_u8L("Ramming customization");
    form.text  = Biz::_u8L(
        "Ramming denotes the rapid extrusion just before a tool change in a single-extruder MM printer. "
        "Speeds are volumetric flow rates (mm³/s), one for every 0.25 s of ramming.");
    form.fields = {
        {.id = "width", .type = MobileForm::Field::Type::Number, .label = Biz::_u8L("Ramming line width (%)"), .value = std::to_string(width)},
        {.id = "spacing", .type = MobileForm::Field::Type::Number, .label = Biz::_u8L("Ramming line spacing (%)"), .value = std::to_string(spacing)},
        {.id = "speeds", .type = MobileForm::Field::Type::Text, .label = Biz::_u8L("Speeds (mm³/s), separated by spaces"),
         .value = fmt::format("{}", fmt::join(speeds, " "))},
    };
    form.buttons = {Biz::_u8L("OK"), Biz::_u8L("Cancel")};

    for (;;) {
        const MobileFormResult result = m_host.show_form(form);
        if (result.button != 0) {
            return ramming_parameters;
        }
        for (MobileForm::Field& field : form.fields) {
            field.value = result.value(field.id, field.value);
        }
        const std::optional<double> new_width   = parse_number(result.value("width"));
        const std::optional<double> new_spacing = parse_number(result.value("spacing"));
        std::vector<double> new_speeds;
        bool speeds_ok = true;
        std::istringstream speeds_in(result.value("speeds"));
        for (std::string item; speeds_in >> item;) {
            const std::optional<double> speed = parse_number(item);
            speeds_ok = speeds_ok && speed && *speed >= 0. && *speed <= 60.;
            if (speed) {
                new_speeds.push_back(*speed);
            }
        }
        if (!new_width || !new_spacing || *new_width < 10 || *new_width > 300 || *new_spacing < 10 || *new_spacing > 300) {
            form.error = Biz::_u8L("Line width and spacing must be between 10 and 300 %.");
        } else if (!speeds_ok || new_speeds.size() > 20) {
            form.error = Biz::_u8L("Enter up to 20 speeds between 0 and 60 mm³/s.");
        } else {
            std::string out = fmt::format("{} {}", int(std::lround(*new_width)), int(std::lround(*new_spacing)));
            for (double speed : new_speeds) {
                out += fmt::format(" {:g}", speed);
            }
            // Chart points through the speeds (the desktop editor draws a spline through them).
            out += "|";
            for (size_t i = 0; i < new_speeds.size(); ++i) {
                out += fmt::format(" {:g} {:g}", 0.25 * double(i) + 0.125, new_speeds[i]);
            }
            return out;
        }
    }
}

std::optional<WipingDialogResult> MobileDialogManager::show_wiping_dialog(
    const std::vector<double>& matrix,
    const std::vector<Domain::ColorRGB>&,
    double printer_purging_volume,
    const std::vector<double>& filament_purging_multipliers,
    bool use_custom_matrix
)
{
    const size_t n = size_t(std::lround(std::sqrt(double(matrix.size()))));
    if (n * n != matrix.size() || n < 2) {
        return std::nullopt;
    }
    std::vector<double> values = matrix;

    MobileForm form;
    form.title   = Biz::_u8L("Wiping volumes");
    form.text    = Biz::_u8L("Volume to purge (mm³) when the filament is changed, from one extruder to another.");
    form.buttons = {Biz::_u8L("OK"), Biz::_u8L("Cancel"), Biz::_u8L("From configuration")};
    auto rebuild = [&](bool custom) {
        form.fields = {{.id = "custom", .type = MobileForm::Field::Type::Check, .label = Biz::_u8L("Use custom project-specific settings"),
                        .value = custom ? "1" : "0"}};
        for (size_t from = 0; from < n; ++from) {
            for (size_t to = 0; to < n; ++to) {
                if (from != to) {
                    form.fields.push_back({.id = fmt::format("{}_{}", from, to), .type = MobileForm::Field::Type::Number,
                                           .label = fmt::format(fmt::runtime(Biz::_u8L("Extruder {} → {}")), from + 1, to + 1),
                                           .value = std::to_string(int(std::lround(values[n * from + to])))});
                }
            }
        }
    };
    rebuild(use_custom_matrix);

    for (;;) {
        const MobileFormResult result = m_host.show_form(form);
        if (result.button == 2) {
            // "Set values from configuration" (WipingDialog).
            for (size_t from = 0; from < n; ++from) {
                for (size_t to = 0; to < n; ++to) {
                    const double multiplier = to < filament_purging_multipliers.size() ? filament_purging_multipliers[to] : 100.;
                    values[n * from + to]   = from == to ? 0. : double(int(printer_purging_volume * multiplier / 100.));
                }
            }
            rebuild(result.value("custom") == "1");
            continue;
        }
        if (result.button != 0) {
            return std::nullopt;
        }
        WipingDialogResult out;
        out.use_custom_matrix = result.value("custom") == "1";
        out.matrix.assign(n * n, 0.);
        bool ok = true;
        for (size_t from = 0; from < n; ++from) {
            for (size_t to = 0; to < n; ++to) {
                if (from == to) {
                    continue;
                }
                const std::optional<double> value = parse_number(result.value(fmt::format("{}_{}", from, to)));
                ok = ok && value && *value >= 0.;
                out.matrix[n * from + to] = value.value_or(0.);
            }
        }
        if (ok) {
            return out;
        }
        values = out.matrix;
        rebuild(out.use_custom_matrix);
        form.error = Biz::_u8L("Enter non-negative numbers.");
    }
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
    const std::string& dialog_name,
    const Domain::ConfigPack& config_original,
    const Domain::ConfigPack& config_selected,
    Domain::ConfigPack* config_new_selected,
    const Biz::Preset::PresetSelectionNames& preset_names,
    const Biz::Preset::PresetSelectionNames& preset_names_new,
    const Biz::Preset::PresetInteractor& preset_interactor,
    bool new_printer_has_multiple_extruders
)
{
    // (An empty result means "cancelled": the preset is not switched.)
    return ask_unsaved_changes(
        m_host, dialog_name, config_original, config_selected, config_new_selected, preset_names, preset_names_new,
        preset_interactor, new_printer_has_multiple_extruders
    );
}

std::optional<Biz::StepLoadDialogResult> MobileDialogManager::show_load_step_dialog(
    const std::string& filename, double linear_precision, double angle_precision, bool multiple
)
{
    MobileForm form;
    form.title = Biz::_u8L("STEP file import parameters");
    form.text  = fs::path(filename).filename().string();
    form.fields = {
        {.id = "linear", .type = MobileForm::Field::Type::Number, .label = Biz::_u8L("Linear precision (mm)"),
         .value = fmt::format("{:g}", linear_precision)},
        {.id = "angle", .type = MobileForm::Field::Type::Number, .label = Biz::_u8L("Angle precision (°)"),
         .value = fmt::format("{:g}", angle_precision)},
        {.id = "dont_show", .type = MobileForm::Field::Type::Check, .label = Biz::_u8L("Don't show again"), .value = "0"},
    };
    if (multiple) {
        form.fields.push_back({.id = "all", .type = MobileForm::Field::Type::Check, .label = Biz::_u8L("Apply to all"), .value = "1"});
    }
    form.buttons = {Biz::_u8L("OK"), Biz::_u8L("Cancel")};

    for (;;) {
        const MobileFormResult result = m_host.show_form(form);
        if (result.button != 0) {
            return std::nullopt;
        }
        const std::optional<double> linear = parse_number(result.value("linear"));
        const std::optional<double> angle  = parse_number(result.value("angle"));
        if (linear && angle && *linear > 0. && *angle > 0.) {
            return Biz::StepLoadDialogResult{
                .do_not_show_again = result.value("dont_show") == "1",
                .linear_precision  = *linear,
                .angle_precision   = *angle,
                .apply_to_all      = result.value("all", "0") == "1",
            };
        }
        // Ask again, keeping what was typed.
        for (MobileForm::Field& field : form.fields) {
            field.value = result.value(field.id, field.value);
        }
        form.error = Biz::_u8L("Enter positive numbers.");
    }
}

std::string MobileDialogManager::show_save_dialog(
    Domain::Preset::PresetKind kind, const std::string& original_name, const Biz::Preset::PresetInteractor& preset_interactor
)
{
    return ask_preset_name(m_host, kind, original_name, preset_interactor);
}

MobileDialogManager::NamesPerKindMap MobileDialogManager::show_save_print_tool_dialog(
    const NamesPerKindMap& original_names_per_kind, const Biz::Preset::PresetInteractor& preset_interactor
)
{
    return ask_print_tool_preset_names(m_host, original_names_per_kind, preset_interactor);
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
