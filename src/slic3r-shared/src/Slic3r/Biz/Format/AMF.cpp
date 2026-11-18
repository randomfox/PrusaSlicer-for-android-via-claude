#include "Slic3r/Biz/Format/AMF.hpp"

#include "Slic3r/Biz/Algorithms/MiniZWrapper.hpp"
#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Biz/Algorithms/TriangleMesh.hpp"
#include "Slic3r/Domain/Model.hpp"
#include "Slic3r/Domain/ModelInstance.hpp"
#include "Slic3r/Domain/ModelObject.hpp"
#include "Slic3r/Domain/ModelVolume.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <fmt/format.h>
#include <pugixml.hpp>

#include <cstring>
#include <map>
#include <sstream>
#include <unordered_map>

namespace Slic3r::Biz {

namespace {

double unit_scale(const std::string& unit)
{
    if (unit == "inch") return 25.4;
    if (unit == "feet") return 304.8;
    if (unit == "meter") return 1000.;
    if (unit == "micron") return 0.001;
    return 1.; // millimeter (default)
}

std::string metadata(const pugi::xml_node& node, const char* type)
{
    for (const pugi::xml_node& m : node.children("metadata")) {
        if (std::strcmp(m.attribute("type").value(), type) == 0) {
            return m.text().get();
        }
    }
    return {};
}

Domain::ModelVolumeType volume_type(const pugi::xml_node& volume)
{
    const std::string type = metadata(volume, "slic3r.volume_type");
    if (type == "ParameterModifier") return Domain::ModelVolumeType::PARAMETER_MODIFIER;
    if (type == "NegativeVolume") return Domain::ModelVolumeType::NEGATIVE_VOLUME;
    if (type == "SupportBlocker") return Domain::ModelVolumeType::SUPPORT_BLOCKER;
    if (type == "SupportEnforcer") return Domain::ModelVolumeType::SUPPORT_ENFORCER;
    if (type.empty() && metadata(volume, "slic3r.modifier") == "1") {
        return Domain::ModelVolumeType::PARAMETER_MODIFIER;
    }
    return Domain::ModelVolumeType::MODEL_PART;
}

// The XML of the AMF: the file itself, or the .amf inside a zip archive.
tl::expected<std::string, std::string> read_amf_xml(const std::string& path)
{
    boost::nowide::ifstream file(path, std::ios::binary);
    if (!file) {
        return tl::make_unexpected(fmt::format("Cannot open {}", path));
    }
    std::ostringstream data;
    data << file.rdbuf();
    std::string content = data.str();
    if (content.size() < 2 || content[0] != 'P' || content[1] != 'K') {
        return content;
    }
    Algorithms::MZ_Archive archive;
    if (!Algorithms::open_zip_reader(&archive.arch, path)) {
        return tl::make_unexpected(fmt::format("Cannot open the zip archive {}", path));
    }
    std::string xml;
    const mz_uint count = mz_zip_reader_get_num_files(&archive.arch);
    for (mz_uint i = 0; i < count && xml.empty(); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive.arch, i, &stat) || stat.m_is_directory) {
            continue;
        }
        if (!boost::algorithm::iends_with(stat.m_filename, ".amf") && count > 1) {
            continue;
        }
        xml.resize(size_t(stat.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(&archive.arch, i, xml.data(), xml.size(), 0)) {
            xml.clear();
        }
    }
    Algorithms::close_zip_reader(&archive.arch);
    if (xml.empty()) {
        return tl::make_unexpected(fmt::format("No AMF found in {}", path));
    }
    return xml;
}

} // namespace

tl::expected<Domain::Model, std::string> load_amf(const std::string& path)
{
    tl::expected<std::string, std::string> xml = read_amf_xml(path);
    if (!xml) {
        return tl::make_unexpected(xml.error());
    }
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(xml->data(), xml->size());
    if (!parsed) {
        return tl::make_unexpected(fmt::format("The AMF file could not be parsed: {}", parsed.description()));
    }
    const pugi::xml_node amf = document.child("amf");
    if (!amf) {
        return tl::make_unexpected(std::string{"The file is not an AMF file."});
    }
    const float scale = float(unit_scale(amf.attribute("unit").value()));
    const std::string file_stem = boost::filesystem::path(path).stem().string();

    Domain::Model model;
    std::unordered_map<std::string, Domain::ModelObject*> objects_by_id;
    for (const pugi::xml_node& object_node : amf.children("object")) {
        const pugi::xml_node mesh = object_node.child("mesh");
        if (!mesh) {
            continue;
        }
        std::vector<Domain::Vec3f> vertices;
        for (const pugi::xml_node& vertex : mesh.child("vertices").children("vertex")) {
            const pugi::xml_node c = vertex.child("coordinates");
            vertices.emplace_back(
                c.child("x").text().as_float() * scale, c.child("y").text().as_float() * scale,
                c.child("z").text().as_float() * scale
            );
        }
        Domain::ModelObject* object = nullptr;
        int volume_index            = 0;
        for (const pugi::xml_node& volume_node : mesh.children("volume")) {
            // Each volume gets its own compact vertex list.
            std::vector<Domain::Vec3f> volume_vertices;
            std::vector<Domain::Index3> faces;
            std::map<int, int> remap;
            bool valid = true;
            for (const pugi::xml_node& triangle : volume_node.children("triangle")) {
                Domain::Index3 face;
                const char* names[] = {"v1", "v2", "v3"};
                for (int k = 0; k < 3; ++k) {
                    const int index = triangle.child(names[k]).text().as_int(-1);
                    if (index < 0 || index >= int(vertices.size())) {
                        valid = false;
                        break;
                    }
                    auto [it, inserted] = remap.emplace(index, int(volume_vertices.size()));
                    if (inserted) {
                        volume_vertices.push_back(vertices[index]);
                    }
                    face[k] = it->second;
                }
                if (!valid) {
                    break;
                }
                faces.push_back(face);
            }
            if (!valid) {
                return tl::make_unexpected(fmt::format("Invalid vertex index in the AMF object {}.", object_node.attribute("id").value()));
            }
            if (faces.empty()) {
                continue;
            }
            if (object == nullptr) {
                object             = model.add_object();
                object->input_file = path;
                object->name       = metadata(object_node, "name");
                if (object->name.empty()) {
                    object->name = model.objects.size() == 1 ? file_stem : fmt::format("{}_{}", file_stem, model.objects.size());
                }
            }
            Domain::TriangleMesh triangle_mesh =
                Algorithms::TriangleMesh::construct(std::move(volume_vertices), std::move(faces));
            Domain::ModelVolume* volume =
                Algorithms::ModelObject::add_volume(object, std::move(triangle_mesh), volume_type(volume_node));
            volume->name = metadata(volume_node, "name");
            if (volume->name.empty()) {
                volume->name = object->name + (volume_index > 0 ? fmt::format("_{}", volume_index + 1) : "");
            }
            volume->source.input_file = path;
            volume->source.object_idx = int(model.objects.size()) - 1;
            volume->source.volume_idx = int(object->volumes.size()) - 1;
            ++volume_index;
        }
        if (object != nullptr) {
            objects_by_id[object_node.attribute("id").value()] = object;
        }
    }
    if (model.objects.empty()) {
        return tl::make_unexpected(std::string{"The AMF file contains no objects."});
    }

    // Instances: the constellation (rotations in radians, as 2.x wrote them).
    for (const pugi::xml_node& constellation : amf.children("constellation")) {
        for (const pugi::xml_node& instance_node : constellation.children("instance")) {
            const auto it = objects_by_id.find(instance_node.attribute("objectid").value());
            if (it == objects_by_id.end()) {
                continue;
            }
            const auto value = [&](const char* name, double default_value) {
                const pugi::xml_node node = instance_node.child(name);
                return node ? node.text().as_double(default_value) : default_value;
            };
            const double uniform = value("scale", 1.);
            Domain::ModelInstance* instance = it->second->add_instance();
            instance->set_offset(Domain::Vec3d(value("deltax", 0.) * scale, value("deltay", 0.) * scale, value("deltaz", 0.) * scale));
            instance->set_rotation(Domain::Vec3d(value("rx", 0.), value("ry", 0.), value("rz", 0.)));
            instance->set_scaling_factor(Domain::Vec3d(value("scalex", uniform), value("scaley", uniform), value("scalez", uniform)));
        }
    }
    return model;
}

} // namespace Slic3r::Biz
