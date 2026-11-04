// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorFilePicker.h"

#include "diag/HangWatchdog.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct PickerState {
        std::mutex mutex;
        EditorFilePicker::Host host;
        EditorFilePicker::Request request;
        std::string baseDirectory;
        bool native = false;
        bool answered = false;
        std::vector<std::string> paths;
        std::atomic<bool> open{false};
    };

    // Never destroyed, as McpServer: a transport thread may still answer through it during exit
    PickerState& picker() {
        static PickerState* instance = new PickerState();
        return *instance;
    }

    std::string lowered(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    const char* modeTitle(EditorFilePicker::Mode mode) {
        switch (mode) {
            case EditorFilePicker::Mode::OpenFile: return "Open File";
            case EditorFilePicker::Mode::OpenFiles: return "Open Files";
            case EditorFilePicker::Mode::OpenFolder: return "Choose Folder";
            case EditorFilePicker::Mode::OpenFolders: return "Choose Folders";
            case EditorFilePicker::Mode::SaveFile: return "Save File";
        }
        return "File Picker";
    }

    bool isFolderMode(EditorFilePicker::Mode mode) {
        return mode == EditorFilePicker::Mode::OpenFolder || mode == EditorFilePicker::Mode::OpenFolders;
    }

    std::string extensionList(const EditorFilePicker::Request& request) {
        std::string out;
        for (const std::string& extension : request.extensions) out += (out.empty() ? "" : ", ") + extension;
        return out;
    }

    // Empty when this path fits the request, or what is wrong with it
    std::string pathProblem(const EditorFilePicker::Request& request, const fs::path& path) {
        const std::string shown = FileUtils::pathToUtf8(path);
        std::error_code ec;
        if (isFolderMode(request.mode)) {
            return fs::is_directory(path, ec) ? std::string() : shown + " is not a folder.";
        }
        if (fs::is_directory(path, ec)) return shown + " is a folder; this picker takes a file.";
        if (request.mode == EditorFilePicker::Mode::SaveFile) {
            const fs::path parent = path.parent_path();
            if (!parent.empty() && !fs::is_directory(parent, ec)) return "The folder of " + shown + " does not exist.";
        } else if (!fs::exists(path, ec)) {
            return shown + " does not exist.";
        }
        if (!request.extensions.empty()) {
            std::string extension = lowered(FileUtils::pathToUtf8(path.extension()));
            if (!extension.empty() && extension[0] == '.') extension.erase(0, 1);
            if (std::find(request.extensions.begin(), request.extensions.end(), extension) == request.extensions.end()) {
                return shown + " is not one of the types this picker takes: " + extensionList(request) + ".";
            }
        }
        return {};
    }

    void open(PickerState& state, const EditorFilePicker::Request& request, bool native, std::string baseDirectory) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.request = request;
        state.baseDirectory = std::move(baseDirectory);
        state.native = native;
        state.answered = false;
        state.paths.clear();
        state.open = true;
    }

}

void EditorFilePicker::setHost(Host host) {
    PickerState& state = picker();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.host = std::move(host);
}

std::optional<std::vector<std::string>> EditorFilePicker::ask(const Request& request) {
    PickerState& state = picker();
    Host host;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        host = state.host;
    }
    const Route route = host.route ? host.route() : Route::Native;
    if (route == Route::Native) return std::nullopt;
    if (route == Route::Cancel) return std::vector<std::string>();

    open(state, request, false, host.baseDirectory ? host.baseDirectory() : std::string());
    if (host.opened) host.opened();
    // as long as the agent wants, like the native dialog it stands for
    const diag::BusyScope busy("File picker waiting for an agent", 0);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.answered) {
                state.open = false;
                return state.paths;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

EditorFilePicker::NativeScope::NativeScope(const Request& request) {
    PickerState& state = picker();
    open(state, request, true, std::string());
    Host host;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        host = state.host;
    }
    if (host.opened) host.opened();
}

EditorFilePicker::NativeScope::~NativeScope() {
    picker().open = false;
}

bool EditorFilePicker::isOpen() {
    return picker().open;
}

bool EditorFilePicker::info(EditorDialogInfo& out) {
    PickerState& state = picker();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.open) return false;
    out = EditorDialogInfo();
    out.kind = "file_picker";
    out.title = modeTitle(state.request.mode);
    if (state.native) {
        out.message = "A native file picker is open in the editor window, and the editor waits for it: only a human can "
                      "answer it there. In a --headless editor agents answer pickers with respond_dialog.";
        return true;
    }
    const Mode mode = state.request.mode;
    const bool several = mode == Mode::OpenFiles || mode == Mode::OpenFolders;
    out.message = std::string("The editor waits for ") +
        (mode == Mode::OpenFile ? "a file to open" : mode == Mode::OpenFiles ? "the files to open" :
         mode == Mode::OpenFolder ? "a folder" : mode == Mode::OpenFolders ? "the folders" : "where to save the file");
    if (!state.request.extensions.empty()) out.message += " (" + extensionList(state.request) + ")";
    if (!state.request.defaultPath.empty()) out.message += ", starting in " + state.request.defaultPath;
    if (!state.request.defaultName.empty()) out.message += ", named " + state.request.defaultName;
    out.message += several ? ". Several: values.paths." : ".";
    out.choices = {"ok", "cancel"};
    out.fields.push_back({"path", "path", EditorDialogValue::ofText(state.request.defaultName), {}});
    return true;
}

bool EditorFilePicker::answer(const std::vector<std::string>& paths, std::string& error) {
    PickerState& state = picker();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.open || state.native) {
        error = state.open ? "This native file picker can only be answered by a human in the editor window." : "No file picker is open.";
        return false;
    }
    if (paths.empty()) {
        error = "Give the path in values.path, or answer with choice cancel.";
        return false;
    }
    if (paths.size() > 1 && state.request.mode != Mode::OpenFiles && state.request.mode != Mode::OpenFolders) {
        error = "This picker takes one path.";
        return false;
    }
    std::vector<std::string> resolved;
    for (const std::string& given : paths) {
        fs::path path = FileUtils::pathFromUtf8(given);
        if (path.is_relative() && !state.baseDirectory.empty()) path = FileUtils::pathFromUtf8(state.baseDirectory) / path;
        error = pathProblem(state.request, path);
        if (!error.empty()) return false;
        resolved.push_back(FileUtils::pathToUtf8(path.lexically_normal()));
    }
    state.paths = std::move(resolved);
    state.answered = true;
    return true;
}

bool EditorFilePicker::cancel(std::string& error) {
    PickerState& state = picker();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.open || state.native) {
        error = state.open ? "This native file picker can only be answered by a human in the editor window." : "No file picker is open.";
        return false;
    }
    state.paths.clear();
    state.answered = true;
    return true;
}

void EditorFilePicker::abandon() {
    PickerState& state = picker();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.open || state.native) return;
    state.paths.clear();
    state.answered = true;
}
