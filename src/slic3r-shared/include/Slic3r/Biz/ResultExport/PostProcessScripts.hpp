#pragma once

#include <boost/filesystem/path.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Slic3r::Biz::ResultExport {

/// Environment passed to post-processing scripts (name, value).
using ScriptEnvironment = std::vector<std::pair<std::string, std::string>>;

/**
 * @brief Runs the user's post-processing scripts on an exported (ASCII) G-code file.
 *
 * Each non-empty line of each entry is a shell command; the G-code path is appended as its
 * last argument and the script edits the file in place (as in PrusaSlicer 2). The scripts get
 * the print settings as SLIC3R_<OPTION> environment variables plus SLIC3R_PP_HOST and
 * SLIC3R_PP_OUTPUT_NAME. On Android, files apps write cannot be executed, so a command whose
 * first word is a script file is run through /system/bin/sh.
 *
 * @throws Slic3r::RuntimeError when a script fails (with its error output).
 */
void run_post_process_scripts(
    const std::vector<std::string>& scripts,
    const boost::filesystem::path& gcode_path,
    const ScriptEnvironment& environment
);

} // namespace Slic3r::Biz::ResultExport
