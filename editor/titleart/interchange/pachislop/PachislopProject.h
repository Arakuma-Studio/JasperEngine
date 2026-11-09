// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/io/project.ts (parseProjectFile, fromBase64) by SafEight, used with
// the author's permission.

// Contract (docs/specs/S87-title-localisation-interchange.md R28)
// - Purpose: a `.pslot.json` file read as Pachislop does: `format: "pslot"`, the project with
//   its layers and comp, `version` 4-11 and equal to `project.version` (versions 1-3 get
//   Pachislop's own message, a newer one `pachislop_version_newer`), the embedded fonts and
//   assets decoded from base64, then the project migrated to v11 (PachislopMigrate.h).
// - Untrusted input: at most 256 MiB of JSON, 64 MiB per decoded asset (larger ones are left
//   out with a note), 4,096 layers; every failure is a sentence, never a crash.
// - Thread-safety: pure functions.

#pragma once

#include "PachislopJson.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine::editor::pachislop {

    constexpr size_t MAX_FILE_BYTES = 256u << 20;
    constexpr size_t MAX_ASSET_BYTES = 64u << 20;
    constexpr size_t MAX_LAYERS = 4096;

    struct Blob {
        std::string id;
        std::string name;
        std::string mime;
        std::string data;           // decoded bytes
    };

    struct Bundle {
        Json project;               // migrated to v11
        std::vector<Blob> fonts;
        std::vector<Blob> assets;
        int version = 0;            // the version the file was saved as
        std::vector<std::string> notes;   // the upgrade note, blobs left out
    };

    // false with `code` (not_json, not_pachislop, pachislop_version_unsupported,
    // pachislop_version_newer, pachislop_too_large) and Pachislop's sentence
    bool parseProjectFile(const std::string& text, Bundle& out, std::string& code, std::string& error);
    // the start of a file is a Pachislop project (a JSON object whose `format` is "pslot")
    bool looksLikeProjectFile(const std::string& head);
    // RFC 4648 base64 (whitespace ignored); false for any other character or a bad length
    bool decodeBase64(const std::string& text, std::string& out, size_t limit = MAX_ASSET_BYTES);

}
