#include "Slic3r/App/Config/GCodePlaceholders.hpp"

#include "Slic3r/Biz/Config/Legacy/PrintConfig.hpp"
#include "Slic3r/Biz/I18N/I18N.hpp"
#include "Slic3r/Domain/ConfigDef.hpp"
#include "Slic3r/Domain/ConfigDefsFDM.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <set>

namespace Slic3r::App {

namespace {
std::string describe(const std::string& label, const std::string& tooltip)
{
    std::string text = label.empty() ? std::string{} : Biz::_u8(label);
    if (!tooltip.empty()) {
        text += (text.empty() ? "" : ": ") + Biz::_u8(tooltip);
    }
    return text;
}
} // namespace

bool is_custom_gcode_key(const std::string& key)
{
    return boost::algorithm::ends_with(key, "_gcode") || key == "template_custom_gcode";
}

std::vector<std::pair<std::string, std::string>> gcode_placeholders(const std::string& gcode_key)
{
    std::vector<std::pair<std::string, std::string>> result;
    std::set<std::string> seen;
    const auto add = [&](const std::string& name, const std::string& description) {
        if (seen.insert(name).second) {
            result.emplace_back(name, description);
        }
    };

    // Specific to this G-code.
    const auto& specific = Slic3rLegacy::custom_gcode_specific_placeholders();
    if (const auto it = specific.find(gcode_key); it != specific.end()) {
        for (const std::string& name : it->second) {
            const auto def = Slic3rLegacy::custom_gcode_specific_config_def.options.find(name);
            add(name, def == Slic3rLegacy::custom_gcode_specific_config_def.options.end() ? std::string{} :
                                                                                          describe(def->second.label, def->second.tooltip));
        }
    }

    // Slicing states, statistics, objects info, dimensions, timestamps, preset names.
    static const Slic3rLegacy::ReadOnlySlicingStatesConfigDef read_only_states;
    static const Slic3rLegacy::ReadWriteSlicingStatesConfigDef read_write_states;
    static const Slic3rLegacy::OtherSlicingStatesConfigDef other_states;
    static const Slic3rLegacy::PrintStatisticsConfigDef statistics;
    static const Slic3rLegacy::ObjectsInfoConfigDef objects_info;
    static const Slic3rLegacy::DimensionsConfigDef dimensions;
    static const Slic3rLegacy::TimestampsConfigDef timestamps;
    static const Slic3rLegacy::OtherPresetsConfigDef presets;
    for (const Slic3rLegacy::ConfigDef* def :
         {static_cast<const Slic3rLegacy::ConfigDef*>(&read_only_states), static_cast<const Slic3rLegacy::ConfigDef*>(&read_write_states),
          static_cast<const Slic3rLegacy::ConfigDef*>(&other_states), static_cast<const Slic3rLegacy::ConfigDef*>(&statistics),
          static_cast<const Slic3rLegacy::ConfigDef*>(&objects_info), static_cast<const Slic3rLegacy::ConfigDef*>(&dimensions),
          static_cast<const Slic3rLegacy::ConfigDef*>(&timestamps), static_cast<const Slic3rLegacy::ConfigDef*>(&presets)}) {
        for (const auto& [name, option] : def->options) {
            add(name, describe(option.label, option.tooltip));
        }
    }

    // All settings, by name.
    std::vector<std::pair<std::string, std::string>> settings;
    for (const Domain::ConfigItemDef& def : Domain::get_defs_fdm().defs()) {
        if (!is_custom_gcode_key(def.name) && !seen.count(def.name)) {
            settings.emplace_back(def.name, describe(def.label, def.tooltip));
        }
    }
    std::sort(settings.begin(), settings.end());
    for (const auto& [name, description] : settings) {
        add(name, description);
    }
    return result;
}

} // namespace Slic3r::App
