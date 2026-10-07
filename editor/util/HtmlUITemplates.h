// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>

namespace jasperengine::editor {

    // Assets > Create > HTML Document: a document and its stylesheet, written next to each
    // other. No ImGui or project state, so the HTML UI tests check it (EditorLogicTests).
    struct HtmlUITemplates {
        // First free "<baseName>[N]" in `directory` for which neither .html nor .css exists
        static std::string uniqueDocumentName(const std::filesystem::path& directory, const std::string& baseName = "NewDocument");
        // A <link> to the stylesheet, a body with one heading and one button
        static std::string documentText(const std::string& name);
        // RCSS that RmlUi 6.3 accepts as it is (and standard CSS a browser shows the same way)
        static std::string styleSheetText();
        // Writes <name>.html and <name>.css; false (and error) when a file cannot be written
        static bool createDocument(const std::filesystem::path& directory, const std::string& name, std::string& error);
    };

}
