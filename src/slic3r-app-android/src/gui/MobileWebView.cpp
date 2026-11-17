#include "MobileWebView.hpp"

#include "Slic3r/App/AppConfig.hpp"
#include "Slic3r/App/AppServices.hpp"
#include "Slic3r/App/Theme.hpp"
#include "Slic3r/Biz/ProjectInteractor.hpp"
#include "Slic3r/Biz/UserAccount/UserAccountInteractor.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Log.hpp"

#include <boost/filesystem/path.hpp>

namespace Slic3r::App::Mobile {

using Command = IMobileHost::WebViewCommand;

MobileWebViewSession::MobileWebViewSession(
    int id,
    IMobileHost& host,
    std::unique_ptr<Browser::AbstractBrowserLogic> logic,
    Biz::ProjectInteractor* project_interactor,
    ClosedCallback on_closed
) :
    m_id(id),
    m_host(host),
    m_logic(std::move(logic)),
    m_project_interactor(project_interactor),
    m_on_closed(std::move(on_closed))
{
    if (m_project_interactor != nullptr) {
        m_project_interactor->user_account_interactor().add_listener<Biz::UserAccount::IUserAccountListener>(this);
    }
}

MobileWebViewSession::~MobileWebViewSession()
{
    if (m_project_interactor != nullptr) {
        m_project_interactor->user_account_interactor().remove_listener<Biz::UserAccount::IUserAccountListener>(this);
    }
}

std::string MobileWebViewSession::resources_page_url(const std::string& name) const
{
    const bool dark = AppServices::instance().app_config().get<Theme::Style>("theme") == Theme::Style::Dark;
    return "file://" + boost::filesystem::path(resources_dir()).generic_string() + "/web/" + name + (dark ? "_dark" : "") + ".html";
}

void MobileWebViewSession::open()
{
    const std::string url = m_logic->url().empty() ? resources_page_url(m_logic->loading_html()) : m_logic->url();
    m_current_url         = url;
    m_host.open_webview(m_id, m_logic->title(), url, m_logic->script_message_handler_names());
    process_logic_command_vector(m_logic->on_webview_created());
    process_logic_command_vector(m_logic->on_show_webview_event(true));
}

void MobileWebViewSession::close(bool ok)
{
    if (m_closed) {
        return;
    }
    m_closed = true;
    m_host.webview_command(m_id, Command::Close, {}, {});
    if (m_on_closed) {
        m_on_closed(ok);
    }
}

bool MobileWebViewSession::on_navigation(const std::string& url, const std::string& current_url)
{
    if (m_closed) {
        return false;
    }
    return process_logic_command_vector(m_logic->on_navigation_request_webview_event(url, current_url));
}

void MobileWebViewSession::on_loaded(const std::string& url)
{
    if (m_closed) {
        return;
    }
    m_current_url = url;
    process_logic_command_vector(m_logic->on_loaded_webview_event(url));
}

void MobileWebViewSession::on_script_message(const std::string& message)
{
    if (!m_closed) {
        process_logic_command_vector(m_logic->on_script_message_webview_event(message));
    }
}

void MobileWebViewSession::on_error(const std::string& description)
{
    SPDLOG_ERROR("WebView error: {}", description);
    if (!m_closed) {
        m_host.webview_command(m_id, Command::LoadUrl, resources_page_url(m_logic->error_html()), {});
    }
}

void MobileWebViewSession::on_dismissed()
{
    if (m_closed) {
        return;
    }
    process_logic_command_vector(m_logic->on_show_webview_event(false));
    m_closed = true; // the host already closed the view
    if (m_on_closed) {
        m_on_closed(false);
    }
}

void MobileWebViewSession::on_user_account_id_success(bool is_refresh, const std::string&)
{
    if (!m_closed) {
        process_logic_command_vector(m_logic->on_user_account_id_success(is_refresh, m_current_url));
    }
}

void MobileWebViewSession::on_user_account_logged_out()
{
    if (!m_closed) {
        process_logic_command_vector(m_logic->on_user_account_logged_out(m_current_url));
    }
}

void MobileWebViewSession::on_user_account_will_refresh()
{
    if (!m_closed) {
        process_logic_command_vector(m_logic->on_user_account_will_refresh());
    }
}

bool MobileWebViewSession::handle_logic_command_LoadURL(const std::string& data)
{
    m_host.webview_command(m_id, Command::LoadUrl, data, {});
    return true;
}

bool MobileWebViewSession::handle_logic_command_LoadRequest(const std::string& data)
{
    m_host.webview_command(m_id, Command::LoadUrlWithToken, data, m_logic->access_token());
    return true;
}

bool MobileWebViewSession::handle_logic_command_RunScript(const std::string& data)
{
    m_host.webview_command(m_id, Command::RunScript, data, {});
    return true;
}

bool MobileWebViewSession::handle_logic_command_EndModalOK(const std::string&)
{
    close(true);
    return true;
}

bool MobileWebViewSession::handle_logic_command_EndModalCancel(const std::string&)
{
    close(false);
    return true;
}

bool MobileWebViewSession::handle_logic_command_DeleteCookies(const std::string& data)
{
    m_host.webview_command(m_id, Command::DeleteCookies, data, {});
    return true;
}

bool MobileWebViewSession::handle_logic_command_DeleteCookiesWithCounter(const std::string& data)
{
    // The desktop waits for all deletions before closing; Android deletes them asynchronously
    // and the next page load happens after that anyway.
    m_host.webview_command(m_id, Command::DeleteCookies, data, {});
    return true;
}

bool MobileWebViewSession::handle_logic_command_DoReload(const std::string&)
{
    if (!m_logic->url().empty() && m_current_url.rfind(m_logic->url(), 0) == 0) {
        m_host.webview_command(m_id, Command::Reload, {}, {});
    } else {
        m_host.webview_command(m_id, Command::LoadUrl, m_logic->url(), {});
    }
    return true;
}

bool MobileWebViewSession::handle_logic_command_AddUserScript(const std::string& data)
{
    m_host.webview_command(m_id, Command::AddUserScript, data, {});
    return true;
}

bool MobileWebViewSession::handle_logic_command_LoadResourcesPage(const std::string& data)
{
    m_host.webview_command(m_id, Command::LoadUrl, resources_page_url(data), {});
    return true;
}

} // namespace Slic3r::App::Mobile
