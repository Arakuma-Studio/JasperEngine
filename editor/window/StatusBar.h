// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>

namespace jasperengine::editor {

    // The bar along the bottom of the editor window. On its right, the file watcher (S05 R28):
    // "Watching (inotify)", "Polling 1 s", "Auto Refresh off: N changes", with the details in
    // its tooltip; a click opens Edit > Preferences > Asset Pipeline.
    class StatusBar {
    public:
        explicit StatusBar(std::function<void()> openAssetPipeline);
        void show();

    private:
        std::function<void()> openAssetPipeline;
    };

}
