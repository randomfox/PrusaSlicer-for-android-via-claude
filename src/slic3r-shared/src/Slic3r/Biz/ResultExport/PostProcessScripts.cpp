#include "Slic3r/Biz/ResultExport/PostProcessScripts.hpp"

#include "Slic3r/Exception.hpp"
#include "Slic3r/Log.hpp"

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>

#include <fmt/format.h>

#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/convert.hpp>
#else
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

#ifndef _WIN32
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
#endif

#ifdef _WIN32
/// Runs one script line as 2.x did on Windows: the first word is the program (.exe/.com run
/// directly, .bat/.cmd through cmd.exe, other files through their associated program, e.g.
/// python for .py), the rest are its arguments, then the G-code path. Returns the exit code;
/// the error output is collected in `std_err`.
int run_windows(const std::string& script, const boost::filesystem::path& gcode_path, const ScriptEnvironment& environment, std::string& std_err)
{
    const std::wstring script_w = boost::nowide::widen(script);
    int argc                    = 0;
    LPWSTR* argv                = ::CommandLineToArgvW(script_w.c_str(), &argc);
    if (argv == nullptr || argc <= 0) {
        if (argv != nullptr) {
            ::LocalFree(argv);
        }
        throw Slic3r::RuntimeError(fmt::format("Invalid post-processing script: {}", script));
    }
    std::vector<std::wstring> args(argv, argv + argc);
    ::LocalFree(argv);

    const std::wstring program = args.front();
    if (!boost::filesystem::exists(boost::filesystem::path(program))) {
        throw Slic3r::RuntimeError(fmt::format("The post-processing script {} does not exist.", boost::nowide::narrow(program)));
    }
    const auto quote = [](const std::wstring& arg) {
        std::wstring quoted = L"\"";
        for (wchar_t c : arg) {
            if (c == L'"') {
                quoted += L"\\";
            }
            quoted.push_back(c);
        }
        return quoted + L"\"";
    };
    std::wstring command_line;
    if (boost::iends_with(program, L".exe") || boost::iends_with(program, L".com")) {
        command_line = quote(program);
    } else if (boost::iends_with(program, L".bat") || boost::iends_with(program, L".cmd")) {
        command_line = L"cmd.exe /c " + quote(program);
    } else {
        wchar_t executable[MAX_PATH] = {0};
        if (reinterpret_cast<INT_PTR>(::FindExecutableW(program.c_str(), nullptr, executable)) <= 32 || executable[0] == 0) {
            throw Slic3r::RuntimeError(fmt::format(
                "No program is associated with the post-processing script {}.", boost::nowide::narrow(program)
            ));
        }
        command_line = quote(executable) + L" " + quote(program);
    }
    for (size_t i = 1; i < args.size(); ++i) {
        command_line += L" " + quote(args[i]);
    }
    command_line += L" " + quote(gcode_path.wstring());

    // Our environment plus the SLIC3R_* variables.
    std::wstring environment_block;
    if (LPWCH current = ::GetEnvironmentStringsW(); current != nullptr) {
        for (LPWCH p = current; *p != 0; p += wcslen(p) + 1) {
            environment_block.append(p).push_back(L'\0');
        }
        ::FreeEnvironmentStringsW(current);
    }
    for (const auto& [name, value] : environment) {
        environment_block.append(boost::nowide::widen(name + "=" + value)).push_back(L'\0');
    }
    environment_block.push_back(L'\0');

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        throw Slic3r::RuntimeError("Cannot run post-processing script: CreatePipe failed.");
    }
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError  = write_pipe;
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        environment_block.data(), nullptr, &startup, &process
    );
    ::CloseHandle(write_pipe);
    if (!created) {
        ::CloseHandle(read_pipe);
        throw Slic3r::RuntimeError(fmt::format("Cannot run post-processing script {} (error {}).", script, ::GetLastError()));
    }
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(read_pipe, buffer, sizeof buffer, &read, nullptr) && read > 0) {
        if (std_err.size() < 64 * 1024) {
            std_err.append(buffer, read);
        }
    }
    ::CloseHandle(read_pipe);
    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return int(exit_code);
}
#else
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
            std::string std_err;
#ifdef _WIN32
            SPDLOG_INFO("Running post-processing script: {}", script);
            const int result = run_windows(script, gcode_path, environment, std_err);
#else
            const std::string command = build_command(script, gcode_path);
            SPDLOG_INFO("Running post-processing script: {}", command);
            const int result = run_shell(command, environment, std_err);
#endif
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
        }
    }
}

} // namespace Slic3r::Biz::ResultExport
