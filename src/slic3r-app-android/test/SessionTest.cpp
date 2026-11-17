// Exercises the long-lived session used by the Android app: several CLI runs in one
// process, reusing the loaded preset bundle. Not built by default:
//   ninja prusaslicer-android-session-test
// Usage (on a device, or via qemu-aarch64 with an Android sysroot):
//   PRUSASLICER_RESOURCES_DIR=... session-test <model.stl> <output dir>

#include "AndroidCli.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
int timed(const char* label, std::vector<std::string> args)
{
    const auto start = std::chrono::steady_clock::now();
    const int rc = Slic3r::App::Android::run(std::move(args));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("[session-test] %-34s rc=%d  %.2f s\n", label, rc, seconds);
    std::fflush(stdout);
    return rc;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <model.stl> <output dir>\n", argv[0]);
        return 2;
    }
    const std::string model = argv[1];
    const std::string out   = std::string{argv[2]} + "/";
    const char* home        = std::getenv("HOME");
    const std::string data  = std::string{home != nullptr ? home : "."};
    Slic3r::App::Android::initialize(std::getenv("PRUSASLICER_RESOURCES_DIR"), data + "/.config", data + "/.cache");

    int failures = 0;
    failures += timed("query printers (loads presets)", {"--query-printer-models", "--output", out + "printers.json"}) != 0;
    failures += timed("query profiles MK4S", {"--query-print-tool-filament-profiles", "--printer-profile", "Prusa MK4S 0.4 HF", "--output", out + "profiles.json"}) != 0;
    failures += timed("slice MK4S", {"--export-gcode", "--printer-profile", "Prusa MK4S 0.4 HF",
        "--print-profile", "0.20mm SPEED @MK4S 0.4HF", "--material-profile", "Prusament PLA @MK4S HF0.4@MK4S 0.4",
        "--output", out + "mk4s.gcode", model}) != 0;
    failures += timed("query profiles MINI", {"--query-print-tool-filament-profiles", "--printer-profile", "Prusa MINI & MINI+ 0.4", "--output", out + "profiles_mini.json"}) != 0;
    failures += timed("slice MK4S again", {"--export-gcode", "--printer-profile", "Prusa MK4S 0.4 HF",
        "--print-profile", "0.20mm SPEED @MK4S 0.4HF", "--material-profile", "Prusament PLA @MK4S HF0.4@MK4S 0.4",
        "--output", out + "mk4s_again.gcode", model}) != 0;
    failures += timed("slice with bad profile (expect rc!=0)", {"--export-gcode", "--printer-profile", "No such printer",
        "--print-profile", "x", "--material-profile", "y", "--output", out + "bad.gcode", model}) == 0;

    std::printf("[session-test] failures: %d\n", failures);
    std::fflush(stdout);
    std::_Exit(failures == 0 ? 0 : 1);
}
