#pragma once

#include "Slic3r/Biz/PhysicalPrinter/PhysicalPrinterConfig.hpp"
#include "Slic3r/Biz/PrintHost/PrintHostJobData.hpp"
#include "Slic3r/Biz/PrintHost/PrintHostJob.hpp"
#include "Slic3r/Biz/Platform/WithListeners.hpp"
#include "Slic3r/Biz/Platform/IMainThreadDispatcher.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <functional>

namespace Slic3r::Biz::PrintHost {

/// One upload, for the upload queue window (2.x PrintHostQueueDialog).
struct UploadRecord
{
    enum class State { Queued, Uploading, Finished, Failed, Cancelled };

    size_t id{0};
    std::string printer;  ///< Printer (host) name.
    std::string host;
    std::string filename;
    State state{State::Queued};
    double progress{0.};  ///< 0..1 while uploading.
    std::string error;
};

class PrintHostJobManager
{
public:
    PrintHostJobManager();
    ~PrintHostJobManager();

    PrintHostJobManager(const PrintHostJobManager &) = delete;
    PrintHostJobManager(PrintHostJobManager &&other) = delete;
    PrintHostJobManager& operator=(const PrintHostJobManager &) = delete;
    PrintHostJobManager& operator=(PrintHostJobManager &&other) = delete;

    /*
     * @brief Defines and starts job in job manager. 
     */
    size_t emplace_job(PhysicalPrinter::PhysicalPrinterConfig config, PrintHostJobData data);

    void cancel_job(size_t id);

    /// The uploads of this session, oldest first (thread safe).
    std::vector<UploadRecord> uploads() const;
    /// Forgets the uploads that are no longer queued or running.
    void clear_finished_uploads();

private:
    void update_record(size_t id, const std::function<void(UploadRecord&)>& update);

    mutable std::mutex m_records_mutex;
    std::vector<UploadRecord> m_records;

    std::map<size_t,std::shared_ptr<PrintHostJobWrapper>> m_wrappers;
};

} // namespace Slic3r::Biz::Slicing
