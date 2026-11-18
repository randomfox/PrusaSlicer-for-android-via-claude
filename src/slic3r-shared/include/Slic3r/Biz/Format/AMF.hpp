#pragma once

#include "tl/expected.hpp"

#include <string>

namespace Slic3r::Domain {
class Model;
}

namespace Slic3r::Biz {

/**
 * Loads an AMF file (plain XML or zip compressed, as PrusaSlicer 2.x wrote them): the objects
 * with their volumes (part, modifier, negative volume, support blocker / enforcer), the units
 * and the instances of the constellation. Material and print settings are not read.
 */
tl::expected<Domain::Model, std::string> load_amf(const std::string& path);

} // namespace Slic3r::Biz
