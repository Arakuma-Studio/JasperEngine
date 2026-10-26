// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The crash handler's backend on Windows, Linux, Android, macOS and iOS while their native
// backends are not part of the build (docs/crash-diagnostics.md, "Platform status"): it arms
// nothing.
// install() still prepares report folders, the session marker, the crash ring and retention,
// so abnormal exits are detected and listed, and the process keeps the handler it already had
// (the editor's editor/util/CrashHandler, which appends the crash to editor.log).

#include "detail/CrashState.h"

#if !defined(__EMSCRIPTEN__)

namespace jasperengine::diag::crash::detail {

    bool installPlatform(const Config&) {
        return false;
    }

    void prepareThreadPlatform() {
    }

    bool platformDebugCrash(std::string_view) {
        return false;
    }

}

#endif
