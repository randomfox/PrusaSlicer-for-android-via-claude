#include "AndroidCli.hpp"

#include "Slic3r/App/AppConfig.hpp"
#include "Slic3r/App/AppServices.hpp"
#include "Slic3r/App/CLI/CLIApp.hpp"
#include "Slic3r/App/Init.hpp"
#include "Slic3r/App/Launcher/ReadCLI.hpp"
#include "Slic3r/Assert.hpp"
#include "Slic3r/Log.hpp"
#include "Slic3r/Version.hpp"
#include "libslic3r/Utils.hpp"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Slic3r::App::Android {

namespace {
std::once_flag g_init_flag;
} // namespace

void initialize(const std::string& resources_dir, const std::string& data_dir, const std::string& cache_dir)
{
    // Directories.cpp resolves the platform directories from these on Linux-like systems,
    // Init.cpp reads PRUSASLICER_RESOURCES_DIR on Android.
    ::setenv("PRUSASLICER_RESOURCES_DIR", resources_dir.c_str(), 1);
    ::setenv("HOME", data_dir.c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", data_dir.c_str(), 1);
    ::setenv("XDG_CACHE_HOME", cache_dir.c_str(), 1);
    ::setenv("TMPDIR", cache_dir.c_str(), 1);

    std::call_once(g_init_flag, []() {
        Slic3r::App::init_common();
        Slic3r::App::init_paths(Slic3r::App::InitParams{});
        Slic3r::init_assert();

        auto& app_services{Slic3r::App::AppServices::instance()};
        app_services.set_app_config(Slic3r::App::AppConfig::create_app_config());
    });
}

int run(std::vector<std::string> args)
{
    args.insert(args.begin(), "prusa-slicer");

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    ::CLI::App app{std::string{SLIC3R_BUILD_ID} + " for Android", "prusa-slicer"};
    Slic3r::App::InitParams init_params = Slic3r::App::Launcher::read_cli(
        app, SLIC3R_VERSION, static_cast<int>(args.size()), argv.data()
    );
    if (init_params.exit_code.has_value()) {
        return init_params.exit_code.value();
    }

    // Without --datadir, the default data directory is derived from XDG_CONFIG_HOME.
    Slic3r::App::init_paths(init_params);

    if (init_params.misc.loglevel.has_value()) {
        Slic3r::set_log_level(init_params.misc.loglevel.value());
    }
    if (init_params.misc.threads.has_value()) {
        Slic3r::thread_count = init_params.misc.threads.value();
    }

    if (!init_params.action.has_any_action() || init_params.action.gcode_viewer) {
        std::fprintf(stderr, "No action given. The Android build supports command line actions only, e.g. --export-gcode.\n");
        return EXIT_FAILURE;
    }

    return Slic3r::App::CLI::run(init_params);
}

} // namespace Slic3r::App::Android
