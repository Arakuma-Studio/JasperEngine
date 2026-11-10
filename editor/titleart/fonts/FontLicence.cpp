// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontLicence.h"

#include "FontTables.h"

namespace jasperengine::editor::titlefonts {

    FontLicenceInfo classifyFsType(uint16_t fsType) {
        FontLicenceInfo info;
        info.hasOs2 = true;
        info.fsType = fsType;
        info.noSubsetting = (fsType & 0x0100) != 0;
        info.bitmapOnly = (fsType & 0x0200) != 0;
        const uint16_t usage = fsType & 0x000F;
        if (usage & 0x0008) info.embedding = EmbeddingLicence::Editable;
        else if (usage & 0x0004) info.embedding = EmbeddingLicence::PreviewAndPrint;
        else if (usage & 0x0002) info.embedding = EmbeddingLicence::Restricted;
        else info.embedding = EmbeddingLicence::Installable;
        return info;
    }

    FontLicenceInfo readFontLicence(const uint8_t* data, size_t size, int faceIndex) {
        const Os2Info os2 = fontOs2(data, size, faceIndex);
        if (!os2.present) return FontLicenceInfo();
        return classifyFsType(os2.fsType);
    }

    const char* embeddingLicenceLabel(EmbeddingLicence embedding) {
        switch (embedding) {
            case EmbeddingLicence::Restricted: return "Restricted";
            case EmbeddingLicence::PreviewAndPrint: return "Preview & Print";
            case EmbeddingLicence::Editable: return "Editable";
            case EmbeddingLicence::Installable: break;
        }
        return "Installable";
    }

    const char* restrictedLicenceQuestion() {
        return "This font's licence does not allow embedding it in a game. Exports of titles using it fail unless you confirm "
               "you hold a licence. Copy it anyway?";
    }

    const char* restrictedLicenceConfirm() { return "Copy Anyway"; }

    const char* restrictedLicenceCancel() { return "Cancel"; }

    const char* systemFontLicenceNote() { return "System font: check its licence before you ship a game with it"; }

}
