#include "MobilePresetDialogs.hpp"

#include "Slic3r/Biz/Config/BedShape.hpp"
#include "Slic3r/Biz/Config/ConfigSerialize.hpp"
#include "Slic3r/Biz/I18N/I18N.hpp"
#include "Slic3r/Biz/Preset/NameValidator.hpp"
#include "Slic3r/Biz/Preset/PresetInteractor.hpp"
#include "Slic3r/Biz/Preset/PresetSelectionNames.hpp"
#include "Slic3r/Domain/ConfigDef.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <set>

namespace Slic3r::App::Mobile {

using Biz::_u8L;
using Biz::Preset::IPresetDialogManager;
using Biz::Preset::NameValidator;
using Biz::Preset::PresetDiffOperation;
using Biz::Preset::PresetSwitchKindId;
using Biz::Preset::PresetSwitchState;
using Domain::Preset::PresetKind;
using FieldType = MobileForm::Field::Type;

namespace {

std::string kind_name(PresetKind kind)
{
    switch (kind) {
    case PresetKind::FdmPrinter:
    case PresetKind::SlaPrinter: return _u8L("Printer");
    case PresetKind::FdmPrint:
    case PresetKind::SlaPrint: return _u8L("Print settings");
    case PresetKind::FdmToolPrint:
    case PresetKind::SlaToolPrint: return _u8L("Tool print settings");
    case PresetKind::FdmMaterial: return _u8L("Filament");
    case PresetKind::SlaMaterial: return _u8L("Material");
    }
    return {};
}

std::string item_as_string(const Domain::ConfigItem& item)
{
    if (item.name() == "bed_shape") {
        return Biz::Config::BedShape(item.get<std::vector<Domain::Vec2d>>()).get_full_name_with_params();
    }
    auto value = Biz::value_as_string(item);
    if (const std::string* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    std::string result;
    for (const std::string& part : std::get<std::vector<std::string>>(value)) {
        result += (result.empty() ? "" : "; ") + part;
    }
    return result;
}

/// The name PrusaSlicer suggests when saving (SavePresetDialog::Item).
std::string suggested_name(const NameValidator& validator, const std::string& original_name)
{
    for (const Domain::Preset::PresetName& name : validator.preset_names()) {
        if (name.name == original_name && name.origin == Domain::Preset::PresetOrigin::System) {
            return fmt::format("{} - {}", original_name, Biz::_ctx_u8L("Copy", "PresetName"));
        }
    }
    return original_name;
}

/// Name typed by the user, with the casing of an existing preset it matches.
std::string final_name(const NameValidator& validator, const std::string& typed)
{
    const std::string existing = validator.get_conflict_name(typed);
    return existing.empty() ? typed : existing;
}

std::string preset_display_name(const Biz::Preset::PresetSelectionNames::PresetName& name)
{
    return (name.is_runtime_only ? _u8L("(From 3mf) ") : std::string{}) + name.name;
}

const Biz::Preset::PresetSelectionNames::PresetName* name_of(
    const Biz::Preset::PresetSelectionNames& names, PresetKind kind, size_t index
)
{
    switch (kind) {
    case PresetKind::FdmPrinter:
    case PresetKind::SlaPrinter: return &names.printer;
    case PresetKind::FdmPrint:
    case PresetKind::SlaPrint: return &names.print;
    case PresetKind::FdmToolPrint:
    case PresetKind::SlaToolPrint: return index < names.tools.size() ? &names.tools[index] : nullptr;
    case PresetKind::FdmMaterial:
    case PresetKind::SlaMaterial: return index < names.materials.size() ? &names.materials[index] : nullptr;
    }
    return nullptr;
}

const Domain::ConfigBox* box_of(const Domain::ConfigPack* pack, PresetKind kind, size_t index)
{
    if (pack == nullptr) {
        return nullptr;
    }
    if (const auto* fdm = std::get_if<Domain::ConfigPackFDM>(pack)) {
        switch (kind) {
        case PresetKind::FdmPrinter: return &fdm->printer;
        case PresetKind::FdmPrint: return &fdm->print;
        case PresetKind::FdmToolPrint: return index < fdm->tool.size() ? &fdm->tool[index] : nullptr;
        case PresetKind::FdmMaterial: return index < fdm->filament.size() ? &fdm->filament[index] : nullptr;
        default: return nullptr;
        }
    }
    const auto& sla = std::get<Domain::ConfigPackSLA>(*pack);
    switch (kind) {
    case PresetKind::SlaPrinter: return &sla.sla_printer_settings;
    case PresetKind::SlaPrint: return &sla.sla_print_settings;
    case PresetKind::SlaMaterial: return &sla.sla_material_settings;
    default: return nullptr;
    }
}

} // namespace

std::string display_value(const Domain::ConfigBox* config, const std::string& key)
{
    if (config != nullptr) {
        if (const Domain::ConfigItem* override_item = config->overrides.find(key); override_item && config->overrides.get(key)) {
            return item_as_string(*override_item);
        }
        if (const Domain::ConfigItem* item = config->items.find(key)) {
            return item_as_string(*item);
        }
    }
    return _u8L("N/A");
}

std::string option_display_name(const Domain::ConfigBox& config, const std::string& key, Domain::PrinterTechnology technology)
{
    const Domain::ConstFindResult found = config.find(key);
    if (found.item == nullptr) {
        return key;
    }
    const Domain::ConfigItemDef& def = found.item->def();
    const std::string category       = Biz::_u8(Domain::ConfigItemDef::translate_category(def.category, technology));
    const std::string group          = def.option_group == Domain::ConfigItemDef::OptionGroup::Unknown ?
                                           std::string{} :
                                           Biz::_u8(Domain::ConfigItemDef::translate_option_group(def.option_group));
    const std::string label          = Biz::_u8(def.full_label.empty() ? def.label : def.full_label);
    std::string result = category;
    for (const std::string& part : {group, label}) {
        if (!part.empty()) {
            result += (result.empty() ? "" : " › ") + part;
        }
    }
    return result.empty() ? key : result;
}

std::string ask_preset_name(
    IMobileHost& host,
    PresetKind kind,
    const std::string& original_name,
    const Biz::Preset::IPresetNameProvider& names,
    const std::vector<std::string>& reserved
)
{
    NameValidator validator(names, kind, original_name, false);
    validator.set_reserved_preset_names(reserved);

    MobileForm form;
    form.title   = fmt::format("{}: {}", _u8L("Save preset"), kind_name(kind));
    form.text    = fmt::format(fmt::runtime(_u8L("Save \"{}\" as:")), original_name);
    form.fields  = {{.id = "name", .type = FieldType::Text, .label = _u8L("Name"), .value = suggested_name(validator, original_name)}};
    form.buttons = {_u8L("Save"), _u8L("Cancel")};

    std::string warned_name; // a name the user was warned about (overwriting) and may confirm
    for (;;) {
        const MobileFormResult result = host.show_form(form);
        if (result.button != 0) {
            return {};
        }
        const std::string typed                     = result.value("name");
        const NameValidator::ValidationResult check = validator.validate(typed);
        form.fields[0].value                        = typed;
        if (check.type == NameValidator::ValidationType::Invalid) {
            form.error = check.message;
            continue;
        }
        if (check.type == NameValidator::ValidationType::Warning && typed != warned_name) {
            // The desktop shows the warning while typing; here confirm it with a second "Save".
            form.error  = check.message;
            warned_name = typed;
            continue;
        }
        return final_name(validator, typed);
    }
}

IPresetDialogManager::NamesPerKindMap ask_print_tool_preset_names(
    IMobileHost& host,
    const IPresetDialogManager::NamesPerKindMap& original_names_per_kind,
    const Biz::Preset::IPresetNameProvider& names
)
{
    struct Slot
    {
        PresetKind kind;
        size_t index;
        std::string original;
        NameValidator validator;
        std::string id;
    };
    std::vector<Slot> slots;
    for (const auto& [kind, originals] : original_names_per_kind) {
        for (size_t i = 0; i < originals.size(); ++i) {
            slots.push_back({kind, i, originals[i], NameValidator(names, kind, originals[i], false), fmt::format("{}_{}", int(kind), i)});
        }
    }

    MobileForm form;
    form.title   = _u8L("Save presets");
    form.buttons = {_u8L("Save"), _u8L("Cancel")};
    for (const Slot& slot : slots) {
        const std::string label = slot.kind == PresetKind::FdmToolPrint ? fmt::format("{} {}", kind_name(slot.kind), slot.index + 1) :
                                                                          kind_name(slot.kind);
        form.fields.push_back({.id = "save_" + slot.id, .type = FieldType::Check, .label = fmt::format(fmt::runtime(_u8L("Save {}")), label), .value = "1"});
        form.fields.push_back({.id = "name_" + slot.id, .type = FieldType::Text, .label = fmt::format("\"{}\" →", slot.original),
                               .value = suggested_name(slot.validator, slot.original)});
    }

    std::set<std::string> warned;
    for (;;) {
        const MobileFormResult result = host.show_form(form);
        if (result.button != 0) {
            return {};
        }
        for (MobileForm::Field& field : form.fields) {
            field.value = result.value(field.id, field.value);
        }

        IPresetDialogManager::NamesPerKindMap names_per_kind;
        std::map<PresetKind, std::vector<std::string>> chosen; // for the reserved names
        std::vector<std::string> errors, warnings;
        bool any = false;
        for (Slot& slot : slots) {
            std::vector<std::string>& out = names_per_kind[slot.kind];
            if (result.value("save_" + slot.id) != "1") {
                out.emplace_back();
                continue;
            }
            any = true;
            const std::string typed = result.value("name_" + slot.id);
            slot.validator.set_reserved_preset_names(chosen[slot.kind]);
            const NameValidator::ValidationResult check = slot.validator.validate(typed);
            if (check.type == NameValidator::ValidationType::Invalid) {
                errors.push_back(fmt::format("\"{}\": {}", typed, check.message));
            } else if (check.type == NameValidator::ValidationType::Warning && warned.count(typed) == 0) {
                warnings.push_back(check.message);
                warned.insert(typed);
            }
            const std::string name = final_name(slot.validator, typed);
            out.push_back(name);
            chosen[slot.kind].push_back(name);
        }
        if (!any) {
            form.error = _u8L("Select at least one preset to save.");
        } else if (!errors.empty()) {
            form.error = fmt::format("{}", fmt::join(errors, "\n"));
        } else if (!warnings.empty()) {
            form.error = fmt::format("{}", fmt::join(warnings, "\n"));
        } else {
            return names_per_kind;
        }
    }
}

IPresetDialogManager::PresetsSwitchStates ask_unsaved_changes(
    IMobileHost& host,
    const std::string& dialog_name,
    const Domain::ConfigPack& config_original,
    const Domain::ConfigPack& config_selected,
    const Domain::ConfigPack* config_new,
    const Biz::Preset::PresetSelectionNames& preset_names,
    const Biz::Preset::PresetSelectionNames& preset_names_new,
    const Biz::Preset::PresetInteractor& preset_interactor,
    bool new_printer_has_multiple_extruders
)
{
    const bool is_fdm                          = std::holds_alternative<Domain::ConfigPackFDM>(config_original);
    const Domain::PrinterTechnology technology = is_fdm ? Domain::PrinterTechnology::FFF : Domain::PrinterTechnology::SLA;

    // The modified presets and their modified options (UnsavedChangesDialog::compare()).
    std::vector<std::pair<PresetSwitchKindId, std::vector<std::string>>> steps;
    auto add_step = [&](PresetKind kind, std::optional<size_t> id) {
        const Domain::ConfigBox* original = box_of(&config_original, kind, id.value_or(0));
        const Domain::ConfigBox* selected = box_of(&config_selected, kind, id.value_or(0));
        if (original == nullptr || selected == nullptr) {
            return;
        }
        std::vector<std::string> keys = original->diff_keys(*selected);
        // diff_keys() lists a key twice when both its value and its override differ.
        std::vector<std::string> unique;
        for (const std::string& key : keys) {
            if (std::find(unique.begin(), unique.end(), key) == unique.end()) {
                unique.push_back(key);
            }
        }
        if (!unique.empty()) {
            steps.emplace_back(PresetSwitchKindId{kind, id}, std::move(unique));
        }
    };
    if (is_fdm) {
        const auto& original = std::get<Domain::ConfigPackFDM>(config_original);
        const auto& selected = std::get<Domain::ConfigPackFDM>(config_selected);
        add_step(PresetKind::FdmPrinter, std::nullopt);
        add_step(PresetKind::FdmPrint, std::nullopt);
        for (size_t i = 0; i < std::min(original.tool.size(), selected.tool.size()); ++i) {
            add_step(PresetKind::FdmToolPrint, i);
        }
        for (size_t i = 0; i < std::min(original.filament.size(), selected.filament.size()); ++i) {
            add_step(PresetKind::FdmMaterial, i);
        }
    } else {
        add_step(PresetKind::SlaPrinter, std::nullopt);
        add_step(PresetKind::SlaPrint, std::nullopt);
        add_step(PresetKind::SlaMaterial, size_t{0});
    }

    IPresetDialogManager::PresetsSwitchStates states;
    if (steps.empty()) {
        // Nothing to ask about; an entry is needed, an empty result means "cancelled".
        states[{is_fdm ? PresetKind::FdmPrinter : PresetKind::SlaPrinter}] = {PresetDiffOperation::Discard, {}, {}};
        return states;
    }

    for (size_t step = 0; step < steps.size();) {
        const auto& [kind_id, keys] = steps[step];
        const PresetKind kind       = kind_id.kind;
        const size_t index          = kind_id.id.value_or(0);

        // Transfer ("Keep" when the preset stays the same) - UnsavedChangesDialog::update_transfer_button().
        bool can_transfer = config_new != nullptr;
        bool is_keep      = false;
        const auto* old_name = name_of(preset_names, kind, index);
        const auto* new_name = name_of(preset_names_new, kind, index);
        if (kind == PresetKind::FdmToolPrint) {
            can_transfer = can_transfer && (preset_names_new.tools.size() > 1 || new_printer_has_multiple_extruders) &&
                           index < preset_names_new.tools.size();
        } else if (kind == PresetKind::FdmMaterial || kind == PresetKind::SlaMaterial) {
            can_transfer = can_transfer && index < preset_names_new.materials.size();
        }
        if (can_transfer && old_name != nullptr && new_name != nullptr) {
            is_keep = *old_name == *new_name;
        }

        const Domain::ConfigBox* box_original = box_of(&config_original, kind, index);
        const Domain::ConfigBox* box_selected = box_of(&config_selected, kind, index);
        const Domain::ConfigBox* box_new      = box_of(config_new, kind, index);

        std::string slot = kind_name(kind);
        if (kind == PresetKind::FdmToolPrint || kind == PresetKind::FdmMaterial) {
            slot += fmt::format(" {}", index + 1);
        }

        MobileForm form;
        form.title = dialog_name.empty() ? _u8L("Unsaved changes") : dialog_name;
        std::string text;
        if (config_new == nullptr) {
            text += _u8L("Printer technology will be changed.") + "\n";
        }
        text += _u8L("Presets below have unsaved modifications.\nYou need to process those modifications first.");
        if (steps.size() > 1) {
            text += "\n\n" + fmt::format(fmt::runtime(_u8L("Step {} from {}")), step + 1, steps.size());
        }
        text += fmt::format("\n\n{} \"{}\"", slot, old_name ? preset_display_name(*old_name) : std::string{});
        if (can_transfer && new_name != nullptr && !is_keep) {
            text += fmt::format(" → \"{}\"", preset_display_name(*new_name));
        }
        form.text = text;

        const std::string transfer = is_keep ? _u8L("Keep the changes") : _u8L("Transfer the changes to the new preset");
        const std::string discard  = _u8L("Discard the changes");
        const std::string save     = _u8L("Save the changes as a preset");
        std::vector<std::string> actions;
        if (can_transfer) {
            actions.push_back(transfer);
        }
        actions.push_back(discard);
        actions.push_back(save);
        form.fields.push_back({.id = "action", .type = FieldType::Choice, .label = _u8L("What to do with the changes"), .value = actions.front(), .options = actions});
        form.fields.push_back({.id = "info", .type = FieldType::Info, .label = _u8L("Modified options (checked ones are kept / transferred)"), .value = {}});
        for (size_t i = 0; i < keys.size(); ++i) {
            const std::string& key = keys[i];
            std::string label = option_display_name(*box_original, key, technology) + "\n    " + display_value(box_original, key) + " → " +
                                display_value(box_selected, key);
            if (config_new != nullptr) {
                label += fmt::format("\n    ({}: {})", _u8L("new preset"), display_value(box_new, key));
            }
            form.fields.push_back({.id = fmt::format("k{}", i), .type = FieldType::Check, .label = label, .value = "1"});
        }
        form.buttons = {_u8L("OK"), _u8L("Cancel")};
        if (step > 0) {
            form.buttons.push_back(_u8L("Back"));
        }

        const MobileFormResult result = host.show_form(form);
        if (result.button == 2) {
            --step;
            continue;
        }
        if (result.button != 0) {
            return {}; // cancelled
        }

        const std::string action          = result.value("action");
        const PresetDiffOperation operation = action == save ? PresetDiffOperation::Save :
                                              action == discard ? PresetDiffOperation::Discard :
                                                                  PresetDiffOperation::Transfer;
        PresetSwitchState state{operation, {}, {}};
        if (operation == PresetDiffOperation::Save) {
            // Names already chosen for other presets of this kind (in the other steps).
            std::vector<std::string> reserved;
            for (const auto& [other_id, other_state] : states) {
                if (other_id.kind == kind && !(other_id == kind_id) && other_state.new_preset_name) {
                    reserved.push_back(*other_state.new_preset_name);
                }
            }
            const std::string name = ask_preset_name(host, kind, old_name ? old_name->name : std::string{}, preset_interactor, reserved);
            if (name.empty()) {
                continue; // back to this step
            }
            state.new_preset_name = name;
        }
        if (operation != PresetDiffOperation::Discard) {
            // Save: values of the *unchecked* options from the original config (they are not saved);
            // Transfer: values of the *checked* options from the modified config.
            const bool want_checked             = operation == PresetDiffOperation::Transfer;
            const Domain::ConfigBox* source     = operation == PresetDiffOperation::Save ? box_original : box_selected;
            for (size_t i = 0; i < keys.size(); ++i) {
                const bool checked = result.value(fmt::format("k{}", i), "1") == "1";
                if (checked != want_checked || source == nullptr) {
                    continue;
                }
                const std::string& key = keys[i];
                if (const Domain::ConfigItem* item = source->items.find(key)) {
                    state.items.insert({key, item->value()});
                }
                if (!source->overrides.empty()) {
                    if (const auto& override_item = source->overrides.get(key)) {
                        state.overrides.insert({key, override_item->value()});
                    } else {
                        state.overrides.insert({key, std::nullopt});
                    }
                }
            }
        }
        states[kind_id] = std::move(state);
        ++step;
    }
    return states;
}

} // namespace Slic3r::App::Mobile
