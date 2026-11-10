// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R55)
// - Purpose: the character checks the export and check_title run. findTitleMissingCharacters: the
//   characters of a set that a title's font chain (the layer's font, its fallback_fonts, the
//   built-in face) cannot draw, W-TITLEART-MISSING-GLYPH naming the character, the title and the
//   font. scanTitleLuaLiterals: the string literals Lua scripts give a TitleArt (`setText("..")`,
//   `.text = ".."`, `setParameter("name", "..")`), and titleLiteralCharsWarnings their
//   characters outside a set, W-TITLEART-CHARS with file:line and the fix "add them to
//   Characters > Custom". The `characters` row of check_title's registry (TitleArtChecks.h) runs
//   both for one document.
// - Which layers show dynamic characters: the targets of the document's exposed `text`
//   parameters; a document without one: every text layer.
// - The Lua scan is best effort (R55, §9): a value is a TitleArt when the expression it was
//   assigned from calls `TitleArt(...)` or `:getTitleArt()` (`local t = TitleArt(scene, e)`,
//   `self.logo = go:getTitleArt()`), or the call chains on one (`go:getTitleArt().text = ".."`).
//   Escapes (\n, \ddd, \xXX, \u{XXX}, \z) are decoded; comments are skipped. A setParameter value
//   that is a colour ("#rrggbb") or a path is not text. Lua's own lexer (ModuleUsage.cpp's) is
//   file-local and keeps escapes undecoded, so this file has its own.
// - Thread-safety: pure functions; the provider and the registration are main thread.

#pragma once

#include "TitleCharacters.h"
#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class FontFace;
}

namespace jasperengine::titleart {
    class AssetResolver;
}

namespace jasperengine::editor {

    // the faces a text layer draws with, in order, and what each is called in a message
    struct TitleFontChain {
        std::vector<std::shared_ptr<FontFace>> faces;
        std::vector<std::string> names;             // assets-relative paths, "the built-in face"
        std::string missing;                        // a font of the chain that does not resolve
    };
    TitleFontChain titleLayerFontChain(const titleart::Json& layer, const titleart::AssetResolver& assets);
    // the code points of `chars` no face of `chain` has (white space, controls, joiners and
    // variation selectors draw nothing, so they are never missing)
    std::vector<uint32_t> titleChainMissing(const TitleFontChain& chain, const TitleCharacterSet& chars);

    struct TitleMissingCharacters {
        std::string code = "W-TITLEART-MISSING-GLYPH";
        std::string title;                          // the document, as the caller names it
        std::string layer;                          // the layer id
        std::string layerName;
        std::string font;                           // the layer's font (its chain in the message)
        std::vector<uint32_t> codepoints;
        std::string message;                        // one sentence with the fix
    };
    // `chars` against the font chain of each layer that shows dynamic characters
    std::vector<TitleMissingCharacters> findTitleMissingCharacters(const titleart::TitleArtDoc& doc, const TitleCharacterSet& chars,
                                                                   const titleart::AssetResolver& assets, const std::string& title);

    // the ids of the text layers that show dynamic characters (see the contract)
    std::vector<std::string> titleDynamicTextLayers(const titleart::TitleArtDoc& doc);

    struct TitleLuaLiteral {
        std::string file;                           // as the caller named the script
        int line = 0;
        std::string call;                           // "setText", "text =", "setParameter"
        std::string parameter;                      // the exposed name it sets ("text" for setText and text =)
        std::string value;                          // the decoded literal
    };
    std::vector<TitleLuaLiteral> scanTitleLuaLiterals(const std::string& source, const std::string& file);

    struct TitleCharsWarning {
        std::string code = "W-TITLEART-CHARS";
        std::string file;
        int line = 0;
        std::string parameter;
        std::vector<uint32_t> codepoints;           // outside the set, in order of appearance
        std::string message;                        // "<file>:<line>: ...: add them to Characters > Custom"
    };
    // the literals with characters outside `set` (white space and controls never count); `title`
    // names the set's document in the message
    std::vector<TitleCharsWarning> titleLiteralCharsWarnings(const std::vector<TitleLuaLiteral>& literals, const TitleCharacterSet& set,
                                                             const std::string& title);

    // the Lua literals of a project's scripts (every .lua under the project but hidden folders),
    // each file read again only when it changed
    std::vector<TitleLuaLiteral> collectTitleLuaLiterals(const std::filesystem::path& projectRoot);
    // what the `characters` check reads the literals from: collectTitleLuaLiterals by default;
    // tests and the exporter (which knows the shipped scripts) set their own; null restores it
    using TitleLuaLiteralProvider = std::function<std::vector<TitleLuaLiteral>(const std::filesystem::path& projectRoot)>;
    void setTitleLuaLiteralProvider(TitleLuaLiteralProvider provider);

    // registers check_title's `characters` check once (registerBuiltinTitleChecks calls it)
    void registerTitleCharactersCheck();

}
