#pragma once

#include <boost/filesystem/path.hpp>
#include <tl/expected.hpp>

#include <string>
#include <vector>

namespace Slic3r::Biz::ConfigSnapshots {

/// 2.x "Configuration snapshots": copies of the presets (system and user) and the physical
/// printers, kept in <data dir>/snapshots as zip files.
struct Snapshot
{
    boost::filesystem::path path;
    std::string id;      ///< Time stamp, e.g. 2026-11-19 10:20:30
    std::string comment;
    std::string app_version;
};

tl::expected<Snapshot, std::string> take_snapshot(const std::string& comment);

/// Newest first.
std::vector<Snapshot> list_snapshots();

/// Replaces the presets and physical printers by the snapshot's (after taking a snapshot of
/// the current ones). The application has to be restarted to load them.
tl::expected<void, std::string> restore_snapshot(const Snapshot& snapshot);

} // namespace Slic3r::Biz::ConfigSnapshots
