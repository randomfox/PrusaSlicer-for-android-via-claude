#pragma once

#include "Slic3r/Domain/Types.hpp"

#include <optional>
#include <vector>

struct indexed_triangle_set;

namespace Slic3r::Biz {

struct OrientationResult
{
    Domain::Vec3d down;            ///< Direction (in the mesh's frame) that should point down.
    double overhang_area{0.};      ///< Area needing support in that orientation (mm2).
    double current_overhang_area{0.};
};

/**
 * 2.x "Optimize orientation": finds the orientation needing the least support, i.e. with the
 * smallest area of faces looking down more steeply than `overhang_angle` (degrees from the
 * vertical) that do not lie on the bed; a lower height wins between orientations that are
 * about as good. The meshes are in world coordinates.
 * Returns nothing when the current orientation is already (about) the best.
 */
std::optional<OrientationResult> optimize_orientation(
    const std::vector<const indexed_triangle_set*>& meshes,
    const std::vector<Domain::Transform3d>& transformations,
    double overhang_angle = 45.
);

} // namespace Slic3r::Biz
