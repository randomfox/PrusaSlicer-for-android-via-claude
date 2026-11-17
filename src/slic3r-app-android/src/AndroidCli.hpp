#pragma once

#include <string>
#include <vector>

namespace Slic3r::App::Android {

/// Points PrusaSlicer at its resources and at writable data/cache directories and performs
/// the one-time process initialization. Safe to call repeatedly; only the first call
/// initializes. Throws on failure.
void initialize(const std::string& resources_dir, const std::string& data_dir, const std::string& cache_dir);

/// Runs the PrusaSlicer command line interface. `args` excludes the program name.
/// Returns the process-style exit code. The preset bundle is loaded by the first run that
/// needs it and kept in memory for later runs. Not thread safe; call from one thread.
int run(std::vector<std::string> args);

} // namespace Slic3r::App::Android
