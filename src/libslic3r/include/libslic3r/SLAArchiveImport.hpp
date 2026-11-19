#pragma once

#include <tl/expected.hpp>

#include <string>

struct indexed_triangle_set;

namespace Slic3r {

/**
 * 2.x "Import SLA archive": rebuilds a mesh from the layer images of an SL1 / SL1S archive
 * (PNG layers; the display geometry and layer heights come from its prusaslicer.ini).
 * The mesh is as precise as the printer's pixels and layers.
 */
tl::expected<indexed_triangle_set, std::string> import_sla_archive(const std::string& path);

} // namespace Slic3r
