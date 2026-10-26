// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "script/ToolchainFingerprint.h"

namespace jasperengine::editor {

    // What a C++ script library loaded into this editor must match (S23 R14-R16)
    const ToolchainFingerprint& editorToolchainFingerprint();
    // The engine build name the generated script sources embed (JASPERENGINE_ABI_VERSION)
    const char* editorAbiVersion();
    // At startup: the editor's fill equals the engine library's (R14); logs the fields when not
    bool checkEditorEngineFingerprint();

}
