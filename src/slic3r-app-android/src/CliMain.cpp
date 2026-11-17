// Stand-alone `prusa-slicer` command line executable for Android (e.g. for Termux or adb
// shell). Uses the same code path as the app's JNI bridge.
//
// Resources are taken from $PRUSASLICER_RESOURCES_DIR, or from ../resources relative to
// the executable. Data and cache directories default to $HOME/.config and $HOME/.cache.

#include "AndroidCli.hpp"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/path.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::string env_or(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string{value} : fallback;
}
// Skips static destructors: their order across the statically linked libraries is not
// guaranteed, and some of them depend on each other (crash on exit otherwise).
[[noreturn]] void quick_exit_after_flush(int rc)
{
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    std::_Exit(rc);
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string home = env_or("HOME", ".");
        const boost::filesystem::path exe_dir = boost::dll::program_location().parent_path();
        Slic3r::App::Android::initialize(
            env_or("PRUSASLICER_RESOURCES_DIR", (exe_dir / ".." / "resources").string()),
            env_or("XDG_CONFIG_HOME", home + "/.config"),
            env_or("XDG_CACHE_HOME", home + "/.cache")
        );
        const int rc = Slic3r::App::Android::run(std::vector<std::string>(argv + 1, argv + argc));
        quick_exit_after_flush(rc);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Error: %s\n", ex.what());
    }
    quick_exit_after_flush(EXIT_FAILURE);
}
