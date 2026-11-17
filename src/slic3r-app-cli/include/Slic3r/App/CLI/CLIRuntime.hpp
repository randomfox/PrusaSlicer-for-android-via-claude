#pragma once

#include "Slic3r/Biz/IProjectsChangedListener.hpp"
#include "Slic3r/Biz/Platform/IMainThreadDispatcher.hpp"
#include "Slic3r/Biz/Platform/JobManager/JobManager.hpp"
#include "Slic3r/Biz/ProjectInteractor.hpp"
#include "Slic3r/Domain/SelectionId.hpp"
#include "Slic3r/Domain/Workbench.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/IThumbnailImageGenerator.hpp"

namespace Slic3r::App {
class InitParams;
} // namespace Slic3r::App

namespace Slic3r::App::CLI {

class CLIThumbnailImageGenerator final : public Biz::Slicing::IThumbnailImageGenerator
{
public:
    CLIThumbnailImageGenerator() = default;
    explicit CLIThumbnailImageGenerator(const std::vector<std::string>& input_files);

    std::future<Biz::Slicing::ThumbnailImageResults> enqueue_thumbnail_requests(
        const Biz::Slicing::ThumbnailImageRequests& thumbnail_requests
    ) override;

    void handle_enqueued_requests() override;

private:
    std::string m_input_3mf_filename;
};

struct ExportFinishedJobManagerStatusListener final :
    public Biz::Platform::JobManager::IJobManagerStatusChangedListener
{
    bool export_finished{false};
    std::optional<std::string> export_error;

    void on_job_manager_status_changed(
        const Biz::Platform::JobManager::JobManagerStatus& job_manager_status
    ) override;
};

struct ProjectLoadResultListener final : public Biz::IProjectsChangedListener
{
    std::optional<Domain::SelectionId> loaded_project_id;
    std::optional<std::string> load_error;

    void on_project_loaded(Domain::SelectionId project_id) override;
    void on_project_load_failed(const std::string& error) override;
    bool finished() const;
};

class CLIRuntime final
{
public:
    explicit CLIRuntime(const InitParams& init_params);
    ~CLIRuntime();

    CLIRuntime(const CLIRuntime&)            = delete;
    CLIRuntime& operator=(const CLIRuntime&) = delete;
    CLIRuntime(CLIRuntime&&)                 = delete;
    CLIRuntime& operator=(CLIRuntime&&)      = delete;

    const Biz::ProjectInteractor& project_interactor() const;

    Biz::ProjectInteractor& project_interactor();

    Biz::Platform::IMainThreadDispatcher& dispatcher();

    void load_standard_preset_bundle();

    /// True once load_standard_preset_bundle() succeeded. A long-lived runtime (see
    /// CLI::run(InitParams&, CLIRuntime&)) loads the bundle only once.
    bool is_preset_bundle_loaded() const { return m_preset_bundle_loaded; }

    /// Input files of the current run (used to take thumbnails from an input 3MF).
    void set_input_files(const std::vector<std::string>& input_files);

    /// Removes all projects, so that a long-lived runtime can be reused for the next run.
    void remove_all_projects();

    void wait_until(const std::function<bool()>& predicate);

private:
    bool m_preset_bundle_loaded{false};
    CLIThumbnailImageGenerator m_thumbnail_image_generator;
    Domain::Workbench m_workbench;
    std::optional<Biz::ProjectInteractor> m_project_interactor;
};

} // namespace Slic3r::App::CLI
