#pragma once

#include "libslic3r/InitPrint.hpp"

#include <boost/filesystem/path.hpp>
#include <tl/expected.hpp>

#include <string>

namespace Slic3r::Biz::GCodeViewer {

/// True for .gcode / .gco / .g / .ngc / .bgcode files.
bool is_gcode_file(const boost::filesystem::path& path);

/**
 * @brief PrusaSlicer 2.x G-code viewer: processes an existing G-code file (ASCII or binary) into
 * the same result the preview shows after slicing.
 *
 * The print settings are taken from the configuration block PrusaSlicer stores at the end of
 * the G-code (machine limits, filament diameters, bed shape, ...); G-codes without it are
 * processed with default settings.
 */
tl::expected<Slicing::FDMResult, std::string> load_gcode_file(const boost::filesystem::path& path);

} // namespace Slic3r::Biz::GCodeViewer
