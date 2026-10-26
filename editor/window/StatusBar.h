// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <string>

namespace jasperengine::editor {

    // The bar along the bottom of the editor window. On its right, the file watcher (S05 R28):
    // "Watching (inotify)", "Polling 1 s", "Auto Refresh off: N changes", with the details in
    // its tooltip; a click opens Edit > Preferences > Asset Pipeline.
    class StatusBar {
    public:
        explicit StatusBar(std::function<void()> openAssetPipeline);
        void show();

        // A short message on the bar's left for `seconds` ("Preparing picking for Rock..."),
        // replacing the one shown
        static void setTransientMessage(const std::string& text, double seconds);
        static std::string getTransientMessage();

    private:
        std::function<void()> openAssetPipeline;
        static std::string transientText;
        static double transientUntil;
    };

}
