// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Android, iOS and the web (S05 R20): nothing is watched and nothing runs.

#include "Backends.h"

using namespace jasperengine;

namespace fs = std::filesystem;

namespace jasperengine::watch {

    class NullBackend : public FileWatcher {
    public:
        bool start(const fs::path&, WatchSink&, std::string& error) override {
            error = "file watching is not available on this platform";
            return false;
        }
        void stop() override {}
        bool flush(std::chrono::milliseconds) override { return false; }
        const char* name() const override { return "null"; }
        bool isNative() const override { return false; }
        size_t watchCount() const override { return 0; }
        void injectOverflowForTesting() override {}
    };

}

std::unique_ptr<FileWatcher> jasperengine::watch::createNullBackend() {
    return std::make_unique<NullBackend>();
}
