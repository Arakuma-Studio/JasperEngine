// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The texture cook report (spec S14 R27, R29): per texture and family its format, dimensions,
// mips, VRAM, disk and source bytes, PSNR, cache hit and status, with totals per family. One
// form for the inspector's table, MCP's get_texture_cook_report, `cook-textures --report` and
// the export log. Keys are snake_case (MCP).

#pragma once

#include "texture/TextureCookQueue.h"

#include "mcp/McpJson.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TextureReportRow {
        std::string path;                   // asset-relative
        TextureImportType type = TextureImportType::Default;
        std::string inferredFrom;
        std::map<TextureFamily, TextureCookState> families;
    };

    class TextureCookReport {
    public:
        // a batch item as a state (Cooked, Failed or Source)
        static TextureCookState stateOf(const TextureBatchItem& item, const TextureCookInputs* inputs);

        static mcp::Json familyJson(const TextureCookState& state);
        // {"textures": [...], "totals": {family: {...}}}
        static mcp::Json toJson(const std::vector<TextureReportRow>& rows);
        // the table `cook-textures` prints and the export log keeps
        static std::string toText(const std::vector<TextureReportRow>& rows);

        // "2048×2048 · BC7 · 12 mips · VRAM 5.3 MB · Disk 3.9 MB (source 7.1 MB) · PSNR 43.1 dB"
        static std::string infoLine(const TextureCookState& state);
        static std::string formatBytes(uint64_t bytes);
        // problems `--check` and `check` fail on: failed cooks and R2 / R6 warnings
        static std::vector<std::string> problems(const std::vector<TextureReportRow>& rows);
    };

}
