#include "libslic3r/SLAArchiveImport.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/MarchingSquares.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "Slic3r/Biz/Algorithms/MiniZWrapper.hpp"
#include "Slic3r/Biz/Algorithms/PNGReadWrite.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <fmt/format.h>
#include <tbb/parallel_for.h>

#include <map>
#include <sstream>

namespace marchsq {
template<> struct _RasterTraits<Slic3r::png::ImageGreyscale>
{
    using Rst       = Slic3r::png::ImageGreyscale;
    using ValueType = uint8_t;
    static uint8_t get(const Rst& rst, size_t row, size_t col) { return rst.get(row, col); }
    static size_t rows(const Rst& rst) { return rst.rows; }
    static size_t cols(const Rst& rst) { return rst.cols; }
};
} // namespace marchsq

namespace Slic3r {

namespace {

std::map<std::string, std::string> parse_ini(const std::string& text)
{
    std::map<std::string, std::string> values;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        values[boost::algorithm::trim_copy(line.substr(0, eq))] = boost::algorithm::trim_copy(line.substr(eq + 1));
    }
    return values;
}

double get_double(const std::map<std::string, std::string>& ini, const std::string& key, double fallback)
{
    const auto it = ini.find(key);
    if (it == ini.end() || it->second.empty()) {
        return fallback;
    }
    try {
        return std::stod(it->second);
    } catch (...) {
        return fallback;
    }
}

struct RasterParams
{
    sla::RasterBase::Trafo trafo;
    double px_w{1.}, px_h{1.}; // mm per pixel, along the image columns / rows
};

// As sla::raster_to_polygons(), for a decoded PNG.
ExPolygons image_to_polygons(const png::ImageGreyscale& image, const RasterParams& params)
{
    const size_t rows = image.rows, cols = image.cols;
    if (rows < 2 || cols < 2) {
        return {};
    }
    const std::vector<marchsq::Ring> rings = marchsq::execute(image, uint8_t(128), marchsq::Coord{2, 2});
    const double px_w = (cols * params.px_w) / (cols - 1);
    const double px_h = (rows * params.px_h) / (rows - 1);
    Polygons polygons;
    polygons.reserve(rings.size());
    for (const marchsq::Ring& ring : rings) {
        Polygon polygon;
        polygon.points.reserve(ring.size());
        for (const marchsq::Coord& c : ring) {
            polygon.points.emplace_back(scaled(c.c * px_w), scaled(c.r * px_h));
        }
        polygons.emplace_back(std::move(polygon));
    }
    ExPolygons result = union_ex(polygons);
    const coord_t width = scaled(cols * px_h), height = scaled(rows * px_w);
    const auto& tr      = params.trafo;
    const auto for_each_point = [](ExPolygon& expoly, auto&& fn) {
        for (Point& p : expoly.contour.points) fn(p);
        for (Polygon& hole : expoly.holes)
            for (Point& p : hole.points) fn(p);
    };
    for (ExPolygon& expoly : result) {
        if (tr.mirror_y) for_each_point(expoly, [height](Point& p) { p.y() = height - p.y(); });
        if (tr.mirror_x) for_each_point(expoly, [width](Point& p) { p.x() = width - p.x(); });
        if (tr.flipXY) for_each_point(expoly, [](Point& p) { std::swap(p.x(), p.y()); });
        if ((tr.mirror_x + tr.mirror_y + tr.flipXY) % 2) {
            expoly.contour.reverse();
            for (Polygon& hole : expoly.holes) hole.reverse();
        }
    }
    return result;
}

} // namespace

tl::expected<indexed_triangle_set, std::string> import_sla_archive(const std::string& path)
{
    Biz::Algorithms::MZ_Archive archive;
    if (!Biz::Algorithms::open_zip_reader(&archive.arch, path)) {
        return tl::make_unexpected(fmt::format("Cannot open the SLA archive {}", path));
    }
    std::map<std::string, std::string> ini;
    std::vector<std::pair<std::string, mz_uint>> layers; // name, index
    bool svg_layers = false;
    const mz_uint count = mz_zip_reader_get_num_files(&archive.arch);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive.arch, i, &stat) || stat.m_is_directory) {
            continue;
        }
        const std::string name = stat.m_filename;
        if (name == "prusaslicer.ini") {
            std::string text(size_t(stat.m_uncomp_size), '\0');
            if (mz_zip_reader_extract_to_mem(&archive.arch, i, text.data(), text.size(), 0)) {
                ini = parse_ini(text);
            }
        } else if (name.find('/') == std::string::npos && boost::algorithm::iends_with(name, ".png")) {
            layers.emplace_back(name, i);
        } else if (name.find('/') == std::string::npos && boost::algorithm::iends_with(name, ".svg")) {
            svg_layers = true;
        }
    }
    if (layers.empty()) {
        Biz::Algorithms::close_zip_reader(&archive.arch);
        return tl::make_unexpected(svg_layers ? std::string{"SLA archives with SVG layers cannot be imported."} :
                                                std::string{"The archive contains no layers."});
    }
    if (ini.empty()) {
        Biz::Algorithms::close_zip_reader(&archive.arch);
        return tl::make_unexpected(std::string{"The archive contains no prusaslicer.ini with the printer settings."});
    }
    std::sort(layers.begin(), layers.end());

    // Display geometry, as the SL1 rasterizer sets it up.
    double w = get_double(ini, "display_width", 0.), h = get_double(ini, "display_height", 0.);
    double pw = get_double(ini, "display_pixels_x", 0.), ph = get_double(ini, "display_pixels_y", 0.);
    if (w <= 0. || h <= 0. || pw <= 0. || ph <= 0.) {
        Biz::Algorithms::close_zip_reader(&archive.arch);
        return tl::make_unexpected(std::string{"The archive has no valid display size."});
    }
    const bool portrait = ini["display_orientation"] == "portrait";
    if (portrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }
    const sla::RasterBase::TMirroring mirror{ini["display_mirror_x"] == "1", ini["display_mirror_y"] == "1"};
    RasterParams params{sla::RasterBase::Trafo{portrait ? sla::RasterBase::roPortrait : sla::RasterBase::roLandscape, mirror}, w / pw, h / ph};
    const double layer_height   = get_double(ini, "layer_height", 0.05);
    const double initial_height = get_double(ini, "initial_layer_height", layer_height);

    // Read the images (sequentially: miniz is not thread safe), trace them in parallel.
    std::vector<std::string> encoded(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        size_t size = 0;
        void* data  = mz_zip_reader_extract_to_heap(&archive.arch, layers[i].second, &size, 0);
        if (data != nullptr) {
            encoded[i].assign(static_cast<const char*>(data), size);
            mz_free(data);
        }
    }
    Biz::Algorithms::close_zip_reader(&archive.arch);

    std::vector<ExPolygons> slices(layers.size());
    std::atomic<bool> failed{false};
    tbb::parallel_for(size_t(0), layers.size(), [&](size_t i) {
        png::ImageGreyscale image;
        if (!png::decode_png(png::ReadBuf{encoded[i].data(), encoded[i].size()}, image)) {
            failed = true;
            return;
        }
        std::string().swap(encoded[i]);
        slices[i] = image_to_polygons(image, params);
    });
    if (failed) {
        return tl::make_unexpected(std::string{"A layer image of the archive could not be decoded (8 bit grayscale PNG expected)."});
    }
    indexed_triangle_set mesh = slices_to_mesh(slices, 0., layer_height, initial_height);
    if (mesh.indices.empty()) {
        return tl::make_unexpected(std::string{"The archive's layers are empty."});
    }
    return mesh;
}

} // namespace Slic3r
