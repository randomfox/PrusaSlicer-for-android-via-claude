#pragma once

#include "MobileHost.hpp"

#include "Slic3r/App/Browser/AbstractBrowserLogic.hpp"
#include "Slic3r/App/Browser/AbstractBrowserLogicCommandHandler.hpp"
#include "Slic3r/Biz/UserAccount/IUserAccountListener.hpp"

#include <functional>
#include <memory>
#include <string>

namespace Slic3r::Biz {
class ProjectInteractor;
}

namespace Slic3r::App::Mobile {

/// One web view dialog (the mobile counterpart of WX::WebView::WebViewDialog): forwards web view
/// events from the host to PrusaSlicer's browser logic and the logic's commands to the host.
class MobileWebViewSession final :
    public Browser::AbstractBrowserLogicCommandHandler,
    public Biz::UserAccount::IUserAccountListener
{
public:
    using ClosedCallback = std::function<void(bool ok)>;

    MobileWebViewSession(
        int id,
        IMobileHost& host,
        std::unique_ptr<Browser::AbstractBrowserLogic> logic,
        Biz::ProjectInteractor* project_interactor,
        ClosedCallback on_closed
    );
    ~MobileWebViewSession() override;

    int id() const { return m_id; }
    bool is_closed() const { return m_closed; }

    /// Shows the web view.
    void open();

    // Events reported by the host.
    bool on_navigation(const std::string& url, const std::string& current_url); ///< false = cancel the navigation
    void on_loaded(const std::string& url);
    void on_script_message(const std::string& message);
    void on_error(const std::string& description);
    /// The user closed the web view (back button).
    void on_dismissed();

    // IUserAccountListener
    void on_user_account_id_success(bool is_refresh, const std::string& username) override;
    void on_user_account_logged_out() override;
    void on_user_account_will_refresh() override;

protected:
    bool handle_logic_command_LoadURL(const std::string& data) override;
    bool handle_logic_command_LoadRequest(const std::string& data) override;
    bool handle_logic_command_RunScript(const std::string& data) override;
    bool handle_logic_command_EndModalOK(const std::string& data) override;
    bool handle_logic_command_EndModalCancel(const std::string& data) override;
    bool handle_logic_command_DeleteCookies(const std::string& data) override;
    bool handle_logic_command_DeleteCookiesWithCounter(const std::string& data) override;
    bool handle_logic_command_Veto(const std::string& data) override { return false; }
    bool handle_logic_command_DoReload(const std::string& data) override;
    bool handle_logic_command_AddUserScript(const std::string& data) override;
    bool handle_logic_command_LoadResourcesPage(const std::string& data) override;
    bool handle_logic_command_RegisterPrusaSlicerURL(const std::string& data) override { return true; }
    bool handle_logic_command_SetLoadDefaultURLOnErrorTrue(const std::string& data) override { return true; }
    bool handle_logic_command_SetLoadDefaultURLOnErrorFalse(const std::string& data) override { return true; }
    bool handle_logic_command_SwitchToSlicing(const std::string& data) override { return true; }
    bool handle_logic_command_SetBasicAuth(const std::string& data) override { return false; }
    bool handle_logic_command_ClearBasicAuth(const std::string& data) override { return false; }

private:
    std::string resources_page_url(const std::string& name) const;
    void close(bool ok);

    int m_id;
    IMobileHost& m_host;
    std::unique_ptr<Browser::AbstractBrowserLogic> m_logic;
    Biz::ProjectInteractor* m_project_interactor;
    ClosedCallback m_on_closed;
    std::string m_current_url;
    bool m_closed{false};
};

} // namespace Slic3r::App::Mobile
