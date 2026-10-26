// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Platform.h"
#include "Out.h"
#include "Backend.h"
#include "EditorHost.h"
#include "imgui.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace jasperengine;

int editor::Platform::width = 0;
int editor::Platform::height = 0;

editor::Platform::Platform(Project* project) : System() {
    this->project = project;
}

bool editor::Platform::setSizes(int width, int height){
    if (editor::Platform::width != width || editor::Platform::height != height){
        editor::Platform::width = width;
        editor::Platform::height = height;

        return true;
    }

    return false;
}

int editor::Platform::getScreenWidth(){
    return width;
}

int editor::Platform::getScreenHeight(){
    return height;
}

std::string editor::Platform::getAssetPath(){
    return project->getAssetsPath().string();
}

std::string editor::Platform::getLuaPath(){
    return project->getLuaPath().string();
}

std::string editor::Platform::getShaderPath(){
    // Compiled shaders are a build output, not a referenced asset
    return (project->getProjectPath() / "shaders").string();
}

std::string editor::Platform::getUserDataPath(){
    return project->getUserDataPath().string();
}

std::string editor::Platform::getImportPath(){
    return project->getImportPath().string();
}

sg_environment editor::Platform::getSokolEnvironment(){
    #if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)
    return Backend::getSokolEnvironment();
    #else
    return System::getSokolEnvironment();
    #endif
}

sg_swapchain editor::Platform::getSokolSwapchain(){
    #if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)
    return Backend::getSokolSwapchain();
    #else
    return System::getSokolSwapchain();
    #endif
}

bool editor::Platform::isTouchDevice(){
    return project->isSimulateTouch();
}

void editor::Platform::setClipboardText(const std::string& utf8){
    if (ImGui::GetCurrentContext()){
        ImGui::SetClipboardText(utf8.c_str());
    }else{
        System::setClipboardText(utf8);
    }
}

std::string editor::Platform::getClipboardText(){
    if (ImGui::GetCurrentContext()){
        const char* text = ImGui::GetClipboardText();
        return text ? std::string(text) : std::string();
    }
    return System::getClipboardText();
}

void editor::Platform::setMouseMode(MouseMode mode){
    Backend::setMouseMode(mode);
}

void editor::Platform::quit(){
    // stops play instead of closing the editor, deferred to not stop inside a script call
    editor::getEditorHost().enqueueMainThreadTask([this]() {
        project->stopActivePlay();
    });
}


#if defined(_WIN32)
namespace {
    std::wstring widen(const std::string& text){
        if (text.empty()) return std::wstring();
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
        return out;
    }

    // One argument as CommandLineToArgvW reads it back
    std::wstring quoteArgument(const std::wstring& argument){
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
        std::wstring out = L"\"";
        for (size_t i = 0; i < argument.size(); i++) {
            size_t backslashes = 0;
            while (i < argument.size() && argument[i] == L'\\') {
                backslashes++;
                i++;
            }
            if (i == argument.size()) {
                out.append(backslashes * 2, L'\\');
                break;
            }
            if (argument[i] == L'"') {
                out.append(backslashes * 2 + 1, L'\\');
            } else {
                out.append(backslashes, L'\\');
            }
            out.push_back(argument[i]);
        }
        out.push_back(L'"');
        return out;
    }
}
#endif

bool editor::Platform::launchDetached(const std::vector<std::string>& argv, std::string* error){
    if (argv.empty() || argv[0].empty()) {
        if (error) *error = "nothing to run";
        return false;
    }
#if defined(_WIN32)
    std::wstring commandLine;
    for (const std::string& argument : argv) {
        if (!commandLine.empty()) commandLine += L' ';
        commandLine += quoteArgument(widen(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &startup, &process)) {
        if (error) *error = "could not start " + argv[0] + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    std::vector<char*> args;
    for (const std::string& argument : argv) args.push_back(const_cast<char*>(argument.c_str()));
    args.push_back(nullptr);

    // exec failures come back through a close-on-exec pipe: nothing read = it started
    int status[2];
    if (pipe(status) != 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    fcntl(status[1], F_SETFD, FD_CLOEXEC);
    const pid_t child = fork();
    if (child < 0) {
        close(status[0]);
        close(status[1]);
        if (error) *error = std::strerror(errno);
        return false;
    }
    if (child == 0) {
        // a session of its own, reparented to init by the second fork: never a zombie of ours
        close(status[0]);
        setsid();
        const pid_t grandchild = fork();
        if (grandchild != 0) _exit(grandchild < 0 ? 1 : 0);
        const int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
        }
        execvp(args[0], args.data());
        const int code = errno;
        if (write(status[1], &code, sizeof(code)) < 0) {}
        _exit(127);
    }
    close(status[1]);
    int childStatus = 0;
    waitpid(child, &childStatus, 0);
    int code = 0;
    const ssize_t got = read(status[0], &code, sizeof(code));
    close(status[0]);
    if (got == static_cast<ssize_t>(sizeof(code))) {
        if (error) *error = "could not start " + argv[0] + ": " + std::strerror(code);
        return false;
    }
    return true;
#endif
}

bool editor::Platform::openWithSystemDefault(const std::string& path, std::string* error){
#if defined(_WIN32)
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        if (error) *error = "no program opens " + path;
        return false;
    }
    return true;
#elif defined(__APPLE__)
    return launchDetached({"open", path}, error);
#else
    return launchDetached({"xdg-open", path}, error);
#endif
}
