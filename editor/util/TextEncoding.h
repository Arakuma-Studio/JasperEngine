// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace jasperengine::editor {

    // Text in the editor is UTF-8 in every std::string (docs/core-utilities.md, "Text encoding").
    // On Windows the process runs in the UTF-8 code page (engine/platform/win/jasperengine.manifest);
    // these are the edges where text from elsewhere comes in or goes out.
    class TextEncoding {
    public:
        // Why the editor cannot run in this active code page, or nothing for UTF-8 (65001)
        static std::optional<std::string> codePageProblem(unsigned activeCodePage);

        // Windows: false (after telling the user, on stderr or in a message box) when the process
        // is not in the UTF-8 code page: Windows before 10 1903 ignores the manifest. Elsewhere true.
        static bool checkWindowsCodePage();

        // Windows: the console this process writes to shows UTF-8 until the process exits (the
        // shell's code page comes back then). Elsewhere nothing.
        static void useUtf8Console();

        // A line a child program wrote: kept when it is UTF-8; otherwise, on Windows, read in the
        // system's console (OEM) code page, what cl, link and cmd write to a pipe; elsewhere
        // repaired, one U+FFFD per bad byte
        static std::string consoleToUtf8(std::string_view line);
    };

}
