// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpLaunch.h"

#include "McpServer.h"
#include "McpToolGroups.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

#ifndef _WIN32
pid_t xvfbPid = -1;
#endif

std::string commandName(const char* executableName) {
    return fs::path(executableName ? executableName : "jasperengine-editor").filename().string();
}

void printUsage(const std::string& name) {
    std::cerr
        << "Usage:\n"
        << "  " << name << " mcp [options]\n\n"
        << "Starts the editor as an MCP server for an MCP client (Claude Code, Codex, ...), speaking\n"
        << "JSON-RPC on stdin/stdout. Everything else the editor prints goes to stderr. The editor\n"
        << "quits when stdin closes; it never saves on its own.\n\n"
        << "Options:\n"
        << "  -p, --project <path>   Project directory or project.yaml to open.\n"
        << "      --create <dir>     Create a new project in <dir> (missing or empty) and open it.\n"
        << "      --name <name>      Name of the --create project. Default: the directory name.\n"
        << "      --headless         Linux: with no DISPLAY, run on a private Xvfb server.\n"
        << "      --http-port <n>    Also serve MCP over HTTP on http://127.0.0.1:<n>/mcp.\n"
        << "      --tool-groups <g>  Tool groups to list besides core: a,b or all (default: Editor\n"
        << "                         Settings mcp.tool_groups). Every tool is callable either way.\n"
        << "      --ignore-lock      Open the project even when another editor has it open\n"
        << "                         (without it that fails with project_locked, exit code 3).\n"
        << "  -h, --help             Show this help.\n\n"
        << "Without --project or --create the last opened project opens, as in a normal start.\n\n"
        << "Client configuration (e.g. .mcp.json):\n"
        << "  {\"mcpServers\": {\"jasperengine\": {\"command\": \"" << name
        << "\", \"args\": [\"mcp\", \"--project\", \"/path/to/project\"]}}}\n";
}

bool parsePort(const std::string& text, int& port) {
    try {
        size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size() || value < 1 || value > 65535) return false;
        port = value;
        return true;
    } catch (...) {
        return false;
    }
}

// The protocol owns the original stdout and stdin; everything else that writes to
// stdout (Out, printf, Lua print) lands on stderr, and child processes (builds,
// Xvfb) get /dev/null as stdin so they can't eat requests.
bool takeOverStdio(McpLaunchOptions& options) {
    std::fflush(stdout);
#ifdef _WIN32
    const int outFd = _dup(_fileno(stdout));
    const int inFd = _dup(_fileno(stdin));
    if (outFd < 0 || inFd < 0) return false;
    _setmode(outFd, _O_BINARY);
    _setmode(inFd, _O_BINARY);
    // _dup makes inheritable handles: child processes (cmake, MSBuild nodes, a run player)
    // would keep the client's pipes open after the editor exits, so it never sees EOF
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(outFd)), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(inFd)), HANDLE_FLAG_INHERIT, 0);
    // fd 1 must stop being the protocol pipe, or every printf corrupts the stream. A GUI
    // process may have no valid stderr (_fileno(stderr) < 0): use NUL then.
    if (_fileno(stderr) < 0 || _dup2(_fileno(stderr), _fileno(stdout)) != 0) {
        const int nulOut = _open("NUL", _O_WRONLY);
        if (nulOut < 0 || _dup2(nulOut, _fileno(stdout)) != 0) return false;
        _close(nulOut);
    }
    SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
    const int nul = _open("NUL", _O_RDONLY);
    if (nul >= 0) {
        _dup2(nul, _fileno(stdin));
        _close(nul);
        SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdin))));
    }
#else
    const int outFd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    const int inFd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    if (outFd < 0 || inFd < 0) return false;
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return false;
    const int devNull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devNull >= 0) {
        dup2(devNull, STDIN_FILENO);
        close(devNull);
    }
    // A client that goes away must not kill the editor mid-write
    std::signal(SIGPIPE, SIG_IGN);
#endif
    options.stdioOutFd = outFd;
    options.stdioInFd = inFd;
    return true;
}

#ifdef __linux__
// Xvfb -displayfd writes the display number it picked once it accepts connections
bool startXvfb(std::string& error) {
    int fds[2];
    if (pipe(fds) != 0) {
        error = "pipe() failed";
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        error = "fork() failed";
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        // Dies with the editor even if it crashes
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        const int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
        }
        const std::string fd = std::to_string(fds[1]);
        execlp("Xvfb", "Xvfb", "-displayfd", fd.c_str(), "-screen", "0", "1600x900x24", "-nolisten", "tcp",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    xvfbPid = pid;

    std::string display;
    pollfd pfd{fds[0], POLLIN, 0};
    for (int waited = 0; waited < 15000;) {
        const int ready = poll(&pfd, 1, 250);
        if (ready > 0) {
            char buffer[32];
            const ssize_t n = read(fds[0], buffer, sizeof(buffer));
            if (n <= 0) break;
            display.append(buffer, static_cast<size_t>(n));
            if (display.find('\n') != std::string::npos) break;
        } else {
            waited += 250;
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) == pid) {
                xvfbPid = -1;
                break;
            }
        }
    }
    close(fds[0]);
    while (!display.empty() && (display.back() == '\n' || display.back() == '\r')) display.pop_back();
    if (display.empty()) {
        error = xvfbPid < 0
            ? "Xvfb could not start (is it installed? apt install xvfb), or set DISPLAY"
            : "Xvfb did not report a display";
        finishMcpCommand();
        return false;
    }
    setenv("DISPLAY", (":" + display).c_str(), 1);
    std::fprintf(stderr, "[mcp] headless: Xvfb on DISPLAY=:%s\n", display.c_str());
    return true;
}
#endif

} // namespace

int prepareMcpCommand(int argc, char** argv, const char* executableName) {
    const std::string name = commandName(executableName);
    McpLaunchOptions& options = mcpLaunchOptions();

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                std::cerr << name << " mcp: " << arg << " needs a value\n";
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string text;
        if (arg == "-h" || arg == "--help" || arg == "help") {
            printUsage(name);
            return 0;
        } else if (arg == "-p" || arg == "--project") {
            if (!value(text)) return 2;
            options.project = text;
        } else if (arg == "--create") {
            if (!value(text)) return 2;
            options.createDir = text;
        } else if (arg == "--name") {
            if (!value(text)) return 2;
            options.createName = text;
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--tool-groups") {
            if (!value(text)) return 2;
            std::string error;
            if (!parseToolGroupList(text, options.toolGroups, error)) {
                std::cerr << name << " mcp: --tool-groups: " << error << "\n";
                return 2;
            }
            options.toolGroupsSet = true;
        } else if (arg == "--ignore-lock") {
            options.ignoreLock = true;
        } else if (arg == "--http-port") {
            if (!value(text)) return 2;
            if (!parsePort(text, options.httpPort)) {
                std::cerr << name << " mcp: --http-port takes a port number (1-65535)\n";
                return 2;
            }
        } else {
            std::cerr << name << " mcp: unknown argument '" << arg << "'\n\n";
            printUsage(name);
            return 2;
        }
    }

    std::error_code ec;
    if (!options.project.empty() && !options.createDir.empty()) {
        std::cerr << name << " mcp: use --project or --create, not both\n";
        return 2;
    }
    if (!options.createName.empty() && options.createDir.empty()) {
        std::cerr << name << " mcp: --name goes with --create\n";
        return 2;
    }
    if (!options.project.empty()) {
        fs::path path = fs::absolute(options.project, ec).lexically_normal();
        if (fs::is_regular_file(path, ec) && path.filename() == "project.yaml") path = path.parent_path();
        if (!fs::is_regular_file(path / "project.yaml", ec)) {
            std::cerr << name << " mcp: no project at " << path.string() << " (project.yaml not found)\n";
            return 2;
        }
        options.project = path;
    }
    if (!options.createDir.empty()) {
        const fs::path path = fs::absolute(options.createDir, ec).lexically_normal();
        if (fs::exists(path, ec) && (!fs::is_directory(path, ec) || !fs::is_empty(path, ec))) {
            std::cerr << name << " mcp: --create needs a missing or empty directory: " << path.string() << "\n";
            return 2;
        }
        options.createDir = path;
    }

#ifdef __linux__
    // Xvfb only makes sense for the X11 editor: Windows and macOS have their own displays
    if (options.headless) {
        const char* display = std::getenv("DISPLAY");
        if (!display || !*display) {
            std::string error;
            if (!startXvfb(error)) {
                std::cerr << name << " mcp: --headless: " << error << "\n";
                return 1;
            }
        }
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
    }
#else
    if (options.headless) {
        std::cerr << "[mcp] --headless only applies on Linux (Xvfb); ignored here.\n";
    }
#endif

    if (!takeOverStdio(options)) {
        std::cerr << name << " mcp: could not set the standard streams aside\n";
        finishMcpCommand();
        return 1;
    }
    options.stdio = true;
    return -1;
}

void finishMcpCommand() {
#ifndef _WIN32
    if (xvfbPid > 0) {
        kill(xvfbPid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 40; i++) {
            if (waitpid(xvfbPid, &status, WNOHANG) == xvfbPid) {
                xvfbPid = -1;
                return;
            }
            usleep(50 * 1000);
        }
        kill(xvfbPid, SIGKILL);
        waitpid(xvfbPid, &status, 0);
        xvfbPid = -1;
    }
#endif
}

bool parseMcpPortFlag(int argc, char** argv) {
    std::string text;
    if (argc == 3 && std::strcmp(argv[1], "--mcp-port") == 0) {
        text = argv[2];
    } else if (argc == 2 && std::strncmp(argv[1], "--mcp-port=", 11) == 0) {
        text = argv[1] + 11;
    } else {
        return false;
    }
    int port = 0;
    if (!parsePort(text, port)) {
        std::cerr << commandName(argv[0]) << ": --mcp-port takes a port number (1-65535)\n";
        return false;
    }
    mcpLaunchOptions().httpPort = port;
    return true;
}

}
