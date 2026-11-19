#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Slic3r::App {

/// Placeholders a custom G-code can use (2.x "Edit custom G-code" dialog): the ones specific to
/// the G-code `gcode_key`, the slicing states, statistics and preset names, then all settings.
/// Pairs of the placeholder name and its description.
std::vector<std::pair<std::string, std::string>> gcode_placeholders(const std::string& gcode_key);

/// True for the settings holding custom G-code.
bool is_custom_gcode_key(const std::string& key);

} // namespace Slic3r::App
