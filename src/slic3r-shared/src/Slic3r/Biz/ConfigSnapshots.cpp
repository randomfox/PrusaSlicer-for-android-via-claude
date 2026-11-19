#include "Slic3r/Biz/ConfigSnapshots.hpp"

#include "Slic3r/Biz/Algorithms/MiniZWrapper.hpp"
#include "Slic3r/Log.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Version.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <fmt/format.h>

#include <ctime>
#include <map>
#include <sstream>

namespace Slic3r::Biz::ConfigSnapshots {

namespace fs = boost::filesystem;

namespace {

// What a snapshot holds, relative to the data directory.
const std::vector<std::string> snapshot_dirs{"presets", "physical_printer"};
const std::vector<std::string> snapshot_files{"shared_runtime/RepositoryManifest.json"};
constexpr const char* info_entry = "snapshot.ini";

fs::path snapshots_dir() { return fs::path(Slic3r::data_dir()) / "snapshots"; }

std::string read_file(const fs::path& path)
{
    boost::nowide::ifstream file(path.string(), std::ios::binary);
    std::ostringstream data;
    data << file.rdbuf();
    return data.str();
}

std::map<std::string, std::string> parse_info(const std::string& text)
{
    std::map<std::string, std::string> values;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (const size_t eq = line.find(" = "); eq != std::string::npos) {
            values[line.substr(0, eq)] = line.substr(eq + 3);
        }
    }
    return values;
}

} // namespace

tl::expected<Snapshot, std::string> take_snapshot(const std::string& comment)
{
    boost::system::error_code ec;
    fs::create_directories(snapshots_dir(), ec);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32], file_stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    std::strftime(file_stamp, sizeof file_stamp, "%Y%m%d-%H%M%S", &local);
    fs::path path = snapshots_dir() / (std::string(file_stamp) + ".zip");
    for (int i = 2; fs::exists(path, ec); ++i) {
        path = snapshots_dir() / fmt::format("{}-{}.zip", file_stamp, i);
    }

    Algorithms::MZ_Archive archive;
    if (!Algorithms::open_zip_writer(&archive.arch, path.string())) {
        return tl::make_unexpected(fmt::format("Cannot create {}", path.string()));
    }
    bool ok = true;
    const auto add = [&](const std::string& name, const std::string& data) {
        ok = ok && mz_zip_writer_add_mem(&archive.arch, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    };
    std::string single_line_comment = comment;
    std::replace(single_line_comment.begin(), single_line_comment.end(), '\n', ' ');
    add(info_entry, fmt::format("id = {}\ncomment = {}\napp_version = {}\n", stamp, single_line_comment, SLIC3R_VERSION));
    const fs::path data_dir(Slic3r::data_dir());
    for (const std::string& dir : snapshot_dirs) {
        if (!fs::is_directory(data_dir / dir, ec)) {
            continue;
        }
        for (fs::recursive_directory_iterator it(data_dir / dir, ec), end; ok && !ec && it != end; it.increment(ec)) {
            if (fs::is_regular_file(it->path(), ec)) {
                add(fs::relative(it->path(), data_dir, ec).generic_string(), read_file(it->path()));
            } else if (fs::is_directory(it->path(), ec)) {
                add(fs::relative(it->path(), data_dir, ec).generic_string() + "/", {}); // keeps empty folders
            }
        }
    }
    for (const std::string& file : snapshot_files) {
        if (fs::is_regular_file(data_dir / file, ec)) {
            add(file, read_file(data_dir / file));
        }
    }
    ok = ok && mz_zip_writer_finalize_archive(&archive.arch);
    Algorithms::close_zip_writer(&archive.arch);
    if (!ok) {
        fs::remove(path, ec);
        return tl::make_unexpected(std::string{"The configuration snapshot could not be written."});
    }
    return Snapshot{path, stamp, single_line_comment, SLIC3R_VERSION};
}

std::vector<Snapshot> list_snapshots()
{
    std::vector<Snapshot> snapshots;
    boost::system::error_code ec;
    for (fs::directory_iterator it(snapshots_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!boost::algorithm::iends_with(it->path().string(), ".zip")) {
            continue;
        }
        Algorithms::MZ_Archive archive;
        if (!Algorithms::open_zip_reader(&archive.arch, it->path().string())) {
            continue;
        }
        size_t size = 0;
        void* data  = mz_zip_reader_extract_file_to_heap(&archive.arch, info_entry, &size, 0);
        if (data != nullptr) {
            const auto info = parse_info(std::string(static_cast<const char*>(data), size));
            mz_free(data);
            const auto value = [&info](const char* key) { const auto i = info.find(key); return i == info.end() ? std::string{} : i->second; };
            snapshots.push_back(Snapshot{it->path(), value("id"), value("comment"), value("app_version")});
        }
        Algorithms::close_zip_reader(&archive.arch);
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& a, const Snapshot& b) { return a.id > b.id; });
    return snapshots;
}

tl::expected<void, std::string> restore_snapshot(const Snapshot& snapshot)
{
    // Extract into a temporary folder first, so a broken archive changes nothing.
    const fs::path data_dir(Slic3r::data_dir());
    const fs::path staging = snapshots_dir() / ".restore";
    boost::system::error_code ec;
    fs::remove_all(staging, ec);
    Algorithms::MZ_Archive archive;
    if (!Algorithms::open_zip_reader(&archive.arch, snapshot.path.string())) {
        return tl::make_unexpected(fmt::format("Cannot open {}", snapshot.path.string()));
    }
    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&archive.arch);
    for (mz_uint i = 0; ok && i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive.arch, i, &stat) || std::string(stat.m_filename) == info_entry) {
            continue;
        }
        const std::string name = stat.m_filename;
        if (name.find("..") != std::string::npos || name.empty() || name.front() == '/') {
            continue; // not written by take_snapshot()
        }
        if (stat.m_is_directory) {
            fs::create_directories(staging / name, ec);
            continue;
        }
        const fs::path target = staging / name;
        fs::create_directories(target.parent_path(), ec);
        std::string data(size_t(stat.m_uncomp_size), '\0');
        ok = mz_zip_reader_extract_to_mem(&archive.arch, i, data.data(), data.size(), 0);
        boost::nowide::ofstream file(target.string(), std::ios::binary);
        file.write(data.data(), std::streamsize(data.size()));
        ok = ok && bool(file);
    }
    Algorithms::close_zip_reader(&archive.arch);
    if (!ok) {
        fs::remove_all(staging, ec);
        return tl::make_unexpected(std::string{"The configuration snapshot could not be read."});
    }

    if (const auto before = take_snapshot("Before restoring the snapshot of " + snapshot.id); !before) {
        fs::remove_all(staging, ec);
        return tl::make_unexpected(before.error());
    }
    for (const std::string& dir : snapshot_dirs) {
        fs::remove_all(data_dir / dir, ec);
        if (fs::exists(staging / dir, ec)) {
            fs::rename(staging / dir, data_dir / dir, ec);
            if (ec) {
                return tl::make_unexpected(fmt::format("Restoring {} failed: {}", dir, ec.message()));
            }
        } else {
            fs::create_directories(data_dir / dir, ec);
        }
    }
    for (const std::string& file : snapshot_files) {
        if (fs::exists(staging / file, ec)) {
            fs::copy_file(staging / file, data_dir / file, fs::copy_options::overwrite_existing, ec);
        }
    }
    fs::remove_all(staging, ec);
    SPDLOG_INFO("Configuration snapshot {} restored", snapshot.id);
    return {};
}

} // namespace Slic3r::Biz::ConfigSnapshots
