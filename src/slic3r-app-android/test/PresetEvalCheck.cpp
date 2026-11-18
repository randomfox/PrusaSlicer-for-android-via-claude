// Checks that the material preset sharing in PresetEvaluator gives the same result as the
// full evaluation (no caching), for every printer template of the bundled vendors.
//   preset-eval-check <resources>/presets

#include "Slic3r/Biz/Preset/HwConfigEvaluator.hpp"
#include "Slic3r/Biz/Preset/IO/BundleLoader.hpp"
#include "Slic3r/Biz/Preset/IO/BundlePaths.hpp"
#include "Slic3r/Biz/Preset/PresetEvaluator.hpp"

#include <chrono>
#include <cstdio>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Biz::Preset;

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <presets dir>\n", argv[0]);
        return 2;
    }
    IO::BundlePaths paths;
    paths.app_bundle_path = argv[1];
    Domain::Preset::Bundle bundle = IO::load_bundle(paths);

    size_t configs = 0, mismatches = 0, material_slots = 0, material_presets = 0;
    std::set<const void*> unique_lists;
    double t_cached = 0, t_full = 0;
    HwConfigEvaluator config_eval;
    for (auto& [vendor, vendor_bundle] : bundle.vendor_bundles) {
        PresetEvaluator evaluator{vendor_bundle.presets, vendor_bundle.vendor_data.info};
        for (const auto& hw_template : vendor_bundle.vendor_data.printer_configs) {
            const auto hw_config = config_eval.create_printer_config(hw_template, vendor_bundle.vendor_data);
            ++configs;
            auto t0          = std::chrono::steady_clock::now();
            const auto cached = evaluator.evaluate(hw_config, true);
            auto t1          = std::chrono::steady_clock::now();
            const auto full   = evaluator.evaluate(hw_config, false);
            auto t2          = std::chrono::steady_clock::now();
            t_cached += std::chrono::duration<double>(t1 - t0).count();
            t_full += std::chrono::duration<double>(t2 - t1).count();

            bool same = cached.size() == full.size();
            for (size_t p = 0; same && p < cached.size(); ++p) {
                const auto& a = cached[p].prints;
                const auto& b = full[p].prints;
                same = a.size() == b.size() && cached[p].preset.id == full[p].preset.id;
                for (size_t i = 0; same && i < a.size(); ++i) {
                    same = a[i].preset.id == b[i].preset.id && a[i].materials.size() == b[i].materials.size();
                    for (size_t s = 0; same && s < a[i].materials.size(); ++s) {
                        const auto& ma = a[i].materials[s];
                        const auto& mb = b[i].materials[s];
                        ++material_slots;
                        material_presets += ma.size();
                        unique_lists.insert(&ma.list());
                        same = ma.size() == mb.size();
                        for (size_t m = 0; same && m < ma.size(); ++m) {
                            same = ma[m].preset.id == mb[m].preset.id && ma[m].preset.name == mb[m].preset.name
                                && ma[m].preset.values == mb[m].preset.values
                                && ma[m].preset.features == mb[m].preset.features;
                        }
                    }
                }
            }
            if (!same) {
                ++mismatches;
                std::printf("MISMATCH: %s\n", hw_config.name.c_str());
            }
        }
    }
    std::printf("%zu printer configurations, %zu mismatches\n", configs, mismatches);
    std::printf("%zu material slots, %zu material presets referenced, %zu distinct lists stored\n",
                material_slots, material_presets, unique_lists.size());
    std::printf("evaluation: %.2f s shared, %.2f s full (single thread)\n", t_cached, t_full);
    return mismatches == 0 ? 0 : 1;
}
