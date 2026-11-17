#pragma once

namespace Slic3r::App {
class InitParams;
} // namespace Slic3r::App

namespace Slic3r::App::CLI {

class CLIRuntime;

int run(Slic3r::App::InitParams& init_params);

/// Runs the CLI on an existing runtime. The preset bundle is loaded only if the runtime has
/// not loaded it yet, so a long-lived host (e.g. the Android app) pays for loading the
/// presets once instead of on every run. The caller should call
/// CLIRuntime::remove_all_projects() after each run.
int run(Slic3r::App::InitParams& init_params, CLIRuntime& runtime);

} // namespace Slic3r::App::CLI
