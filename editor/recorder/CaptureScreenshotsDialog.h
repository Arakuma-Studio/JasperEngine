// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Record Input > Capture Screenshots... (docs/specs/input-recorder-and-input-scripts.md R38): the
// input file, the resolutions (the Game view's list as checkboxes, plus custom WxH), the output
// folder (default Recordings/Screenshots/<file stem>/) and Capture, which replays the file once per
// resolution writing its `shot` steps (InputCapture), with progress and Cancel, then opens the folder.

#include <set>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::recordinput {

    class CaptureScreenshotsDialog {
    public:
        static CaptureScreenshotsDialog& get();

        // opens it on that file (project-relative; "" keeps the last one)
        void open(const std::string& file);
        bool isOpen() const { return windowOpen; }
        // App, every frame
        void show(Project* project);

    private:
        bool windowOpen = false;
        std::string file;
        std::string output;             // "" until edited: the default follows the file
        std::set<std::string> sizes;    // the checked resolutions' names
        std::string custom;             // "1290x2796, 2048x2732"
        std::string message;
        bool messageIsError = false;
        bool capturing = false;

        void capture(Project* project);
    };

}
