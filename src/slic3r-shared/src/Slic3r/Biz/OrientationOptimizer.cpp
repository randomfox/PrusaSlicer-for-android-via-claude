#include "Slic3r/Biz/OrientationOptimizer.hpp"

#include "admesh/stl.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Slic3r::Biz {

namespace {

struct Facet
{
    Domain::Vec3d normal;
    double area;
    int v[3];
};

struct Mesh
{
    std::vector<Domain::Vec3d> vertices;
    std::vector<Facet> facets;
    double total_area{0.};
};

Mesh world_mesh(const std::vector<const indexed_triangle_set*>& meshes, const std::vector<Domain::Transform3d>& transformations)
{
    Mesh mesh;
    for (size_t m = 0; m < meshes.size(); ++m) {
        const int offset = int(mesh.vertices.size());
        for (const auto& v : meshes[m]->vertices) {
            mesh.vertices.push_back(transformations[m] * v.cast<double>());
        }
        for (const auto& f : meshes[m]->indices) {
            Facet facet;
            const Domain::Vec3d a = mesh.vertices[offset + f[0]], b = mesh.vertices[offset + f[1]], c = mesh.vertices[offset + f[2]];
            const Domain::Vec3d cross = (b - a).cross(c - a);
            const double length = cross.norm();
            if (length <= 0.) {
                continue;
            }
            facet.normal = cross / length;
            facet.area   = 0.5 * length;
            facet.v[0] = offset + f[0], facet.v[1] = offset + f[1], facet.v[2] = offset + f[2];
            mesh.total_area += facet.area;
            mesh.facets.push_back(facet);
        }
    }
    return mesh;
}

struct Score
{
    double overhang{0.};
    double contact{0.}; // area lying on the bed
    double height{0.};
    double cost{0.};
};

Score score(const Mesh& mesh, const Domain::Vec3d& down, double cos_overhang)
{
    double zmin = std::numeric_limits<double>::max(), zmax = std::numeric_limits<double>::lowest();
    for (const Domain::Vec3d& v : mesh.vertices) {
        const double z = -v.dot(down);
        zmin = std::min(zmin, z);
        zmax = std::max(zmax, z);
    }
    Score s;
    s.height = zmax - zmin;
    const double on_bed = zmin + 0.05;
    for (const Facet& f : mesh.facets) {
        const double c = f.normal.dot(down);
        if (c <= cos_overhang) {
            continue;
        }
        if (-mesh.vertices[f.v[0]].dot(down) < on_bed && -mesh.vertices[f.v[1]].dot(down) < on_bed
            && -mesh.vertices[f.v[2]].dot(down) < on_bed) {
            s.contact += f.area; // lies on the bed
            continue;
        }
        s.overhang += f.area * c;
    }
    // A pose standing on a corner or an edge needs no support but does not stick to the bed:
    // reward flat contact (up to 10 % of the surface).
    s.cost = s.overhang - 0.5 * std::min(s.contact, 0.1 * mesh.total_area);
    return s;
}

} // namespace

std::optional<OrientationResult> optimize_orientation(
    const std::vector<const indexed_triangle_set*>& meshes,
    const std::vector<Domain::Transform3d>& transformations,
    double overhang_angle
)
{
    const Mesh mesh = world_mesh(meshes, transformations);
    if (mesh.facets.empty()) {
        return std::nullopt;
    }
    const double cos_overhang = std::cos(overhang_angle * std::numbers::pi / 180.);

    // Candidates: an even sampling of the sphere, the normals of the largest faces (lying on a
    // flat face is often best) and the current orientation.
    std::vector<Domain::Vec3d> candidates{Domain::Vec3d(0., 0., -1.)};
    constexpr int sphere_samples = 1200;
    const double golden          = std::numbers::pi * (3. - std::sqrt(5.));
    for (int i = 0; i < sphere_samples; ++i) {
        const double z = 1. - 2. * (i + 0.5) / sphere_samples;
        const double r = std::sqrt(1. - z * z);
        candidates.emplace_back(r * std::cos(golden * i), r * std::sin(golden * i), z);
    }
    std::vector<size_t> by_area(mesh.facets.size());
    for (size_t i = 0; i < by_area.size(); ++i) {
        by_area[i] = i;
    }
    const size_t largest = std::min<size_t>(64, by_area.size());
    std::partial_sort(by_area.begin(), by_area.begin() + largest, by_area.end(), [&](size_t a, size_t b) {
        return mesh.facets[a].area > mesh.facets[b].area;
    });
    for (size_t i = 0; i < largest; ++i) {
        candidates.push_back(mesh.facets[by_area[i]].normal);
    }

    std::vector<Score> scores(candidates.size());
    tbb::parallel_for(size_t(0), candidates.size(), [&](size_t i) { scores[i] = score(mesh, candidates[i], cos_overhang); });

    // Least cost; within 2 % of the total area, the lower one.
    const double tolerance = 0.02 * mesh.total_area;
    double best_cost       = std::numeric_limits<double>::max();
    for (const Score& s : scores) {
        best_cost = std::min(best_cost, s.cost);
    }
    size_t best = 0;
    for (size_t i = 0; i < scores.size(); ++i) {
        if (scores[i].cost <= best_cost + tolerance
            && (scores[best].cost > best_cost + tolerance || scores[i].height < scores[best].height - 1e-6)) {
            best = i;
        }
    }
    // Keep the current orientation unless it is clearly worse.
    if (best == 0 || (scores[0].cost <= scores[best].cost + tolerance && scores[0].height <= scores[best].height + 1e-3)) {
        return std::nullopt;
    }
    return OrientationResult{candidates[best], scores[best].overhang, scores[0].overhang};
}

} // namespace Slic3r::Biz
