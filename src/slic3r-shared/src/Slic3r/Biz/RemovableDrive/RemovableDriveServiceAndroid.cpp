// Android: removable storage is only reachable through the Storage Access Framework,
// so PrusaSlicer's removable drive detection/ejection reports no drives.

#include "Slic3r/Biz/RemovableDrive/RemovableDriveService.hpp"

namespace Slic3r::Biz::RemovableDrive {

namespace {

class RemovableDriveMonitorAndroid : public IRemovableDriveMonitor
{
public:
    void on_volumes_changed() override {}
    boost::filesystem::path get_path_on_removable_drive(const boost::filesystem::path&) const override { return {}; }
    boost::filesystem::path get_removable_drive_path_from_path(const boost::filesystem::path&) const override { return {}; }
    size_t removable_drives_count() const override { return 0; }
};

} // namespace

RemovableDriveService::RemovableDriveService(Platform::IMainThreadDispatcher& dispatcher) :
    m_monitor(std::make_unique<RemovableDriveMonitorAndroid>()),
    m_dispatcher(dispatcher)
{}

void RemovableDriveService::eject_in_thread(const boost::filesystem::path& path)
{
    dispatch_status_on_main_thread(path, RemovableDriveStatus::Failed);
}

} // namespace Slic3r::Biz::RemovableDrive
