// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R58)
// - Purpose: what a font's OS/2 fsType says about embedding it in a game, read when the picker
//   copies a system font into the project, and the words the user sees about it. Restricted
//   License embedding (0x0002 without the Preview & Print or Editable bits) asks first; every
//   other system font is copied with a note. S81's export applies its own fsType rule and
//   acknowledgement (S81 AC15): this only tells the user early.
// - Classification: bits 0-3 of fsType; when an old font sets several, the least restrictive
//   wins (OpenType 1.8: Editable over Preview & Print over Restricted). A font without an OS/2
//   table is Installable (nothing restricts it).
// - Thread-safety: pure functions.

#pragma once

#include <cstddef>
#include <cstdint>

namespace jasperengine::editor::titlefonts {

    enum class EmbeddingLicence : uint8_t { Installable, Restricted, PreviewAndPrint, Editable };

    struct FontLicenceInfo {
        bool hasOs2 = false;
        uint16_t fsType = 0;
        EmbeddingLicence embedding = EmbeddingLicence::Installable;
        bool noSubsetting = false;      // 0x0100
        bool bitmapOnly = false;        // 0x0200
        // Restricted: ask (restrictedLicenceQuestion) before copying it
        bool asksBeforeCopy() const { return embedding == EmbeddingLicence::Restricted; }
    };

    FontLicenceInfo classifyFsType(uint16_t fsType);
    // the face's OS/2 fsType, classified (Installable without an OS/2 table)
    FontLicenceInfo readFontLicence(const uint8_t* data, size_t size, int faceIndex);

    // "Installable", "Restricted", "Preview & Print", "Editable"
    const char* embeddingLicenceLabel(EmbeddingLicence embedding);

    // R58's words: the question before a Restricted font is copied, its two answers, and the note
    // every copied system font gets (repeated in the Font section)
    const char* restrictedLicenceQuestion();
    const char* restrictedLicenceConfirm();     // "Copy Anyway"
    const char* restrictedLicenceCancel();      // "Cancel"
    const char* systemFontLicenceNote();

}
