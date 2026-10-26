// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's toolchain fingerprint. CMake gives this file JASPERENGINE_ABI_VERSION (the
// editor version), as it gives the engine's ToolchainFingerprint.cpp, so the engine ABI hash
// names this editor build; never in a unity batch (another file could include the header first).
#include "EditorFingerprint.h"

#include "Out.h"

using namespace jasperengine;

const ToolchainFingerprint& editor::editorToolchainFingerprint(){
    static constexpr ToolchainFingerprint fingerprint = currentToolchainFingerprint();
    return fingerprint;
}

const char* editor::editorAbiVersion(){
    return JASPERENGINE_ABI_VERSION;
}

bool editor::checkEditorEngineFingerprint(){
    const ToolchainFingerprint* engine = jasperengine_engineFingerprint();
    if (!engine) return true;
    const std::vector<FingerprintMismatch> mismatches = compare(editorToolchainFingerprint(), *engine);
    if (mismatches.empty()) return true;
    std::string fields;
    for (const FingerprintMismatch& mismatch : mismatches) {
        if (!fields.empty()) fields += "; ";
        fields += std::string(mismatch.field) + " engine=" + std::to_string(mismatch.library) + " editor=" + std::to_string(mismatch.editor);
    }
    Out::error("The editor and its engine library were built with different toolchains or engine builds (%s): C++ scripts "
               "may not load. Rebuild the editor.", fields.c_str());
    return false;
}
