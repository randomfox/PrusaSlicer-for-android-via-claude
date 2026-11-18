#include "Slic3r/Biz/ResultExport/PostProcessScripts.hpp"

#include "Slic3r/Exception.hpp"
#include "Slic3r/Log.hpp"

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>

#include <fmt/format.h>

#include <fstream>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Slic3r::Biz::ResultExport {

namespace {

std::string shell_quote(const std::string& text)
{
    std::string quoted{"'"};
    for (char c : text) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

#ifdef __ANDROID__
constexpr const char* shell_path = "/system/bin/sh";
#else
constexpr const char* shell_path = "/bin/sh";
#endif

/// The command for one script line. On Android a script file is passed to the shell
/// (apps may not execute files they wrote), so only shell scripts work there.
std::string build_command(const std::string& script, const boost::filesystem::path& gcode_path)
{
    std::string command = script;
#ifdef __ANDROID__
    const std::string first_word = script.substr(0, script.find_first_of(" \t"));
    boost::system::error_code ec;
    if (!first_word.empty() && boost::filesystem::is_regular_file(first_word, ec)) {
        std::ifstream file(first_word);
        std::string shebang;
        std::getline(file, shebang);
        if (shebang.rfind("#!", 0) == 0 && shebang.find("sh") == std::string::npos) {
            throw Slic3r::RuntimeError(fmt::format(
                "Post-processing script {} needs \"{}\". Only shell scripts can run on Android.", first_word, shebang.substr(2)
            ));
        }
        command = std::string{shell_path} + " " + script;
    }
#endif
    return command + " " + shell_quote(gcode_path.string());
}

#ifndef _WIN32
/// Runs `command` with /bin/sh -c and the given environment added to ours. Returns the exit
/// code; the error output is collected in `std_err`.
int run_shell(const std::string& command, const ScriptEnvironment& environment, std::string& std_err)
{
    std::vector<std::string> env_storage;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        env_storage.emplace_back(*e);
    }
    for (const auto& [name, value] : environment) {
        env_storage.push_back(name + "=" + value);
    }
    std::vector<char*> envp;
    for (std::string& entry : env_storage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        throw Slic3r::RuntimeError(fmt::format("Cannot run post-processing script: pipe(): {}", std::strerror(errno)));
    }
    ::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    std::string shell{shell_path}, dash_c{"-c"}, cmd{command};
    char* argv[] = {shell.data(), dash_c.data(), cmd.data(), nullptr};
    pid_t pid      = 0;
    const int rc   = ::posix_spawn(&pid, shell_path, &actions, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    if (rc != 0) {
        ::close(pipe_fds[0]);
        throw Slic3r::RuntimeError(fmt::format("Cannot run post-processing script: {}", std::strerror(rc)));
    }

    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(pipe_fds[0], buffer, sizeof buffer);
        if (n > 0) {
            if (std_err.size() < 64 * 1024) {
                std_err.append(buffer, size_t(n));
            }
        } else if (n == 0 || errno != EINTR) {
            break;
        }
    }
    ::close(pipe_fds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}
#endif

} // namespace

void run_post_process_scripts(
    const std::vector<std::string>& scripts,
    const boost::filesystem::path& gcode_path,
    const ScriptEnvironment& environment
)
{
    for (const std::string& entry : scripts) {
        std::vector<std::string> lines;
        boost::split(lines, entry, boost::is_any_of("\r\n"));
        for (std::string script : lines) {
            boost::trim(script);
            if (script.empty()) {
                continue;
            }
#ifdef _WIN32
            throw Slic3r::RuntimeError("Post-processing scripts are not supported in this build.");
#else
            const std::string command = build_command(script, gcode_path);
            SPDLOG_INFO("Running post-processing script: {}", command);
            std::string std_err;
            const int result = run_shell(command, environment, std_err);
            boost::trim(std_err);
            if (result != 0) {
                throw Slic3r::RuntimeError(
                    std_err.empty() ? fmt::format("Post-processing script {} failed (exit code {}).", script, result) :
                                      fmt::format("Post-processing script {} failed (exit code {}):\n{}", script, result, std_err)
                );
            }
            if (!std_err.empty()) {
                SPDLOG_INFO("Post-processing script {} output: {}", script, std_err);
            }
            boost::system::error_code ec;
            if (!boost::filesystem::exists(gcode_path, ec)) {
                throw Slic3r::RuntimeError(fmt::format(
                    "Post-processing script {} removed the G-code file. Scripts must change the file in place.", script
                ));
            }
#endif
        }
    }
}

} // namespace Slic3r::Biz::ResultExport
