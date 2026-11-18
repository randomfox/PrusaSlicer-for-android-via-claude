#include "Slic3r/Biz/GCodeViewer/GCodeFileLoader.hpp"

#include "Slic3r/Biz/Config/ConfigLegacy.hpp"
#include "Slic3r/Biz/Config/Legacy/PrintConfig.hpp"
#include "Slic3r/Biz/I18N/I18N.hpp"
#include "Slic3r/Biz/libpgcode/Processor.hpp"
#include "Slic3r/Domain/FullConfigFDM.hpp"
#include "Slic3r/Log.hpp"
#include "libslic3r/ConfigViews.hpp"
#include "libslic3r/GCode.hpp"

#include <LibBGCode/convert/convert.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/cstdio.hpp>

#include <cstdio>
#include <memory>
#include <numeric>

namespace Slic3r::Biz::GCodeViewer {

namespace {

struct FileCloser
{
    void operator()(FILE* file) const
    {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

/// The G-code as text (binary G-code is converted).
tl::expected<std::string, std::string> read_gcode_text(const boost::filesystem::path& path)
{
    FilePtr file{boost::nowide::fopen(path.string().c_str(), "rb")};
    if (!file) {
        return tl::make_unexpected(_u8L("Cannot open the file."));
    }
    char magic[4] = {};
    const size_t read = std::fread(magic, 1, 4, file.get());
    std::rewind(file.get());

    if (read == 4 && std::string_view(magic, 4) == "GCDE") {
        FilePtr ascii{std::tmpfile()};
        if (!ascii) {
            return tl::make_unexpected(_u8L("Cannot create a temporary file."));
        }
        const bgcode::core::EResult res = bgcode::convert::from_binary_to_ascii(*file, *ascii, true);
        if (res != bgcode::core::EResult::Success) {
            return tl::make_unexpected(std::string{bgcode::core::translate_result(res)});
        }
        std::fflush(ascii.get());
        std::rewind(ascii.get());
        file = std::move(ascii);
    }

    std::string text;
    std::vector<char> buffer(1 << 16);
    for (size_t n; (n = std::fread(buffer.data(), 1, buffer.size(), file.get())) > 0;) {
        text.append(buffer.data(), n);
    }
    return text;
}

/// A printer with one tool per nozzle diameter found in the G-code.
Domain::Preset::HwPrinterConfig hw_config_for(const std::vector<double>& nozzle_diameters)
{
    Domain::Preset::HwPrinterConfig hw{};
    hw.id         = "gcode-viewer";
    hw.name       = "G-code viewer";
    hw.technology = Domain::PrinterTechnology::FFF;
    const size_t tools = std::max<size_t>(1, nozzle_diameters.size());
    hw.tool_count      = int(tools);
    for (size_t i = 0; i < tools; ++i) {
        const double diameter = i < nozzle_diameters.size() ? nozzle_diameters[i] : 0.4;
        hw.tools.push_back(Domain::Preset::HwToolConfig{.features = {{"nozzle_diameter", diameter}}});
    }
    return hw;
}

} // namespace

bool is_gcode_file(const boost::filesystem::path& path)
{
    const std::string ext = path.extension().string();
    for (const char* known : {".gcode", ".gco", ".g", ".ngc", ".bgcode", ".bgc"}) {
        if (boost::iequals(ext, known)) {
            return true;
        }
    }
    return false;
}

tl::expected<Slicing::FDMResult, std::string> load_gcode_file(const boost::filesystem::path& path)
{
    tl::expected<std::string, std::string> text = read_gcode_text(path);
    if (!text) {
        return tl::make_unexpected(text.error());
    }
    if (text->empty()) {
        return tl::make_unexpected(_u8L("The file is empty."));
    }

    // Settings from the configuration block at the end of the G-code (legacy key = value format).
    Slic3rLegacy::DynamicPrintConfig legacy_config;
    Slic3rLegacy::ConfigSubstitutionContext substitutions(Slic3rLegacy::ForwardCompatibilitySubstitutionRule::EnableSilent);
    size_t settings = 0;
    try {
        settings = Slic3rLegacy::ConfigBase::load_from_gcode_string_legacy(legacy_config, text->c_str(), substitutions);
    } catch (const std::exception& e) {
        SPDLOG_WARN("G-code viewer: cannot read the configuration of {}: {}", path.string(), e.what());
    }
    SPDLOG_INFO("G-code viewer: {} settings read from {}", settings, path.string());

    std::vector<double> nozzle_diameters{0.4};
    if (const auto* option = legacy_config.option<Slic3rLegacy::ConfigOptionFloats>("nozzle_diameter"); option && !option->values.empty()) {
        nozzle_diameters = option->values;
    }

    Biz::libpgcode::ProcessorConfig processor_config;
    try {
        const Domain::ConfigPack pack = Biz::convert_dynamic_print_config_to_new(legacy_config);
        const auto* fdm               = std::get_if<Domain::ConfigPackFDM>(&pack);
        if (fdm == nullptr) {
            return tl::make_unexpected(_u8L("Only FDM G-code can be previewed."));
        }
        const Domain::Preset::HwPrinterConfig hw = hw_config_for(nozzle_diameters);
        std::vector<unsigned> extruders(hw.tools.size());
        std::iota(extruders.begin(), extruders.end(), 0u);
        auto full_config = std::make_shared<const Domain::FullConfigFDM>(*fdm, extruders, hw);
        processor_config = GCode::processor_config(PrintConfigView{full_config}, hw);
    } catch (const std::exception& e) {
        SPDLOG_WARN("G-code viewer: using default settings for {}: {}", path.string(), e.what());
        processor_config = Biz::libpgcode::ProcessorConfig{};
    }
    processor_config.producer = Biz::libpgcode::detect_producer(*text);

    try {
        Biz::libpgcode::Processor processor{std::move(processor_config)};
        processor.process_buffer(std::move(*text));
        return processor.finalize();
    } catch (const std::exception& e) {
        return tl::make_unexpected(std::string{e.what()});
    }
}

} // namespace Slic3r::Biz::GCodeViewer
