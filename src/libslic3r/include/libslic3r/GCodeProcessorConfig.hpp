#pragma once

#include "Slic3r/Biz/libpgcode/ProcessorConfig.hpp"
#include "Slic3r/Domain/ConfigPack.hpp"
#include "Slic3r/Domain/Preset/HwConfig.hpp"

namespace Slic3r::Biz::Slicing {

/**
 * G-code processor settings for a print configuration, the same ones G-code export uses.
 * Used by the G-code viewer to process an existing G-code file with the configuration stored
 * in it.
 */
libpgcode::ProcessorConfig gcode_processor_config(
    const Domain::ConfigPackFDM& config,
    const Domain::Preset::HwPrinterConfig& hw_printer_config
);

} // namespace Slic3r::Biz::Slicing
