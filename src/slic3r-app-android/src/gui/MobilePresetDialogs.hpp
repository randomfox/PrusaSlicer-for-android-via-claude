#pragma once

// Mobile versions of the desktop preset dialogs (UnsavedChangesDialog, SavePresetDialog),
// built from MobileForms. They block the calling (render) thread like the desktop's modal
// dialogs, and return the same results.

#include "MobileHost.hpp"

#include "Slic3r/Biz/Preset/IPresetDialogManager.hpp"
#include "Slic3r/Domain/ConfigPack.hpp"
#include "Slic3r/Domain/Preset/Types.hpp"

#include <string>
#include <vector>

namespace Slic3r::Biz::Preset {
class IPresetNameProvider;
class PresetInteractor;
struct PresetSelectionNames;
} // namespace Slic3r::Biz::Preset

namespace Slic3r::App::Mobile {

/// Asks for the name to save a preset under (SavePresetDialog with one item). Returns "" when
/// cancelled. `reserved` are names already chosen for other presets of the same kind.
std::string ask_preset_name(
    IMobileHost& host,
    Domain::Preset::PresetKind kind,
    const std::string& original_name,
    const Biz::Preset::IPresetNameProvider& names,
    const std::vector<std::string>& reserved = {}
);

/// SavePresetDialog for a print with several tool prints. Empty map when cancelled; "" for the
/// presets the user chose not to save.
Biz::Preset::IPresetDialogManager::NamesPerKindMap ask_print_tool_preset_names(
    IMobileHost& host,
    const Biz::Preset::IPresetDialogManager::NamesPerKindMap& original_names_per_kind,
    const Biz::Preset::IPresetNameProvider& names
);

/// UnsavedChangesDialog: one form per modified preset. Empty map when cancelled.
Biz::Preset::IPresetDialogManager::PresetsSwitchStates ask_unsaved_changes(
    IMobileHost& host,
    const std::string& dialog_name,
    const Domain::ConfigPack& config_original,
    const Domain::ConfigPack& config_selected,
    const Domain::ConfigPack* config_new_selected,
    const Biz::Preset::PresetSelectionNames& preset_names,
    const Biz::Preset::PresetSelectionNames& preset_names_new,
    const Biz::Preset::PresetInteractor& preset_interactor,
    bool new_printer_has_multiple_extruders
);

/// Text of a config value as the desktop diff dialogs show it ("N/A" without a config).
std::string display_value(const Domain::ConfigBox* config, const std::string& key);

/// "Category › Group › Option" of a config key.
std::string option_display_name(const Domain::ConfigBox& config, const std::string& key, Domain::PrinterTechnology technology);

} // namespace Slic3r::App::Mobile
